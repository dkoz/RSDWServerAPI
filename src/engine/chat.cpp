#include "chat.h"
#include "bans.h"
#include "dom_engine.h"
#include "items.h"
#include "native_call.h"
#include "process_event.h"
#include "../utils/logger.h"
#include "../utils/memory.h"

#include <algorithm>
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

// ADominionPlayerController::PlayerChatComponent - Source: Assertions.inl
constexpr uintptr_t kControllerChatComponent = 0x1288;

typedef void (*ExecThunk)(void* context, void* frame, void* result);

constexpr uintptr_t kExecFunction = 0xD8;

ExecThunk g_OriginalSend = nullptr;
uintptr_t g_SendFunction = 0;
uintptr_t g_ReceiveFunction = 0;
uintptr_t g_AdminFunction = 0;
uintptr_t g_AdminLibrary = 0;
uintptr_t g_ClientEventFunction = 0;
uintptr_t g_ServerEventFunction = 0;
uintptr_t g_SystemMessageFunction = 0;
bool g_Hooked = false;
std::string g_Status = "not initialised";

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

bool IsPlayersChatComponent(Engine& engine, uintptr_t component, uintptr_t controller) {
    return component && engine.IsA(component, "PlayerChatComponent") &&
           Mem::ReadPtr(component + Offsets::UObject_Outer) == controller;
}

uintptr_t FindPlayersChatComponent(Engine& engine, uintptr_t controller) {
    if (!controller) return 0;
    uintptr_t component = Mem::ReadPtr(controller + kControllerChatComponent);
    if (IsPlayersChatComponent(engine, component, controller)) return component;

    // Controller members can move between builds: only use a chat
    // component OWNED by this controller, never an arbitrary readable one.
    uintptr_t match = 0;
    for (int32_t i = 0, count = engine.GetObjectCount(); i < count; ++i) {
        uintptr_t candidate = engine.GetObjectByIndex(i);
        if (!IsPlayersChatComponent(engine, candidate, controller)) continue;
        if (match && match != candidate) {
            LogMessage("Chat: multiple chat components belong to controller; refusing ambiguous recipient — plusieurs composants de chat pour ce contrôleur, destinataire ambigu refusé");
            return 0;
        }
        match = candidate;
    }
    if (match) LogMessage("Chat: resolved recipient component by controller ownership (SDK offset did not validate) — composant du destinataire résolu par appartenance au contrôleur (offset SDK invalide)");
    else LogMessage("Chat: no PlayerChatComponent found for recipient controller — aucun composant de chat trouvé pour le contrôleur du destinataire");
    return match;
}

bool SendToPlayerInternal(const PlayerInfo& player, const std::string& body) {
    if (!g_ReceiveFunction || !GameThread::HasProcessEvent()) return false;
    if (!player.controllerPtr || !player.playerStatePtr) return false;

    if (!g_Engine) return false;
    uintptr_t component = FindPlayersChatComponent(*g_Engine, player.controllerPtr);
    if (!component) return false;

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

    GameThread::CallFunction(component, g_ReceiveFunction, params);
    return true;
}

bool IsAdmin(uintptr_t controller) {
    if (!controller) return false;

    Engine* engine = g_Engine;
    if (!engine || !engine->IsInitialized()) return false;

    // Late-registered game feature: the one-shot Initialize() lookup can
    // miss it at boot. Resolve lazily on first use, like items.cpp.
    if (!g_AdminFunction) {
        g_AdminFunction =
            engine->FindFunction("PrivilegeFunctionLibrary", "ControllerIsOwnerOrAdmin");
    }
    if (!g_AdminLibrary) {
        g_AdminLibrary = engine->FindObject("PrivilegeFunctionLibrary", true);
    }
    if (!g_AdminFunction || !g_AdminLibrary) return false;
    if (!NativeCall::IsNative(g_AdminFunction)) return false;

    struct Params {
        uintptr_t Controller;
        bool ReturnValue;
        uint8_t Pad[7];
    } params = {};
    params.Controller = controller;

    if (!NativeCall::Invoke(g_AdminLibrary, g_AdminFunction, &params, 0x08)) return false;
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

// Defined here (not in the anonymous namespace) because chat.h declares
// it with external linkage — an internal definition would break at link
// time with a symbol lookup error (crashing the game process).
bool SendToPlayer(const DomEngine::PlayerInfo& player, const std::string& body) {
    return SendToPlayerInternal(player, body);
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
    if (!g_ClientEventFunction && !g_ServerEventFunction && !g_SystemMessageFunction) return;
    if (GameThread::VTableIndex() < 0) return;

    for (const PlayerInfo& player : engine->GetAllPlayers()) {
        if (!player.controllerPtr) continue;
        uintptr_t component = Mem::ReadPtr(player.controllerPtr + kControllerChatComponent);
        if (!component || !Mem::Readable((void*)component, sizeof(uintptr_t))) continue;
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
        LogMessage("Chat: watching player events (surveille les événements joueurs) on " + engine->GetObjectClassName(component));
    }
}

bool Initialize() {
    if (g_Hooked) return true;

    Engine* engine = g_Engine;
    if (!engine || !engine->IsInitialized()) {
        g_Status = "engine not initialised";
        return false;
    }

    g_SendFunction = engine->FindFunction("PlayerChatComponent", "Server_SendChatMessage");
    g_ReceiveFunction = engine->FindFunction("PlayerChatComponent", "Client_ReceiveChatMessage");
    g_AdminFunction = engine->FindFunction("PrivilegeFunctionLibrary", "ControllerIsOwnerOrAdmin");
    g_AdminLibrary = engine->FindObject("PrivilegeFunctionLibrary", true);
    g_ClientEventFunction = engine->FindFunction("PlayerChatComponent", "Client_ReceivePlayerEvent");
    g_ServerEventFunction = engine->FindFunction("PlayerChatComponent", "Server_SendPlayerEvent");
    g_SystemMessageFunction = engine->FindFunction("PlayerChatComponent", "Client_ReceiveSystemMessage");

    if (!g_SendFunction) {
        g_Status = "Server_SendChatMessage not found";
        LogMessage("Chat: " + g_Status);
        return false;
    }

    uintptr_t original = Mem::ReadPtr(g_SendFunction + kExecFunction);
    if (!original || !Mem::InImage((void*)original)) {
        g_Status = "Server_SendChatMessage has no usable thunk";
        LogMessage("Chat: " + g_Status);
        return false;
    }

    g_OriginalSend = (ExecThunk)original;
    *(uintptr_t*)(g_SendFunction + kExecFunction) = (uintptr_t)&SendThunk;

    g_Hooked = true;
    g_Status = "hooked";
    LogMessage("Chat: reading messages (lecture des messages) from PlayerChatComponent.Server_SendChatMessage");
    if (!g_ReceiveFunction) LogMessage("Chat: Client_ReceiveChatMessage not found, broadcast unavailable — introuvable, diffusion indisponible");
    if (!g_AdminFunction || !g_AdminLibrary) LogMessage("Chat: ControllerIsOwnerOrAdmin not found, chat commands unavailable — introuvable, commandes chat indisponibles");
    return true;
}

bool Broadcast(const std::string& body, std::string& outError) {
    Engine* engine = g_Engine;
    if (!engine || !engine->IsInitialized()) {
        outError = "engine not initialised";
        return false;
    }
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
            if (SendToPlayerInternal(player, body)) delivered++;
        }
    }, 10000);

    if (!ran) {
        outError = "the game thread did not run the broadcast within 10s";
        return false;
    }
    if (!delivered) {
        outError = "the game accepted no recipients";
        return false;
    }

    LogMessage("Chat: broadcast (diffusion) to " + std::to_string(delivered) + " player(s): " + body);
    return true;
}


bool Tell(const std::string& identifier, const std::string& body,
          std::string& outError) {
    Engine* engine = g_Engine;
    if (!engine || !engine->IsInitialized()) {
        outError = "engine not initialised";
        return false;
    }
    if (!GameThread::IsReady() || !GameThread::HasProcessEvent()) {
        outError = "chat unavailable: " + GameThread::Status();
        return false;
    }
    if (body.empty()) {
        outError = "message is empty";
        return false;
    }

    std::vector<PlayerInfo> players = engine->GetAllPlayers();
    PlayerInfo target;
    int hits = 0;
    for (const PlayerInfo& player : players) {
        bool match = std::to_string(player.playerId) == identifier ||
                     player.uniqueNetId == identifier ||
                     player.characterGuid == identifier ||
                     player.characterName == identifier ||
                     player.name == identifier;
        if (match) {
            hits++;
            target = player;
        }
    }
    if (hits == 0) {
        outError = "player not found: " + identifier;
        return false;
    }
    if (hits > 1) {
        outError = "ambiguous identifier \"" + identifier + "\"";
        return false;
    }

    bool delivered = false;
    std::string innerError;
    bool ran = GameThread::RunSync([&]() {
        delivered = SendToPlayerInternal(target, body);
        if (!delivered) innerError = "send failed";
    });
    if (!ran) {
        outError = "game thread busy: " + GameThread::Status();
        return false;
    }
    if (!delivered) {
        outError = innerError.empty() ? "message not delivered" : innerError;
        return false;
    }
    return true;
}

}
