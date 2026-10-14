#include "dom_engine.h"
#include "native_call.h"
#include "process_event.h"
#include "../utils/logger.h"
#include "../utils/memory.h"

#include <cstring>
#include <unordered_set>

namespace DomEngine {

namespace {

// Même recouvrement par appartenance que GetAllPlayers : l'offset SDK direct
// peut bouger entre les builds, l'appartenance (Outer) ne ment pas.
uintptr_t FindOwnedComponent(const Engine* engine, uintptr_t owner,
                             const char* className) {
    if (!owner) return 0;
    int32_t count = engine->GetObjectCount();
    uintptr_t match = 0;
    for (int32_t i = 0; i < count; i++) {
        uintptr_t object = engine->GetObjectByIndex(i);
        if (!object) continue;
        if (!engine->IsA(object, className)) continue;
        if (Mem::ReadPtr(object + Offsets::UObject_Outer) != owner) continue;
        if (match && match != object) return 0; // ambigu → refus
        match = object;
    }
    return match;
}

bool ReadArray(uintptr_t address, FUEArray& out, int32_t maxCount) {
    if (!Mem::Readable((void*)address, sizeof(FUEArray))) return false;
    memcpy(&out, (void*)address, sizeof(out));
    return out.Data && out.Num > 0 && out.Num <= maxCount;
}

bool Resolve(const Engine* engine, const std::string& identifier,
             PlayerInfo& out, std::string& outError) {
    std::string PlayerInfo::* const fields[] = {
        &PlayerInfo::uniqueNetId,
        &PlayerInfo::characterGuid,
        &PlayerInfo::characterName,
        &PlayerInfo::name,
    };
    std::vector<PlayerInfo> players = engine->GetAllPlayers();
    for (const PlayerInfo& player : players) {
        if (std::to_string(player.playerId) == identifier) {
            out = player;
            return true;
        }
    }
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

/// Fixe la valeur d'un ATTRIBUT (ex. « Health ») d'un joueur.
bool Engine::SetAttribute(const std::string& identifier,
                          const std::string& attributeName, float value,
                          std::string& outError) const {
    if (!initialized) {
        outError = "engine not initialised";
        return false;
    }
    PlayerInfo player;
    if (!Resolve(this, identifier, player, outError)) return false;
    if (!player.pawnPtr) {
        outError = "player has no spawned pawn";
        return false;
    }

    uintptr_t component =
        Mem::ReadPtr(player.pawnPtr + Offsets::DomPlayerChar_AttributesComponent);
    if (!component || !IsA(component, "DominionAttributesComponent")) {
        component = FindOwnedComponent(this, player.pawnPtr, "DominionAttributesComponent");
    }
    if (!component || !Mem::Readable((void*)component, 0x1B8)) {
        outError = "attributes component not found";
        return false;
    }

    FUEArray attributes = {};
    FUEArray values = {};
    // Même recouvrement que la lecture des joueurs : les attributs peuvent
    // vivre dans la paire « partagée » (SharedFloatAttributes/SharedAttributeValues),
    // pas seulement dans la paire normale.
    for (int pass = 0; pass < 2; pass++) {
        uintptr_t attrsOffset = pass == 0 ? Offsets::Attributes_FloatAttributes
                                          : Offsets::Attributes_SharedFloatAttributes;
        uintptr_t valsOffset = pass == 0 ? Offsets::Attributes_AttributeValues
                                         : Offsets::Attributes_SharedAttributeValues;
        attributes = {};
        values = {};
        if (!ReadArray(component + attrsOffset, attributes, 512) ||
            !ReadArray(component + valsOffset, values, 512)) {
            continue;
        }
        if (!Mem::Readable(attributes.Data, sizeof(uintptr_t) * (size_t)attributes.Num) ||
            !Mem::Readable(values.Data, sizeof(float) * (size_t)values.Num)) {
            continue;
        }

        int32_t count = attributes.Num < values.Num ? attributes.Num : values.Num;
        for (int32_t i = 0; i < count; i++) {
            uintptr_t attribute = ((uintptr_t*)attributes.Data)[i];
            if (!attribute) continue;
            std::string name = GetObjectClassName(attribute);
            const std::string suffix = "Attribute";
            if (name.size() > suffix.size() &&
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                name.erase(name.size() - suffix.size());
            }
            if (name != attributeName) continue;
            ((float*)values.Data)[i] = value;
            LogMessage("Admin: set attribute " + attributeName + " of " + identifier +
                       " to " + std::to_string(value));
            return true;
        }
    }
    outError = "attribute not found: " + attributeName;
    return false;
}

/// Remet la santé du joueur à son maximum.
/// Voie propre = la native du jeu HealthComponent::IncreaseHealth (réplique
/// et notifie le système de santé) sur le game thread — l'écriture directe
/// d'attribut ne marque pas la propriété « dirty » : le client ne voit pas
/// le changement. Repli : écriture directe attribut + AuthoritativeHealth.
bool Engine::HealPlayer(const std::string& identifier, std::string& outError) const {
    if (!initialized) {
        outError = "engine not initialised";
        return false;
    }
    PlayerInfo player;
    if (!Resolve(this, identifier, player, outError)) return false;
    if (!player.pawnPtr) {
        outError = "player has no spawned pawn";
        return false;
    }
    if (!player.hasHealth) {
        outError = "health not readable for this player";
        return false;
    }

    uintptr_t health =
        Mem::ReadPtr(player.pawnPtr + Offsets::DomPlayerChar_HealthComponent);
    if (!health || !IsA(health, "PlayerHealthComponent")) {
        health = FindOwnedComponent(this, player.pawnPtr, "PlayerHealthComponent");
    }

    float amount = player.maxHealth - player.health;
    if (amount > 0.0f && health) {
        static uintptr_t healFn = 0;
        if (!healFn) {
            healFn = FindFunction("HealthComponent", "IncreaseHealth");
            if (healFn && !NativeCall::IsNative(healFn)) healFn = 0;
            LogMessage(healFn ? "Heal: HealthComponent::IncreaseHealth resolved"
                              : "Heal: IncreaseHealth not found — direct write fallback");
        }
        if (healFn) {
            struct Params {
                float Amount;
            } params = {amount};
            bool ok = false;
            bool ran = GameThread::RunSync(
                [&]() { ok = NativeCall::Invoke(health, healFn, &params, 0x00); }, 5000);
            if (ran && ok) {
                LogMessage("Admin: healed " + identifier + " by " +
                           std::to_string(amount) + " (native IncreaseHealth)");
                return true;
            }
            LogMessage("Heal: native call refused/unavailable — direct write fallback");
        }
    } else if (amount <= 0.0f) {
        LogMessage("Admin: " + identifier + " already at full health");
        return true;
    }

    // Repli (et synchronisation) : écriture directe de l'attribut partagé
    // « Health » + du champ AuthoritativeHealth du composant.
    bool ok = SetAttribute(identifier, "Health", player.maxHealth, outError);
    if (health && Mem::Readable((void*)health, 0x1B8)) {
        Mem::WriteFloat(health + Offsets::Health_AuthoritativeHealth, player.maxHealth);
    }
    if (ok) {
        LogMessage("Admin: healed " + identifier + " to " + std::to_string(player.maxHealth) +
                   " (direct write)");
    }
    return ok;
}

/// Bascule `bCanDie` du composant de santé (god mode).
bool Engine::SetGodMode(const std::string& identifier, bool enabled,
                        std::string& outError) const {
    if (!initialized) {
        outError = "engine not initialised";
        return false;
    }
    PlayerInfo player;
    if (!Resolve(this, identifier, player, outError)) return false;
    if (!player.pawnPtr) {
        outError = "player has no spawned pawn";
        return false;
    }
    uintptr_t health =
        Mem::ReadPtr(player.pawnPtr + Offsets::DomPlayerChar_HealthComponent);
    if (!health || !IsA(health, "PlayerHealthComponent")) {
        health = FindOwnedComponent(this, player.pawnPtr, "PlayerHealthComponent");
    }
    if (!health || !Mem::Readable((void*)health, 0x1B8)) {
        outError = "health component not found";
        return false;
    }
    // Écriture directe d'un booléen : simple et réversible.
    if (!Mem::WriteU8(health + Offsets::Health_bCanDie, enabled ? 0 : 1)) {
        outError = "health component not writable";
        return false;
    }
    LogMessage("Admin: god mode " + std::string(enabled ? "ON" : "OFF") +
               " for " + identifier);
    return true;
}

/// Règle Sustenance ou Hydration d'un joueur (composants recouvrés).
bool Engine::SetVital(const std::string& identifier, const char* componentName,
                      float value, std::string& outError) const {
    if (!initialized) {
        outError = "engine not initialised";
        return false;
    }
    PlayerInfo player;
    if (!Resolve(this, identifier, player, outError)) return false;
    if (!player.pawnPtr) {
        outError = "player has no spawned pawn";
        return false;
    }
    uintptr_t component = FindOwnedComponent(this, player.pawnPtr, componentName);
    if (!component || !Mem::Readable((void*)component, 0x140)) {
        outError = std::string(componentName) + " not found";
        return false;
    }
    if (!Mem::WriteFloat(component + Offsets::SurvivalStat_Current, value)) {
        outError = "component not writable";
        return false;
    }
    LogMessage("Admin: " + std::string(componentName) + " of " + identifier +
               " set to " + std::to_string(value));
    return true;
}

/// Nourrit le joueur à 100 % (Sustenance = CurrentMax).
bool Engine::FeedPlayer(const std::string& identifier, std::string& outError) const {
    PlayerInfo player;
    if (!Resolve(this, identifier, player, outError)) return false;
    float max = 100.0f;
    // Le maximum lu par le scan de GetAllPlayers est plus juste s'il existe.
    if (player.hasSustenance && player.maxSustenance > 0) max = player.maxSustenance;
    return SetVital(identifier, "SustenanceComponent", max, outError);
}

/// Hydrate le joueur à 100 % (Hydration = CurrentMax).
bool Engine::HydratePlayer(const std::string& identifier, std::string& outError) const {
    PlayerInfo player;
    if (!Resolve(this, identifier, player, outError)) return false;
    float max = 100.0f;
    if (player.hasHydration && player.maxHydration > 0) max = player.maxHydration;
    return SetVital(identifier, "HydrationComponent", max, outError);
}

}
