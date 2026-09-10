#include "chat.h"
#include "dom_engine.h"
#include "native_call.h"
#include "process_event.h"
#include "../utils/logger.h"
#include "../utils/memory.h"

#include <cstring>
#include <mutex>
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

void SendThunk(void* context, void* frame, void* result) {
    Engine* engine = g_Engine;
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

                for (const PlayerInfo& player : engine->GetAllPlayers()) {
                    if (player.playerId != message.playerId) continue;
                    message.senderName =
                        player.characterName.empty() ? player.name : player.characterName;
                    if (message.senderNetId.empty()) message.senderNetId = player.uniqueNetId;
                    break;
                }

                Deliver(message);
            }
        }
    }

    if (g_OriginalSend) g_OriginalSend(context, frame, result);
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
    if (!g_ReceiveFunction) {
        LogMessage("Chat: Client_ReceiveChatMessage not found, broadcast unavailable");
    }
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

    struct Target {
        uintptr_t component;
        uintptr_t playerState;
        int32_t playerId;
    };

    std::vector<Target> targets;
    for (const PlayerInfo& player : engine->GetAllPlayers()) {
        if (!player.controllerPtr || !player.playerStatePtr) continue;
        if (!engine->IsA(player.controllerPtr, "DominionPlayerController")) continue;

        uintptr_t component = Mem::ReadPtr(player.controllerPtr + kControllerChatComponent);
        if (!component || !Mem::Readable((void*)component, 0x150)) continue;
        targets.push_back({component, player.playerStatePtr, player.playerId});
    }

    if (targets.empty()) {
        outError = "no players to broadcast to";
        return false;
    }

    int delivered = 0;
    bool ran = GameThread::RunSync([&]() {
        for (const Target& target : targets) {
            uint8_t params[kMessageSize + 16];
            memset(params, 0, sizeof(params));

            uintptr_t netId = target.playerState + Offsets::PlayerState_UniqueID;
            if (Mem::Readable((void*)netId, Offsets::NetIdRepl_Size)) {
                memcpy(params + kSenderId, (void*)netId, Offsets::NetIdRepl_Size);
            }

            uintptr_t guid = target.playerState + Offsets::DomPlayerState_CharacterGuid;
            if (Mem::Readable((void*)guid, 16)) {
                memcpy(params + kCharacterGuid, (void*)guid, 16);
            }

            *(int32_t*)(params + kPlayerId) = target.playerId;
            *(uint32_t*)(params + kColor) = 0xFFFFFFFF;
            WriteFString((uintptr_t)params + kMessageBody, body);

            GameThread::CallFunction(target.component, g_ReceiveFunction, params);
            delivered++;
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
