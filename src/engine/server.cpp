#include "dom_engine.h"
#include "../utils/memory.h"

namespace DomEngine {

ServerInfo Engine::GetServerInfo() const {
    ServerInfo info;
    if (!initialized) return info;

    uintptr_t settings = FindObject("DedicatedServerSettings", true);
    if (settings) {
        info.ownerId = ReadFString(settings + Offsets::DediSettings_OwnerId);
        info.serverName = ReadFString(settings + Offsets::DediSettings_ServerName);
        info.worldName = ReadFString(settings + Offsets::DediSettings_DefaultWorldName);
        info.serverGuid = ReadGuid(settings + Offsets::DediSettings_ServerGuid);
        info.valid = true;
    }

    uintptr_t gameState = GetGameState();
    if (gameState && IsA(gameState, "DominionGameStateBase")) {
        info.isDedicatedServer =
            Mem::ReadU8(gameState + Offsets::DomGameState_bIsDedicatedServer) != 0;
        info.crossplayEnabled =
            Mem::ReadU8(gameState + Offsets::DomGameState_bIsCrossplayEnabled) != 0;
        info.hardcoreState =
            (int32_t)Mem::ReadU8(gameState + Offsets::DomGameState_WorldHardcoreState);
        info.valid = true;
    }

    return info;
}

}
