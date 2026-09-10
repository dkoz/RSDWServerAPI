#include "dom_engine.h"
#include "native_call.h"
#include "process_event.h"
#include "../utils/logger.h"
#include "../utils/memory.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace DomEngine {

namespace {

// Source: RSDWSDK/CppSDK/SDK/RedpointEOSFramework_parameters.hpp:483
struct KickParams {
    uintptr_t PlayerController;  // 0x00
    uint8_t   KickReason[0x10];  // 0x08, FText
    bool      ReturnValue;       // 0x18
    uint8_t   Pad[0x7];
};

// Source: RSDWSDK/CppSDK/SDK/Engine_parameters.hpp:45747
struct ConvStringToTextParams {
    FString InString;            // 0x00
    uint8_t ReturnValue[0x10];   // 0x10, FText
};

void WriteFString(FString& out, const std::string& value) {
    int32_t count = (int32_t)value.size() + 1;
    char16_t* buffer = new char16_t[(size_t)count];
    for (size_t i = 0; i < value.size(); i++) buffer[i] = (char16_t)(unsigned char)value[i];
    buffer[value.size()] = 0;

    out.Data = buffer;
    out.Num = count;
    out.Max = count;
}

}

bool Engine::KickPlayer(const std::string& identifier, const std::string& reason,
                        std::string& outError) const {
    if (!initialized) {
        outError = "engine not initialised";
        return false;
    }

    std::string PlayerInfo::* const fields[] = {
        &PlayerInfo::uniqueNetId,
        &PlayerInfo::characterGuid,
        &PlayerInfo::characterName,
        &PlayerInfo::name,
    };

    std::vector<PlayerInfo> players = GetAllPlayers();

    uintptr_t controller = 0;
    std::string matched;
    std::string matchedNetId;
    std::string matchedBy;

    for (const PlayerInfo& player : players) {
        if (std::to_string(player.playerId) != identifier) continue;
        controller = player.controllerPtr;
        matched = player.name;
        matchedNetId = player.uniqueNetId;
        matchedBy = "playerId";
        break;
    }

    for (size_t f = 0; !controller && f < sizeof(fields) / sizeof(fields[0]); f++) {
        int hits = 0;
        for (const PlayerInfo& player : players) {
            if ((player.*fields[f]).empty()) continue;
            if (player.*fields[f] != identifier) continue;
            hits++;
            controller = player.controllerPtr;
            matched = player.name;
            matchedNetId = player.uniqueNetId;
        }

        if (hits > 1) {
            outError = "'" + identifier + "' matches " + std::to_string(hits) +
                       " players; kick by netId or playerId instead";
            return false;
        }
        if (hits == 1) {
            const char* names[] = {"netId", "characterGuid", "characterName", "name"};
            matchedBy = names[f];
        }
    }

    if (!controller) {
        outError = "no connected player matches '" + identifier + "'";
        return false;
    }
    LogVerbose("Kick: matched '" + matched + "' by " + matchedBy);

    uintptr_t kickFunction = FindFunction("RedpointFrameworkBlueprintLibrary",
                                          "KickPlayerController");
    if (!kickFunction) {
        outError = "KickPlayerController function not found in GObjects";
        return false;
    }

    uintptr_t kickLibrary = FindObject("RedpointFrameworkBlueprintLibrary", true);
    if (!kickLibrary) {
        outError = "RedpointFrameworkBlueprintLibrary default object not found";
        return false;
    }

    uintptr_t textFunction = FindFunction("KismetTextLibrary", "Conv_StringToText");
    uintptr_t textLibrary = FindObject("KismetTextLibrary", true);

    if (!NativeCall::IsNative(kickFunction)) {
        outError = "KickPlayerController is not a native function";
        return false;
    }

    KickParams params = {};
    params.PlayerController = controller;

    bool haveReason = false;

    if (!GameThread::IsReady()) {
        outError = "kick needs the game thread pump: " + GameThread::Status();
        return false;
    }

    bool invoked = false;
    bool ran = GameThread::RunSync([&]() {
        if (textFunction && textLibrary && NativeCall::IsNative(textFunction)) {
            ConvStringToTextParams textParams = {};
            WriteFString(textParams.InString, reason);
            // Source: RSDWSDK/CppSDK/SDK/Engine_parameters.hpp:45751
            if (NativeCall::Invoke(textLibrary, textFunction, &textParams, 0x10)) {
                memcpy(params.KickReason, textParams.ReturnValue, sizeof(params.KickReason));
                haveReason = true;
            }
        }
        if (!haveReason) return;
        // Source: RSDWSDK/CppSDK/SDK/RedpointEOSFramework_parameters.hpp:488
        invoked = NativeCall::Invoke(kickLibrary, kickFunction, &params, 0x18);
    }, 10000);

    if (!ran) {
        outError = "the game thread did not run the kick within 10s";
        return false;
    }
    if (!haveReason) {
        outError = "could not build the kick reason text";
        return false;
    }

    if (!invoked) {
        outError = "could not invoke KickPlayerController";
        return false;
    }

    if (!params.ReturnValue) {
        outError = "the game refused the kick for '" + matched + "'";
        return false;
    }

    LogMessage("Kick: removed '" + matched + "' (" + reason + ")");
    RememberKick(matchedNetId, matched, reason);
    return true;
}

namespace {

struct KickedPlayer {
    std::string netId;
    std::string name;
    std::string reason;
};

std::mutex g_KickedMutex;
std::vector<KickedPlayer> g_Kicked;

}

void Engine::RememberKick(const std::string& netId, const std::string& name,
                          const std::string& reason) const {
    if (netId.empty()) {
        LogMessage("Kick: '" + name + "' has no net id, so the kick cannot be enforced "
                   "across a reconnect");
        return;
    }

    std::lock_guard<std::mutex> lock(g_KickedMutex);
    for (const KickedPlayer& entry : g_Kicked) {
        if (entry.netId == netId) return;
    }
    g_Kicked.push_back({netId, name, reason});
}

bool Engine::IsKicked(const std::string& netId) const {
    if (netId.empty()) return false;
    std::lock_guard<std::mutex> lock(g_KickedMutex);
    for (const KickedPlayer& entry : g_Kicked) {
        if (entry.netId == netId) return true;
    }
    return false;
}

std::vector<std::string> Engine::KickedNetIds() const {
    std::lock_guard<std::mutex> lock(g_KickedMutex);
    std::vector<std::string> out;
    out.reserve(g_Kicked.size());
    for (const KickedPlayer& entry : g_Kicked) out.push_back(entry.netId);
    return out;
}

bool Engine::ClearKick(const std::string& netId) const {
    std::lock_guard<std::mutex> lock(g_KickedMutex);
    for (size_t i = 0; i < g_Kicked.size(); i++) {
        if (g_Kicked[i].netId != netId) continue;
        g_Kicked.erase(g_Kicked.begin() + (long)i);
        return true;
    }
    return false;
}

void Engine::EnforceKicks() const {
    if (!initialized || !GameThread::IsReady()) return;

    std::vector<std::string> wanted = KickedNetIds();
    if (wanted.empty()) return;

    for (const PlayerInfo& player : GetAllPlayers()) {
        if (player.uniqueNetId.empty()) continue;

        bool listed = false;
        std::string reason = "Kicked by an administrator";
        {
            std::lock_guard<std::mutex> lock(g_KickedMutex);
            for (const KickedPlayer& entry : g_Kicked) {
                if (entry.netId != player.uniqueNetId) continue;
                listed = true;
                reason = entry.reason;
                break;
            }
        }
        if (!listed) continue;

        LogMessage("Kick: '" + player.name + "' reconnected while kicked, removing again");
        std::string error;
        if (!KickPlayer(player.uniqueNetId, reason, error)) {
            LogMessage("Kick: re-kick failed - " + error);
        }
    }
}

uintptr_t Engine::FindFunction(const char* className, const char* functionName) const {
    if (!className || !functionName) return 0;

    std::string wanted = functionName;
    int32_t count = GetObjectCount();
    for (int32_t i = 0; i < count; i++) {
        uintptr_t object = GetObjectByIndex(i);
        if (!object) continue;
        if (GetObjectClassName(object) != "Function") continue;
        if (GetObjectName(object) != wanted) continue;

        uintptr_t outer = Mem::ReadPtr(object + Offsets::UObject_Outer);
        if (GetObjectName(outer) != className) continue;
        return object;
    }
    return 0;
}

}
