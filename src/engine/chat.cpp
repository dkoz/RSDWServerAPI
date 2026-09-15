#include "chat.h"
#include "bans.h"
#include "dom_engine.h"
#include "items.h"
#include "native_call.h"
#include "process_event.h"
#include "../utils/logger.h"
#include "../utils/memory.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

namespace DomChat {

namespace {

using namespace DomEngine;

// FChatMessageData - Source: RSDWSDK/CppSDK/Assertions.inl
constexpr uintptr_t kSenderId      = 0x00;
constexpr uintptr_t kCharacterGuid = 0x30;
constexpr uintptr_t kPlayerId      = 0x40;
constexpr uintptr_t kColor         = 0x44;
constexpr uintptr_t kMessageBody   = 0x48;
constexpr size_t    kMessageSize   = 0x58;

// ADominionPlayerController::PlayerChatComponent - Source: Assertions.inl
constexpr uintptr_t kControllerChatComponent = 0x11D0;

typedef void (*ExecThunk)(void* context, void* frame, void* result);

constexpr uintptr_t kExecFunction = 0xD8;

ExecThunk g_OriginalSend = nullptr;
uintptr_t g_SendFunction = 0;
uintptr_t g_ReceiveFunction = 0;
uintptr_t g_AdminFunction = 0;
uintptr_t g_AdminLibrary = 0;
bool g_Hooked = false;
std::string g_Status = "not initialised";

std::mutex g_ListenerMutex;
std::vector<Listener> g_Listeners;

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

bool IsPlayersChatComponent(Engine& engine, uintptr_t component, uintptr_t controller) {
    return component && engine.IsA(component, "PlayerChatComponent") &&
           Mem::ReadPtr(component + Offsets::UObject_Outer) == controller;
}

uintptr_t FindPlayersChatComponent(Engine& engine, uintptr_t controller) {
    if (!controller) return 0;
    uintptr_t component = Mem::ReadPtr(controller + kControllerChatComponent);
    if (IsPlayersChatComponent(engine, component, controller)) return component;

    // Controller members can move between game builds. Only use a live chat
    // component belonging to this controller, never an arbitrary readable object.
    uintptr_t match = 0;
    for (int32_t i = 0, count = engine.GetObjectCount(); i < count; ++i) {
        uintptr_t candidate = engine.GetObjectByIndex(i);
        if (!IsPlayersChatComponent(engine, candidate, controller)) continue;
        if (match && match != candidate) {
            LogMessage("Chat: multiple chat components belong to controller; refusing ambiguous recipient");
            return 0;
        }
        match = candidate;
    }
    if (match) LogMessage("Chat: resolved recipient component by controller ownership (SDK offset did not validate)");
    else LogMessage("Chat: no PlayerChatComponent found for recipient controller");
    return match;
}

bool SendToPlayer(const PlayerInfo& player, const std::string& body) {
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
    if (!controller || !g_AdminFunction || !g_AdminLibrary) return false;
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

bool IsHooked() { return g_Hooked; }
const std::string& Status() { return g_Status; }

void AddListener(Listener listener) {
    std::lock_guard<std::mutex> lock(g_ListenerMutex);
    g_Listeners.push_back(listener);
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
    LogMessage("Chat: reading messages from PlayerChatComponent.Server_SendChatMessage");
    if (!g_ReceiveFunction) LogMessage("Chat: Client_ReceiveChatMessage not found, broadcast unavailable");
    if (!g_AdminFunction || !g_AdminLibrary) LogMessage("Chat: ControllerIsOwnerOrAdmin not found, chat commands unavailable");
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
            if (SendToPlayer(player, body)) delivered++;
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

    LogMessage("Chat: broadcast to " + std::to_string(delivered) + " player(s): " + body);
    return true;
}

}
