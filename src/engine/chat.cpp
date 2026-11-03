#include "chat.h"
#include "bans.h"
#include "dom_engine.h"
#include "items.h"
#include "native_call.h"
#include "process_event.h"
#include "../utils/logger.h"
#include "../utils/memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

namespace DomChat {

namespace {

using namespace DomEngine;

// FChatMessageData - Source: RSDWSDK/CppSDK/Assertions.inl
constexpr uintptr_t kSenderId      = 0x00;
constexpr uintptr_t kSecondaryId   = 0x30;
constexpr uintptr_t kCharacterGuid = 0x60;
constexpr uintptr_t kPlayerId      = 0x70;
constexpr uintptr_t kColor         = 0x74;
constexpr uintptr_t kMessageBody   = 0x78;
constexpr size_t    kMessageSize   = 0x88;

// FChatPlayerEventData - Source: RSDWSDK/CppSDK/SDK/JagexChatBackend_structs.hpp
constexpr uintptr_t kEventTag  = 0x78;
constexpr size_t    kEventSize = 0x80;

// ADominionPlayerController::PlayerChatComponent.
//
// This used to be a single hardcoded offset (0x1288). After the 1.0 / 10-14 patches that
// slot holds BuildInteractionComponent instead, so Client_ReceiveChatMessage was being
// called on the wrong component and broadcasts silently went nowhere (issues #5, #6).
// The offset is now discovered at runtime per controller class and every read is
// validated with IsA("PlayerChatComponent"). These are only hints, tried first because
// they are cheap - a hint that does not validate is ignored, never trusted.
constexpr uintptr_t kChatComponentHints[] = {0x12E0, 0x1288};
constexpr uintptr_t kControllerScanStart = 0x228;  // past AActor's own members
constexpr uintptr_t kControllerScanLimit = 0x4000;  // sanity cap on the reflected size
constexpr const char* kChatComponentClass = "PlayerChatComponent";

typedef void (*ExecThunk)(void* context, void* frame, void* result);

constexpr uintptr_t kExecFunction = 0xD8;

ExecThunk g_OriginalSend = nullptr;
uintptr_t g_SendFunction = 0;
std::atomic<uintptr_t> g_ReceiveFunction{0};
std::atomic<uintptr_t> g_AdminFunction{0};
std::atomic<uintptr_t> g_AdminLibrary{0};
uintptr_t g_ClientEventFunction = 0;
uintptr_t g_ServerEventFunction = 0;
uintptr_t g_SystemMessageFunction = 0;
std::atomic<bool> g_Hooked{false};
std::string g_Status = "not initialised";
int g_InitAttempts = 0;

// Late-registered classes (PrivilegeFunctionLibrary in particular) may not exist yet when
// Initialize() first runs, so anything missing is looked up again on a timer.
std::mutex g_LateMutex;
std::chrono::steady_clock::time_point g_LastLateResolve{};
constexpr auto kLateResolveInterval = std::chrono::seconds(15);

// Discovered PlayerChatComponent offset per controller class.
std::mutex g_ComponentMutex;
std::map<uintptr_t, uintptr_t> g_ChatOffsetByClass;
std::map<uintptr_t, std::chrono::steady_clock::time_point> g_ChatDiscoveryFailedAt;
constexpr auto kDiscoveryRetry = std::chrono::seconds(15);

std::mutex g_ListenerMutex;
std::vector<Listener> g_Listeners;
std::vector<EventListener> g_EventListeners;

std::mutex g_VTableMutex;
std::map<uintptr_t, GameThread::ProcessEventFn> g_HookedVTables;
std::map<std::string, std::chrono::steady_clock::time_point> g_RecentEvents;

void WriteFString(uintptr_t dest, const std::string& value) {
    int32_t count = (int32_t)value.size() + 1;
    char16_t* buffer = new char16_t[(size_t)count];
    for (size_t i = 0; i < value.size(); i++) buffer[i] = (char16_t)(unsigned char)value[i];
    buffer[value.size()] = 0;

    *(char16_t**)(dest) = buffer;
    *(int32_t*)(dest + 8) = count;
    *(int32_t*)(dest + 12) = count;
}

void Deliver(const Message& message) {
    std::vector<Listener> listeners;
    {
        std::lock_guard<std::mutex> lock(g_ListenerMutex);
        listeners = g_Listeners;
    }
    for (const Listener& listener : listeners) {
        try {
            listener(message);
        } catch (...) {
        }
    }
}

void DeliverEvent(const PlayerEvent& event) {
    std::vector<EventListener> listeners;
    {
        std::lock_guard<std::mutex> lock(g_ListenerMutex);
        listeners = g_EventListeners;
    }
    for (const EventListener& listener : listeners) {
        try {
            listener(event);
        } catch (...) {
        }
    }
}

// A live UObject: aligned, readable, vtable inside the game image.
bool LooksLikeObject(uintptr_t candidate) {
    if (!candidate || (candidate & 7) != 0) return false;
    if (!Mem::Readable((void*)candidate, 0x30)) return false;
    return Mem::InImage((void*)Mem::ReadPtr(candidate));
}

bool IsChatComponent(uintptr_t candidate) {
    Engine* engine = g_Engine;
    return engine && LooksLikeObject(candidate) && engine->IsA(candidate, kChatComponentClass);
}

// The component must belong to this controller: a PlayerChatComponent is a default
// subobject, so its Outer is the controller that owns it. This is what stops a scan from
// ever settling on another player's component.
bool IsOwnedChatComponent(uintptr_t candidate, uintptr_t controller) {
    return IsChatComponent(candidate) &&
           Mem::ReadPtr(candidate + Offsets::UObject_Outer) == controller;
}

// How far the controller's own members extend, from its class's reflected PropertiesSize.
// Scanning further would read whatever object the allocator placed next to it.
uintptr_t ControllerScanEnd(uintptr_t controller) {
    uintptr_t cls = Mem::ReadPtr(controller + Offsets::UObject_Class);
    int32_t size = cls ? Mem::ReadI32(cls + Offsets::UStruct_PropertiesSize) : 0;
    if (size <= (int32_t)kControllerScanStart || size > (int32_t)kControllerScanLimit) return 0;
    return (uintptr_t)size & ~(uintptr_t)7;
}

// Finds the controller-relative offset of its PlayerChatComponent pointer. Hints first,
// then a scan of the controller's members. Returns 0 when nothing validates or when more
// than one distinct owned component turns up - an ambiguous answer is worse than none.
uintptr_t DiscoverChatOffset(uintptr_t controller, std::string& outVia) {
    Engine* engine = g_Engine;
    if (!engine || !controller) return 0;

    for (uintptr_t hint : kChatComponentHints) {
        if (IsOwnedChatComponent(Mem::ReadPtr(controller + hint), controller)) {
            outVia = "hint";
            return hint;
        }
    }

    uintptr_t end = ControllerScanEnd(controller);
    if (!end) {
        LogMessage("Chat: " + engine->GetObjectClassName(controller) +
                   " has no usable PropertiesSize, skipping the member scan");
        return 0;
    }
    if (!Mem::Readable((void*)(controller + kControllerScanStart), end - kControllerScanStart)) return 0;

    uintptr_t foundOffset = 0, foundValue = 0;
    for (uintptr_t offset = kControllerScanStart; offset + 8 <= end; offset += 8) {
        uintptr_t value = *(const uintptr_t*)(controller + offset);
        if (!IsOwnedChatComponent(value, controller)) continue;
        if (foundValue && foundValue != value) {
            LogMessage("Chat: several PlayerChatComponents on " + engine->GetObjectClassName(controller) +
                       " (+" + HexString(foundOffset) + ", +" + HexString(offset) + "), refusing to guess");
            return 0;
        }
        if (!foundValue) { foundOffset = offset; foundValue = value; }
    }
    if (!foundValue) return 0;

    outVia = "member scan";
    return foundOffset;
}

// Last resort: walk the object registry for the component owned by this controller and
// work back to the member offset that points at it.
uintptr_t DiscoverChatOffsetFromRegistry(uintptr_t controller) {
    Engine* engine = g_Engine;
    if (!engine) return 0;

    uintptr_t match = 0;
    int32_t count = engine->GetObjectCount();
    for (int32_t i = 0; i < count; i++) {
        uintptr_t object = engine->GetObjectByIndex(i);
        if (!object) continue;
        if (Mem::ReadPtr(object + Offsets::UObject_Outer) != controller) continue;
        if (!IsChatComponent(object)) continue;
        if (match && match != object) return 0;  // ambiguous
        match = object;
    }
    if (!match) return 0;

    // Matching one exact, ownership-checked address, so a fixed window is safe here even
    // when the reflected size is unavailable - the first hit is inside the controller.
    uintptr_t end = ControllerScanEnd(controller);
    if (!end) end = 0x2000;
    for (uintptr_t offset = 0x28; offset + 8 <= end; offset += 8) {
        if (Mem::ReadPtr(controller + offset) == match) return offset;
    }
    return 0;
}

// The validated PlayerChatComponent for a controller, or 0. The discovered offset is cached
// per controller class and re-validated on every call, so a patch that moves the member
// costs one rediscovery rather than misdirected RPCs.
uintptr_t ResolveChatComponent(uintptr_t controller) {
    Engine* engine = g_Engine;
    if (!engine || !controller || !LooksLikeObject(controller)) return 0;

    uintptr_t controllerClass = Mem::ReadPtr(controller + Offsets::UObject_Class);
    if (!controllerClass) return 0;

    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_ComponentMutex);
        auto cached = g_ChatOffsetByClass.find(controllerClass);
        if (cached != g_ChatOffsetByClass.end()) {
            uintptr_t component = Mem::ReadPtr(controller + cached->second);
            if (IsOwnedChatComponent(component, controller)) return component;
            // Null means this controller has not created its component yet (a player who is
            // still joining) - not a sign the offset is wrong. Anything else is.
            if (!component) return 0;
            LogMessage("Chat: cached PlayerChatComponent offset " + HexString(cached->second) +
                       " no longer validates, rediscovering");
            g_ChatOffsetByClass.erase(cached);
        }
        auto failed = g_ChatDiscoveryFailedAt.find(controllerClass);
        if (failed != g_ChatDiscoveryFailedAt.end() && now - failed->second < kDiscoveryRetry) {
            return 0;
        }
    }

    std::string via;
    uintptr_t offset = DiscoverChatOffset(controller, via);
    if (!offset) {
        // Mem::Readable works from a cached /proc/self/maps that is re-read at most every
        // 100ms, so a controller in freshly grown heap (a player who just joined) can look
        // unmapped for a moment. Refresh once before believing a negative, since a failure
        // backs off discovery for the whole controller class.
        Mem::Refresh();
        offset = DiscoverChatOffset(controller, via);
    }
    // The registry sweep walks every UObject. Never on the game thread, where it would be a
    // visible hitch - the main loop's Poll() gets there within two seconds anyway.
    if (!offset && !GameThread::IsGameThread()) {
        offset = DiscoverChatOffsetFromRegistry(controller);
        if (offset) via = "object registry";
    }
    if (!offset && GameThread::IsGameThread()) return 0;  // leave the verdict to Poll()

    std::lock_guard<std::mutex> lock(g_ComponentMutex);
    if (!offset) {
        g_ChatDiscoveryFailedAt[controllerClass] = now;
        LogMessage("Chat: no PlayerChatComponent found on " + engine->GetObjectClassName(controller) +
                   "; chat delivery to this player is unavailable (retrying in 15s)");
        return 0;
    }

    g_ChatDiscoveryFailedAt.erase(controllerClass);
    g_ChatOffsetByClass[controllerClass] = offset;
    uintptr_t component = Mem::ReadPtr(controller + offset);
    LogMessage("Chat: PlayerChatComponent at " + engine->GetObjectClassName(controller) + " +" +
               HexString(offset) + " via " + via + " (" + engine->GetObjectClassName(component) + ")");
    return component;
}

void ResolveLateFunctions(bool force) {
    Engine* engine = g_Engine;
    if (!engine || !engine->IsInitialized()) return;

    std::lock_guard<std::mutex> lock(g_LateMutex);
    auto now = std::chrono::steady_clock::now();
    if (!force && now - g_LastLateResolve < kLateResolveInterval) return;
    g_LastLateResolve = now;

    if (!g_ReceiveFunction) {
        g_ReceiveFunction = engine->FindFunction("PlayerChatComponent", "Client_ReceiveChatMessage");
        if (g_ReceiveFunction) LogMessage("Chat: Client_ReceiveChatMessage resolved, broadcast available");
    }
    if (!g_AdminFunction) {
        g_AdminFunction = engine->FindFunction("PrivilegeFunctionLibrary", "ControllerIsOwnerOrAdmin");
    }
    if (!g_AdminLibrary) {
        g_AdminLibrary = engine->FindObject("PrivilegeFunctionLibrary", true);
        if (g_AdminFunction && g_AdminLibrary) LogMessage("Chat: admin check resolved, chat commands available");
    }
    if (!g_ClientEventFunction) {
        g_ClientEventFunction = engine->FindFunction("PlayerChatComponent", "Client_ReceivePlayerEvent");
    }
    if (!g_ServerEventFunction) {
        g_ServerEventFunction = engine->FindFunction("PlayerChatComponent", "Server_SendPlayerEvent");
    }
    if (!g_SystemMessageFunction) {
        g_SystemMessageFunction = engine->FindFunction("PlayerChatComponent", "Client_ReceiveSystemMessage");
    }
}

void HandlePlayerEvent(uintptr_t params) {
    Engine* engine = g_Engine;
    if (!engine || !Mem::Readable((void*)params, kEventSize)) return;

    PlayerEvent event;
    uint32_t tagIndex = (uint32_t)Mem::ReadI32(params + kEventTag);
    uint32_t tagNumber = (uint32_t)Mem::ReadI32(params + kEventTag + 4);
    event.tag = engine->NameToString(tagIndex, tagNumber);
    if (event.tag.empty() || event.tag == "None") return;

    event.characterGuid = engine->ReadGuid(params + kCharacterGuid);
    event.playerId = Mem::ReadI32(params + kPlayerId);
    std::string source;
    event.senderNetId = engine->ReadUniqueNetId(params + kSenderId, source);

    std::string key = std::to_string(event.playerId) + "|" + event.characterGuid + "|" + event.tag;
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_VTableMutex);
        auto it = g_RecentEvents.find(key);
        if (it != g_RecentEvents.end() && now - it->second < std::chrono::seconds(3)) return;
        g_RecentEvents[key] = now;
        for (auto entry = g_RecentEvents.begin(); entry != g_RecentEvents.end();) {
            if (now - entry->second > std::chrono::seconds(30)) entry = g_RecentEvents.erase(entry);
            else ++entry;
        }
    }

    for (const PlayerInfo& player : engine->GetAllPlayers()) {
        if (player.playerId != event.playerId &&
            (event.characterGuid.empty() || player.characterGuid != event.characterGuid)) continue;
        event.senderName = player.characterName.empty() ? player.name : player.characterName;
        if (event.senderNetId.empty()) event.senderNetId = player.uniqueNetId;
        break;
    }

    LogMessage("Chat: player event '" + event.tag + "' from " +
               (event.senderName.empty() ? "playerId " + std::to_string(event.playerId) : event.senderName));
    DeliverEvent(event);
}

void HookedChatProcessEvent(void* self, void* function, void* params) {
    GameThread::ProcessEventFn previous = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_VTableMutex);
        uintptr_t vtable = Mem::ReadPtr((uintptr_t)self);
        auto it = g_HookedVTables.find(vtable);
        if (it != g_HookedVTables.end()) previous = it->second;
    }

    uintptr_t fn = (uintptr_t)function;
    if (fn && (fn == g_ClientEventFunction || fn == g_ServerEventFunction)) {
        HandlePlayerEvent((uintptr_t)params);
    } else if (fn && fn == g_SystemMessageFunction && g_Engine && Mem::Readable(params, 8)) {
        uint32_t tagIndex = (uint32_t)Mem::ReadI32((uintptr_t)params);
        uint32_t tagNumber = (uint32_t)Mem::ReadI32((uintptr_t)params + 4);
        LogMessage("Chat: system message '" + g_Engine->NameToString(tagIndex, tagNumber) + "'");
    }

    if (previous) previous(self, function, params);
}

bool SendToPlayer(const PlayerInfo& player, const std::string& body) {
    uintptr_t receive = g_ReceiveFunction;
    if (!receive || !GameThread::HasProcessEvent()) return false;
    if (!player.controllerPtr || !player.playerStatePtr) return false;

    uintptr_t component = ResolveChatComponent(player.controllerPtr);
    if (!component || !Mem::Readable((void*)component, 0x150)) return false;

    uint8_t params[kMessageSize + 16];
    memset(params, 0, sizeof(params));

    uintptr_t netId = player.playerStatePtr + Offsets::PlayerState_UniqueID;
    if (Mem::Readable((void*)netId, Offsets::NetIdRepl_Size)) {
        memcpy(params + kSenderId, (void*)netId, Offsets::NetIdRepl_Size);
    }
    uintptr_t secondary = player.playerStatePtr + Offsets::DomPlayerState_NetIdSecondary;
    if (Mem::Readable((void*)secondary, Offsets::NetIdRepl_Size)) {
        memcpy(params + kSecondaryId, (void*)secondary, Offsets::NetIdRepl_Size);
    }
    uintptr_t guid = player.playerStatePtr + Offsets::DomPlayerState_CharacterGuid;
    if (Mem::Readable((void*)guid, 16)) {
        memcpy(params + kCharacterGuid, (void*)guid, 16);
    }
    *(int32_t*)(params + kPlayerId) = player.playerId;
    *(uint32_t*)(params + kColor) = 0xFFFFFFFF;
    WriteFString((uintptr_t)params + kMessageBody, body);

    GameThread::CallFunction(component, receive, params);
    return true;
}

bool IsAdmin(uintptr_t controller) {
    if (!controller) return false;
    if (!g_AdminFunction || !g_AdminLibrary) ResolveLateFunctions(false);

    uintptr_t function = g_AdminFunction;
    uintptr_t library = g_AdminLibrary;
    if (!function || !library) return false;
    if (!NativeCall::IsNative(function)) return false;

    struct Params {
        uintptr_t Controller;
        bool ReturnValue;
        uint8_t Pad[7];
    } params = {};
    params.Controller = controller;

    if (!NativeCall::Invoke(library, function, &params, 0x08)) return false;
    return params.ReturnValue;
}

std::vector<std::string> Split(const std::string& line) {
    std::istringstream stream(line);
    std::vector<std::string> parts;
    std::string part;
    while (stream >> part) parts.push_back(part);
    return parts;
}

std::string Join(const std::vector<std::string>& parts, size_t from) {
    std::string out;
    for (size_t i = from; i < parts.size(); i++) {
        if (!out.empty()) out += " ";
        out += parts[i];
    }
    return out;
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return value;
}

bool HandleCommand(const PlayerInfo& sender, const std::string& body) {
    std::vector<std::string> parts = Split(body);
    if (parts.empty()) return false;

    std::string command = Lower(parts[0]);
    if (command != "/kick" && command != "/ban" && command != "/unban" &&
        command != "/give" && command != "/help") {
        return false;
    }

    if (!IsAdmin(sender.controllerPtr)) {
        SendToPlayer(sender, "[Server] You are not an admin.");
        LogMessage("Chat: '" + sender.name + "' tried " + parts[0] + " without admin");
        return true;
    }

    Engine* engine = g_Engine;
    std::string result;

    if (command == "/help") {
        SendToPlayer(sender, "[Server] /kick <player> [reason]");
        SendToPlayer(sender, "[Server] /ban <player> [reason]");
        SendToPlayer(sender, "[Server] /unban <player or SteamID64>");
        SendToPlayer(sender, "[Server] /give <player> <item> [count]");
        return true;
    }

    if (command == "/kick") {
        if (parts.size() < 2) {
            SendToPlayer(sender, "[Server] Usage: /kick <player> [reason]");
            return true;
        }
        std::string reason = parts.size() > 2 ? Join(parts, 2) : "Kicked by an administrator";
        std::string error;
        if (engine->KickPlayer(parts[1], reason, error)) {
            result = "Kicked " + parts[1];
        } else {
            result = "Kick failed: " + error;
        }
    } else if (command == "/ban") {
        if (parts.size() < 2) {
            SendToPlayer(sender, "[Server] Usage: /ban <player> [reason]");
            return true;
        }
        std::string reason = parts.size() > 2 ? Join(parts, 2) : "Banned by an administrator";
        std::string message;
        DomBans::Ban(parts[1], reason, message);
        result = message;
    } else if (command == "/unban") {
        if (parts.size() < 2) {
            SendToPlayer(sender, "[Server] Usage: /unban <player or SteamID64>");
            return true;
        }
        std::string message;
        DomBans::Unban(parts[1], message);
        result = message;
    } else if (command == "/give") {
        if (parts.size() < 3) {
            SendToPlayer(sender, "[Server] Usage: /give <player> <item> [count]");
            return true;
        }
        int32_t count = parts.size() > 3 ? atoi(parts[3].c_str()) : 1;
        std::string message;
        DomItems::GiveItem(parts[1], parts[2], count, message);
        result = message;
    }

    LogMessage("Chat: '" + sender.name + "' ran " + body + " -> " + result);
    SendToPlayer(sender, "[Server] " + result);
    return true;
}

void SendThunk(void* context, void* frame, void* result) {
    Engine* engine = g_Engine;
    bool suppress = false;

    if (engine && engine->IsInitialized() && GameThread::FrameLayoutReady()) {
        uintptr_t locals = Mem::ReadPtr((uintptr_t)frame + GameThread::FrameLocalsOffset());
        if (locals && Mem::Readable((void*)locals, kMessageSize)) {
            Message message;
            message.body = engine->ReadFString(locals + kMessageBody);
            if (!message.body.empty()) {
                message.characterGuid = engine->ReadGuid(locals + kCharacterGuid);
                message.playerId = Mem::ReadI32(locals + kPlayerId);

                std::string source;
                message.senderNetId = engine->ReadUniqueNetId(locals + kSenderId, source);

                PlayerInfo sender;
                bool haveSender = false;
                for (const PlayerInfo& player : engine->GetAllPlayers()) {
                    if (player.playerId != message.playerId) continue;
                    sender = player;
                    haveSender = true;
                    message.senderName =
                        player.characterName.empty() ? player.name : player.characterName;
                    if (message.senderNetId.empty()) message.senderNetId = player.uniqueNetId;
                    break;
                }

                if (haveSender && !message.body.empty() && message.body[0] == '/') {
                    suppress = HandleCommand(sender, message.body);
                }

                if (!suppress) Deliver(message);
            }
        }
    }

    if (!suppress && g_OriginalSend) g_OriginalSend(context, frame, result);
}

}

bool IsHooked() { return g_Hooked; }
const std::string& Status() { return g_Status; }

void AddListener(Listener listener) {
    std::lock_guard<std::mutex> lock(g_ListenerMutex);
    g_Listeners.push_back(listener);
}

void AddEventListener(EventListener listener) {
    std::lock_guard<std::mutex> lock(g_ListenerMutex);
    g_EventListeners.push_back(listener);
}

void Poll() {
    Engine* engine = g_Engine;
    if (!engine || !engine->IsInitialized() || !g_Hooked) return;

    ResolveLateFunctions(false);

    if (!g_ClientEventFunction && !g_ServerEventFunction && !g_SystemMessageFunction) return;
    if (GameThread::VTableIndex() < 0) return;

    for (const PlayerInfo& player : engine->GetAllPlayers()) {
        if (!player.controllerPtr) continue;
        uintptr_t component = ResolveChatComponent(player.controllerPtr);
        if (!component) continue;
        uintptr_t vtable = Mem::ReadPtr(component);
        if (!vtable) continue;

        {
            std::lock_guard<std::mutex> lock(g_VTableMutex);
            if (g_HookedVTables.count(vtable)) continue;
        }

        GameThread::ProcessEventFn previous = nullptr;
        if (!GameThread::HookVTable(component, &HookedChatProcessEvent, previous)) continue;
        if (!previous) continue;

        {
            std::lock_guard<std::mutex> lock(g_VTableMutex);
            g_HookedVTables[vtable] = previous;
        }
        LogMessage("Chat: watching player events on " + engine->GetObjectClassName(component));
    }
}

bool Initialize() {
    if (g_Hooked) return true;

    Engine* engine = g_Engine;
    if (!engine || !engine->IsInitialized()) {
        g_Status = "engine not initialised";
        return false;
    }

    // Called repeatedly until it succeeds (see the main loop); only the first failure and
    // every tenth after it are logged so a slow load does not flood the console.
    g_InitAttempts++;
    bool logFailure = g_InitAttempts == 1 || g_InitAttempts % 10 == 0;
    std::string attempt = " (attempt " + std::to_string(g_InitAttempts) + ")";

    g_SendFunction = engine->FindFunction("PlayerChatComponent", "Server_SendChatMessage");
    if (!g_SendFunction) {
        g_Status = "Server_SendChatMessage not found yet";
        if (logFailure) LogMessage("Chat: " + g_Status + attempt + ", will retry");
        return false;
    }

    uintptr_t original = Mem::ReadPtr(g_SendFunction + kExecFunction);
    if (original == (uintptr_t)&SendThunk) {
        // Already ours - a previous attempt got this far.
        g_Hooked = true;
        g_Status = "hooked";
        return true;
    }
    if (!original || !Mem::InImage((void*)original)) {
        g_Status = "Server_SendChatMessage has no usable thunk yet";
        if (logFailure) LogMessage("Chat: " + g_Status + attempt + ", will retry");
        return false;
    }

    ResolveLateFunctions(true);

    g_OriginalSend = (ExecThunk)original;
    *(uintptr_t*)(g_SendFunction + kExecFunction) = (uintptr_t)&SendThunk;

    g_Hooked = true;
    g_Status = "hooked";
    LogMessage("Chat: reading messages from PlayerChatComponent.Server_SendChatMessage" +
               (g_InitAttempts > 1 ? attempt : std::string()));
    if (!g_ReceiveFunction) {
        LogMessage("Chat: Client_ReceiveChatMessage not found yet, broadcast unavailable until it is");
    }
    if (!g_AdminFunction || !g_AdminLibrary) {
        LogMessage("Chat: ControllerIsOwnerOrAdmin not found yet, chat commands unavailable until it is");
    }
    return true;
}

bool Broadcast(const std::string& body, std::string& outError, int* outDelivered) {
    Engine* engine = g_Engine;
    if (!engine || !engine->IsInitialized()) {
        outError = "engine not initialised";
        return false;
    }
    if (!g_ReceiveFunction) ResolveLateFunctions(false);
    if (!g_ReceiveFunction) {
        outError = "Client_ReceiveChatMessage not found";
        return false;
    }
    if (!GameThread::IsReady()) {
        outError = "broadcast needs the game thread pump: " + GameThread::Status();
        return false;
    }
    if (!GameThread::HasProcessEvent()) {
        outError = "broadcast needs ProcessEvent, which has not been located yet";
        return false;
    }
    if (body.empty()) {
        outError = "message is empty";
        return false;
    }

    std::vector<PlayerInfo> players = engine->GetAllPlayers();
    if (players.empty()) {
        outError = "no players to broadcast to";
        return false;
    }

    int delivered = 0;
    bool ran = GameThread::RunSync([&]() {
        for (const PlayerInfo& player : players) {
            if (SendToPlayer(player, body)) delivered++;
        }
    }, 10000);

    if (!ran) {
        outError = "the game thread did not run the broadcast within 10s";
        return false;
    }
    if (!delivered) {
        outError = "no recipient had a validated PlayerChatComponent - see the log";
        return false;
    }

    // Dispatched, not confirmed: Client_ReceiveChatMessage is a client RPC and the server
    // gets no acknowledgement that a client displayed it.
    LogMessage("Chat: broadcast dispatched to " + std::to_string(delivered) + "/" +
               std::to_string(players.size()) + " player(s): " + body);
    if (outDelivered) *outDelivered = delivered;
    return true;
}

}
