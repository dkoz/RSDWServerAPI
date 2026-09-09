#include "dom_engine.h"
#include "../utils/logger.h"
#include "../utils/memory.h"

#include <cstring>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace DomEngine {

namespace {

struct PawnComponents {
    uintptr_t sustenance = 0;
    uintptr_t hydration = 0;
};

void ReadDoubleTriple(uintptr_t address, double& a, double& b, double& c) {
    if (!Mem::Readable((void*)address, sizeof(double) * 3)) return;
    double values[3] = {};
    memcpy(values, (void*)address, sizeof(values));
    a = values[0];
    b = values[1];
    c = values[2];
}

bool ReadArray(uintptr_t address, FUEArray& out, int32_t maxCount) {
    if (!Mem::Readable((void*)address, sizeof(FUEArray))) return false;
    memcpy(&out, (void*)address, sizeof(out));
    return out.Data && out.Num > 0 && out.Num <= maxCount;
}

}

void Engine::ReadAttributeArrays(uintptr_t attributesComponent, uintptr_t attributesOffset,
                                 uintptr_t valuesOffset, bool shared,
                                 std::vector<AttributeInfo>& out) const {
    FUEArray attributes = {};
    FUEArray values = {};
    if (!ReadArray(attributesComponent + attributesOffset, attributes, 512)) return;
    if (!ReadArray(attributesComponent + valuesOffset, values, 512)) return;
    if (!Mem::Readable(attributes.Data, sizeof(uintptr_t) * (size_t)attributes.Num)) return;
    if (!Mem::Readable(values.Data, sizeof(float) * (size_t)values.Num)) return;

    int32_t count = attributes.Num < values.Num ? attributes.Num : values.Num;
    for (int32_t i = 0; i < count; i++) {
        uintptr_t attribute = ((uintptr_t*)attributes.Data)[i];
        if (!attribute) continue;

        AttributeInfo info;
        info.name = GetObjectClassName(attribute);
        if (info.name.empty()) continue;
        info.value = ((float*)values.Data)[i];
        info.baseValue = Mem::ReadFloat(attribute + Offsets::FloatAttribute_BaseValue);
        info.shared = shared;
        out.push_back(info);
    }
}

void Engine::ReadHealthAndAttributes(PlayerInfo& info) const {
    if (!IsA(info.pawnPtr, "DominionPlayerCharacter")) return;

    uintptr_t health = Mem::ReadPtr(info.pawnPtr + Offsets::DomPlayerChar_HealthComponent);
    if (!health || !Mem::Readable((void*)health, 0x300)) return;

    info.canDie = Mem::ReadU8(health + Offsets::Health_bCanDie) != 0;

    uintptr_t attributesComponent =
        Mem::ReadPtr(health + Offsets::HealthFromAttr_AttributesComponent);
    if (!attributesComponent || !Mem::Readable((void*)attributesComponent, 0x170)) return;

    ReadAttributeArrays(attributesComponent, Offsets::Attributes_FloatAttributes,
                        Offsets::Attributes_AttributeValues, false, info.attributes);
    ReadAttributeArrays(attributesComponent, Offsets::Attributes_SharedFloatAttributes,
                        Offsets::Attributes_SharedAttributeValues, true, info.attributes);

    for (const AttributeInfo& attribute : info.attributes) {
        if (attribute.name == "Health") {
            info.health = attribute.value;
            info.hasHealth = true;
        } else if (attribute.name == "MaxHealth") {
            info.maxHealth = attribute.value;
            info.hasHealth = true;
        }
    }
    info.isDead = info.hasHealth && info.health <= 0.0f;
}

void Engine::ReadSkills(PlayerInfo& info) const {
    if (!info.controllerPtr) return;
    if (!IsA(info.controllerPtr, "DominionPlayerControllerBase")) return;

    uintptr_t skills = Mem::ReadPtr(info.controllerPtr + Offsets::DomPlayerController_SkillComponent);
    if (!skills || !Mem::Readable((void*)skills, 0x210)) return;

    FUEArray list = {};
    if (!ReadArray(skills + Offsets::Skill_Skills, list, 256)) return;
    if (!Mem::Readable(list.Data, Offsets::FSkill_Size * (size_t)list.Num)) return;

    for (int32_t i = 0; i < list.Num; i++) {
        uintptr_t entry = (uintptr_t)list.Data + (uintptr_t)i * Offsets::FSkill_Size;
        uintptr_t skillData = Mem::ReadPtr(entry + Offsets::FSkill_SkillData);
        if (!skillData) continue;

        SkillInfo skill;
        skill.skill = GetObjectName(skillData);
        if (skill.skill.empty()) continue;
        skill.currentXP = Mem::ReadI32(entry + Offsets::FSkill_CurrentXP);
        info.skills.push_back(skill);
    }
}

std::vector<PlayerInfo> Engine::GetAllPlayers() const {
    std::vector<PlayerInfo> players;
    if (!initialized) return players;

    uintptr_t gameState = GetGameState();
    if (!gameState) {
        LogVerbose("GetAllPlayers: no GameState yet");
        return players;
    }

    FUEArray playerArray = {};
    uintptr_t arrayAddress = gameState + Offsets::GameStateBase_PlayerArray;
    if (!Mem::Readable((void*)arrayAddress, sizeof(FUEArray))) {
        LogVerbose("GetAllPlayers: PlayerArray unreadable");
        return players;
    }
    memcpy(&playerArray, (void*)arrayAddress, sizeof(playerArray));

    if (!playerArray.Data || playerArray.Num <= 0 || playerArray.Num > 512) return players;
    if (!Mem::Readable(playerArray.Data, sizeof(uintptr_t) * (size_t)playerArray.Num)) {
        return players;
    }

    bool isDominionGameState = IsA(gameState, "DominionGameStateBase");

    std::unordered_map<std::string, std::string> characterNames;
    if (isDominionGameState) {
        uintptr_t displayNames = Mem::ReadPtr(gameState + Offsets::DomGameState_DisplayNameComponent);
        if (displayNames) {
            uintptr_t listAddress = displayNames + Offsets::DisplayName_OriginalCharacterNames;
            if (Mem::Readable((void*)listAddress, sizeof(FUEArray))) {
                FUEArray list = {};
                memcpy(&list, (void*)listAddress, sizeof(list));
                if (list.Data && list.Num > 0 && list.Num <= 4096 &&
                    Mem::Readable(list.Data, Offsets::CharDisplayName_Size * (size_t)list.Num)) {
                    for (int32_t i = 0; i < list.Num; i++) {
                        uintptr_t entry = (uintptr_t)list.Data + (uintptr_t)i * Offsets::CharDisplayName_Size;
                        std::string guid = ReadGuid(entry + Offsets::CharDisplayName_CharacterGuid);
                        std::string name = ReadFString(entry + Offsets::CharDisplayName_CharacterName);
                        if (!guid.empty() && !name.empty()) characterNames[guid] = name;
                    }
                }
            }
        }
    }

    uintptr_t* states = (uintptr_t*)playerArray.Data;
    std::unordered_set<uintptr_t> pawnSet;

    for (int32_t i = 0; i < playerArray.Num; i++) {
        uintptr_t playerState = states[i];
        if (!playerState || !Mem::Readable((void*)playerState, 0x370)) continue;

        PlayerInfo info;
        info.playerStatePtr = playerState;
        info.name = ReadFString(playerState + Offsets::PlayerState_PlayerName);
        info.playerId = Mem::ReadI32(playerState + Offsets::PlayerState_PlayerId);
        info.score = Mem::ReadFloat(playerState + Offsets::PlayerState_Score);
        info.startTime = Mem::ReadI32(playerState + Offsets::PlayerState_StartTime);
        info.pingMs = (int32_t)Mem::ReadU8(playerState + Offsets::PlayerState_CompressedPing) * 4;

        uint8_t flags = Mem::ReadU8(playerState + Offsets::PlayerState_Flags);
        info.isSpectator = (flags & (1 << 1)) != 0;
        info.isBot = (flags & (1 << 3)) != 0;
        info.isInactive = (flags & (1 << 5)) != 0;

        info.uniqueNetId = ReadUniqueNetId(playerState + Offsets::PlayerState_UniqueID,
                                           info.uniqueNetIdSource);

        if (IsA(playerState, "DominionPlayerState")) {
            info.characterGuid = ReadGuid(playerState + Offsets::DomPlayerState_CharacterGuid);

            uintptr_t platformData = playerState + Offsets::DomPlayerState_PlatformData;
            info.platform = ReadFString(platformData + Offsets::PlatformData_PlatformName);
            if (info.uniqueNetId.empty()) {
                info.uniqueNetId = ReadUniqueNetId(platformData + Offsets::PlatformData_UniqueID,
                                                   info.uniqueNetIdSource);
            }

            uintptr_t colorAddress = playerState + Offsets::DomPlayerState_PlayerColor;
            if (Mem::Readable((void*)(colorAddress + Offsets::PlayerColor_Color), sizeof(float) * 4)) {
                memcpy(info.color, (void*)(colorAddress + Offsets::PlayerColor_Color), sizeof(info.color));
                info.hasColor = true;
            }
            info.colorHex = ReadFString(colorAddress + Offsets::PlayerColor_HexString);

            auto nameIt = characterNames.find(info.characterGuid);
            if (nameIt != characterNames.end()) info.characterName = nameIt->second;
        }

        uintptr_t pawn = Mem::ReadPtr(playerState + Offsets::PlayerState_PawnPrivate);
        if (pawn && Mem::Readable((void*)pawn, 0x400)) {
            info.spawned = true;
            info.pawnPtr = pawn;
            info.pawnClass = GetObjectClassName(pawn);
            info.controllerPtr = Mem::ReadPtr(pawn + Offsets::Pawn_Controller);
            pawnSet.insert(pawn);

            uintptr_t root = Mem::ReadPtr(pawn + Offsets::Actor_RootComponent);
            if (root && Mem::Readable((void*)root, 0x200)) {
                ReadDoubleTriple(root + Offsets::SceneComp_RelativeLocation, info.x, info.y, info.z);
                ReadDoubleTriple(root + Offsets::SceneComp_RelativeRotation,
                                 info.pitch, info.yaw, info.roll);
                info.hasLocation = true;
            }

            if (IsA(pawn, "Character")) {
                uintptr_t movement = Mem::ReadPtr(pawn + Offsets::Character_CharacterMovement);
                if (movement && Mem::Readable((void*)movement, 0x300)) {
                    ReadDoubleTriple(movement + Offsets::MovementComp_Velocity,
                                     info.vx, info.vy, info.vz);
                    info.hasVelocity = true;
                    info.movementMode = Mem::ReadU8(movement + Offsets::CharMove_MovementMode);
                }
            }
        }

        players.push_back(info);
    }

    if (pawnSet.empty()) return players;

    std::unordered_map<uintptr_t, PawnComponents> byPawn;
    int32_t objectCount = GetObjectCount();
    for (int32_t i = 0; i < objectCount; i++) {
        uintptr_t object = GetObjectByIndex(i);
        if (!object) continue;

        uintptr_t outer = Mem::ReadPtr(object + Offsets::UObject_Outer);
        if (!outer || pawnSet.find(outer) == pawnSet.end()) continue;

        PawnComponents& slots = byPawn[outer];
        if (!slots.sustenance && IsA(object, "SustenanceComponent")) {
            slots.sustenance = object;
        } else if (!slots.hydration && IsA(object, "HydrationComponent")) {
            slots.hydration = object;
        }
    }

    for (PlayerInfo& info : players) {
        if (!info.pawnPtr) continue;

        ReadHealthAndAttributes(info);
        ReadSkills(info);

        auto it = byPawn.find(info.pawnPtr);
        if (it == byPawn.end()) continue;
        const PawnComponents& slots = it->second;

        if (slots.sustenance && Mem::Readable((void*)slots.sustenance, 0x140)) {
            info.hasSustenance = true;
            info.sustenance = Mem::ReadFloat(slots.sustenance + Offsets::SurvivalStat_Current);
            info.maxSustenance = Mem::ReadFloat(slots.sustenance + Offsets::SurvivalStat_CurrentMax);
        }

        if (slots.hydration && Mem::Readable((void*)slots.hydration, 0x140)) {
            info.hasHydration = true;
            info.hydration = Mem::ReadFloat(slots.hydration + Offsets::SurvivalStat_Current);
            info.maxHydration = Mem::ReadFloat(slots.hydration + Offsets::SurvivalStat_CurrentMax);
        }

    }

    return players;
}

}
