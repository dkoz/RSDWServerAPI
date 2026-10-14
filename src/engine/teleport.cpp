#include "dom_engine.h"
#include "native_call.h"
#include "process_event.h"
#include "../utils/logger.h"
#include "../utils/memory.h"

#include <cmath>
#include <cstring>

namespace DomEngine {

namespace {

// Resolved once: names survive builds; image offsets move (the segment
// scan recovers the object pool).
uintptr_t g_TeleportFn = 0;
uintptr_t g_TeleportRetOffset = 0x38;

uintptr_t ResolveTeleportFunction(const Engine* engine) {
    if (g_TeleportFn) return g_TeleportFn;

    // Build 1.0.1: AActor::TeleportTo is no longer a registered UFunction;
    // the BlueprintCallable wrapper is K2_TeleportTo.
    g_TeleportFn = engine->FindFunction("Actor", "TeleportTo");
    g_TeleportRetOffset = 0x38;
    if (!g_TeleportFn) {
        g_TeleportFn = engine->FindFunction("Actor", "K2_TeleportTo");
        g_TeleportRetOffset = 0x30;
    }
    if (g_TeleportFn && !NativeCall::IsNative(g_TeleportFn)) g_TeleportFn = 0;
    if (g_TeleportFn) {
        LogMessage("Teleport: Actor::TeleportTo/K2_TeleportTo resolved (ret " +
                   HexString(g_TeleportRetOffset) + "), native calls enabled — résolu, appels natifs activés");
    } else {
        LogMessage("Teleport: Actor::TeleportTo not found — direct RootComponent write only — introuvable, écriture directe du RootComponent uniquement");
    }
    return g_TeleportFn;
}

// Direct RootComponent write fallback; replication snapshots win next tick.
void WriteDoubleTriple(uintptr_t address, double a, double b, double c) {
    if (!Mem::Readable((void*)address, sizeof(double) * 3)) return;
    double values[3] = {a, b, c};
    memcpy((void*)address, values, sizeof(values));
}

} // anonymous namespace

/// Teleports `identifier` to absolute world coordinates (x, y, z).
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
    if (!ResolvePlayerIdentifier(identifier, player, matchedBy, outError)) return false;
    if (!player.pawnPtr) {
        outError = "player has no spawned pawn";
        return false;
    }

    uintptr_t fn = ResolveTeleportFunction(this);
    if (fn) {
        // Params de Actor::TeleportTo(FVector, FRotator, bool, bool) :
        // 0x00 DestLocation (3 doubles), 0x18 DestRotation (3 doubles),
        // TeleportTo: loc 0x00, rot 0x18, bIsATest 0x30, bNoCheck 0x31,
        // bool return 0x38. K2_TeleportTo: same layout, bool return at 0x30.
        uint8_t params[0x100];
        memset(params, 0, sizeof(params));
        double* loc = (double*)(params + 0x00);
        double* rot = (double*)(params + 0x18);
        loc[0] = x; loc[1] = y; loc[2] = z;
        rot[0] = player.pitch; rot[1] = player.yaw; rot[2] = player.roll;

        // REST handler runs on the HTTP thread; UE natives must run on the
        // game thread. Marshalled via the pump queue (drained on player RPC),
        // like Broadcast().
        bool ok = false;
        bool ran = GameThread::RunSync(
            [&]() { ok = NativeCall::Invoke(player.pawnPtr, fn, params, g_TeleportRetOffset); },
            5000);
        if (ran && ok) {
            LogMessage("Teleport: " + identifier + " teleported to " +
                       std::to_string(x) + "," + std::to_string(y) + "," +
                       std::to_string(z) + " (native / natif)");
            return true;
        }
        LogMessage("Teleport: native call refused/unavailable (pump=" +
                   std::string(GameThread::IsReady() ? "ready" : "absent") +
                   ") — direct write fallback — appel natif refusé/indisponible, repli en écriture directe");
    }

    uintptr_t root = Mem::ReadPtr(player.pawnPtr + Offsets::Actor_RootComponent);
    if (!root) {
        outError = "no root component";
        return false;
    }
    WriteDoubleTriple(root + Offsets::SceneComp_RelativeLocation, x, y, z);
    LogMessage("Teleport: " + identifier + " location written directly — position écrite directement");
    return true;
}

/// Teleports `from` onto `to`'s position.
bool Engine::TeleportPlayer(const std::string& from, const std::string& to,
                            std::string& outError) const {
    if (!initialized) {
        outError = "engine not initialised";
        return false;
    }
    PlayerInfo source, target;
    std::string matchedBy;
    if (!ResolvePlayerIdentifier(from, source, matchedBy, outError)) return false;
    if (!ResolvePlayerIdentifier(to, target, matchedBy, outError)) return false;
    if (!target.hasLocation) {
        outError = "target has no known location";
        return false;
    }
    return TeleportToCoords(std::to_string(source.playerId), target.x, target.y,
                            target.z, outError);
}

}
