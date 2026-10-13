#include "dom_engine.h"
#include "native_call.h"
#include "../utils/logger.h"
#include "../utils/memory.h"

#include <cmath>
#include <cstring>

namespace DomEngine {

namespace {

// Résolues une seule fois (les noms ne bougent pas entre les builds, seuls
// les offsets d'image bougent — et le pool d'objets est recouvré par le
// scan de segments).
uintptr_t g_TeleportFn = 0;

uintptr_t ResolveTeleportFunction(const Engine* engine) {
    if (g_TeleportFn) return g_TeleportFn;
    g_TeleportFn = engine->FindFunction("Actor", "TeleportTo");
    if (g_TeleportFn && !NativeCall::IsNative(g_TeleportFn)) g_TeleportFn = 0;
    if (g_TeleportFn) {
        LogMessage("Teleport: Actor::TeleportTo resolved, native calls enabled");
    } else {
        LogMessage("Teleport: Actor::TeleportTo not found — direct RootComponent write only");
    }
    return g_TeleportFn;
}

// Écriture directe de la localisation du RootComponent (repli sans appel
// natif, et mécanique des SNAPSHOTS : la prochaine réplication l'emporte).
void WriteDoubleTriple(uintptr_t address, double a, double b, double c) {
    if (!Mem::Readable((void*)address, sizeof(double) * 3)) return;
    double values[3] = {a, b, c};
    memcpy((void*)address, values, sizeof(values));
}

bool ResolvePlayer(const Engine* engine, const std::string& identifier,
                   PlayerInfo& out, std::string& matchedBy, std::string& outError) {
    std::string PlayerInfo::* const fields[] = {
        &PlayerInfo::uniqueNetId,
        &PlayerInfo::characterGuid,
        &PlayerInfo::characterName,
        &PlayerInfo::name,
    };

    std::vector<PlayerInfo> players = engine->GetAllPlayers();

    // 1. playerId numérique exact.
    for (const PlayerInfo& player : players) {
        if (std::to_string(player.playerId) == identifier) {
            out = player;
            matchedBy = "playerId";
            return true;
        }
    }
    // 2. Autres champs (précédence documentée).
    for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); f++) {
        int hits = 0;
        PlayerInfo match;
        for (const PlayerInfo& player : players) {
            if ((player.*fields[f]).empty()) continue;
            if (player.*fields[f] != identifier) continue;
            hits++;
            match = player;
        }
        if (hits == 1) {
            out = match;
            matchedBy = "field";
            return true;
        }
        if (hits > 1) {
            outError = "ambiguous identifier \"" + identifier + "\"";
            return false;
        }
    }
    outError = "player not found: " + identifier;
    return false;
}

}

/// Téléporte `identifier` aux coordonnées absolues (x, y, z).
bool Engine::TeleportToCoords(const std::string& identifier, double x, double y,
                              double z, std::string& outError) const {
    if (!initialized) {
        outError = "engine not initialised";
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        std::fabs(x) > 1e7 || std::fabs(y) > 1e7 || std::fabs(z) > 1e7) {
        outError = "coordinates out of range";
        return false;
    }

    PlayerInfo player;
    std::string matchedBy;
    if (!ResolvePlayer(this, identifier, player, matchedBy, outError)) return false;
    if (!player.pawnPtr) {
        outError = "player has no spawned pawn";
        return false;
    }

    uintptr_t fn = ResolveTeleportFunction(this);
    if (fn) {
        // Params de Actor::TeleportTo(FVector, FRotator, bool, bool) :
        // 0x00 DestLocation (3 doubles), 0x18 DestRotation (3 doubles),
        // 0x30 bIsATest, 0x31 bNoCheck, retour aligné 8 → 0x38.
        uint8_t params[0x100];
        memset(params, 0, sizeof(params));
        double* loc = (double*)(params + 0x00);
        double* rot = (double*)(params + 0x18);
        loc[0] = x; loc[1] = y; loc[2] = z;
        rot[0] = player.pitch; rot[1] = player.yaw; rot[2] = player.roll;
        if (NativeCall::Invoke(player.pawnPtr, fn, params, 0x38)) {
            LogMessage("Teleport: " + identifier + " teleported to " +
                       std::to_string(x) + "," + std::to_string(y) + "," +
                       std::to_string(z) + " (native)");
            return true;
        }
        LogMessage("Teleport: native call refused — direct write fallback");
    }

    uintptr_t root = Mem::ReadPtr(player.pawnPtr + Offsets::Actor_RootComponent);
    if (!root) {
        outError = "no root component";
        return false;
    }
    WriteDoubleTriple(root + Offsets::SceneComp_RelativeLocation, x, y, z);
    LogMessage("Teleport: " + identifier + " location written directly");
    return true;
}

/// Téléporte `from` sur la position de `to`.
bool Engine::TeleportPlayer(const std::string& from, const std::string& to,
                            std::string& outError) const {
    if (!initialized) {
        outError = "engine not initialised";
        return false;
    }
    PlayerInfo source, target;
    std::string matchedBy;
    if (!ResolvePlayer(this, from, source, matchedBy, outError)) return false;
    if (!ResolvePlayer(this, to, target, matchedBy, outError)) return false;
    if (!target.hasLocation) {
        outError = "target has no known location";
        return false;
    }
    return TeleportToCoords(std::to_string(source.playerId), target.x, target.y,
                            target.z, outError);
}

}
