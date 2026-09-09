#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct FString {
    char16_t* Data;
    int32_t Num;   // includes the null terminator
    int32_t Max;
};

struct FUEArray {
    void* Data;
    int32_t Num;
    int32_t Max;
};

struct FVector3d {
    double X, Y, Z;
};

namespace DomEngine {

namespace Offsets {

// Image offsets - Source: RSDWSDK/Dumpspace/OffsetsInfo.json
constexpr uintptr_t GObjects = 0x0DA3D210;
constexpr uintptr_t GNames   = 0x0D981CF8;
constexpr uintptr_t GWorld   = 0x0DB61318;

// TUObjectArray / FUObjectItem - Source: RSDWSDK/CppSDK/SDK/Basic.hpp
constexpr uintptr_t ObjArray_Objects     = 0x00;
constexpr uintptr_t ObjArray_MaxElements = 0x10;
constexpr uintptr_t ObjArray_NumElements = 0x14;
constexpr uintptr_t ObjArray_MaxChunks   = 0x18;
constexpr uintptr_t ObjArray_NumChunks   = 0x1C;
constexpr int32_t   ObjectsPerChunk      = 0x10000;
constexpr uintptr_t ObjectItem_Size      = 0x18;

// FNamePool - Source: RSDWSDK/CppSDK/SDK/Basic.hpp (FNamePool, FNameEntry)
constexpr uintptr_t NamePool_CurrentBlock      = 0x00;
constexpr uintptr_t NamePool_CurrentByteCursor = 0x04;
constexpr uintptr_t NamePool_Blocks            = 0x08;
constexpr int32_t   NameBlockOffsetBits        = 16;
constexpr int32_t   NameEntryStride            = 2;
constexpr int32_t   NamePool_MaxBlocks         = 0x2000;

// UObject / UStruct / UClass - Source: RSDWSDK/CppSDK/Assertions.inl
constexpr uintptr_t UObject_Flags     = 0x08;
constexpr uintptr_t UObject_Index     = 0x0C;
constexpr uintptr_t UObject_Class     = 0x10;
constexpr uintptr_t UObject_Name      = 0x18;
constexpr uintptr_t UObject_Outer     = 0x20;
constexpr uintptr_t UStruct_SuperStruct = 0x40;
constexpr uintptr_t UClass_ClassDefaultObject = 0x110;

// UWorld - Source: RSDWSDK/CppSDK/Assertions.inl
constexpr uintptr_t World_PersistentLevel   = 0x30;
constexpr uintptr_t World_AuthorityGameMode = 0x1D8;
constexpr uintptr_t World_GameState         = 0x1E0;

// AGameStateBase - Source: RSDWSDK/CppSDK/Assertions.inl
constexpr uintptr_t GameStateBase_AuthorityGameMode = 0x2C8;
constexpr uintptr_t GameStateBase_PlayerArray       = 0x2D8;

// ADominionGameStateBase - Source: RSDWSDK/CppSDK/SDK/Dominion_classes.hpp
constexpr uintptr_t DomGameState_bIsDedicatedServer  = 0x314;
constexpr uintptr_t DomGameState_bIsCrossplayEnabled = 0x315;
constexpr uintptr_t DomGameState_KickedUsers         = 0x348;
constexpr uintptr_t DomGameState_BannedUsers         = 0x358;
constexpr uintptr_t DomGameState_DisplayNameComponent = 0x398;
constexpr uintptr_t DomGameState_WorldHardcoreState  = 0x3D0;

// UDisplayNameComponent - Source: RSDWSDK/CppSDK/SDK/Dominion_classes.hpp
constexpr uintptr_t DisplayName_OriginalCharacterNames = 0xD8;

// FDomCharacterDisplayName - Source: RSDWSDK/CppSDK/SDK/Dominion_structs.hpp
constexpr uintptr_t CharDisplayName_UniqueID      = 0x00; // FUniqueNetIdRepl
constexpr uintptr_t CharDisplayName_CharacterGuid = 0x30; // FGuid
constexpr uintptr_t CharDisplayName_CharacterName = 0x40; // FString
constexpr uintptr_t CharDisplayName_Size          = 0x50;

// APlayerState - Source: RSDWSDK/CppSDK/SDK/Engine_classes.hpp
constexpr uintptr_t PlayerState_Score          = 0x2C0;
constexpr uintptr_t PlayerState_PlayerId       = 0x2C4;
constexpr uintptr_t PlayerState_CompressedPing = 0x2C8;
constexpr uintptr_t PlayerState_Flags          = 0x2CA; // bit 1 spectator, 3 bot, 5 inactive
constexpr uintptr_t PlayerState_StartTime      = 0x2CC;
constexpr uintptr_t PlayerState_UniqueID       = 0x2D0; // FUniqueNetIdRepl, 0x30
constexpr uintptr_t PlayerState_PawnPrivate    = 0x338;
constexpr uintptr_t PlayerState_PlayerName     = 0x350;

// ADominionPlayerState - Source: RSDWSDK/CppSDK/SDK/Dominion_classes.hpp
constexpr uintptr_t DomPlayerState_CharacterGuid = 0x3D0; // FDomCharacterGuid -> FGuid
constexpr uintptr_t DomPlayerState_PlayerColor   = 0x3E0; // FPlayerColor
constexpr uintptr_t DomPlayerState_PlatformData  = 0x408; // FDomPlatformData

// FPlayerColor - Source: RSDWSDK/CppSDK/SDK/Dominion_structs.hpp
constexpr uintptr_t PlayerColor_Color     = 0x00; // FLinearColor, 4 floats
constexpr uintptr_t PlayerColor_HexString = 0x10; // FString

// FDomPlatformData - Source: RSDWSDK/CppSDK/SDK/Dominion_structs.hpp
constexpr uintptr_t PlatformData_UniqueID     = 0x00; // FUniqueNetIdRepl, 0x30
constexpr uintptr_t PlatformData_PlatformName = 0x30; // FString

constexpr uintptr_t NetIdRepl_SharedPtrA  = 0x00;
constexpr uintptr_t NetIdRepl_SharedPtrB  = 0x08;
constexpr uintptr_t NetIdRepl_Size        = 0x30;
constexpr uintptr_t NetId_FirstMember     = 0x18;

// AActor / APawn / AController / ACharacter - Source: RSDWSDK/CppSDK/Assertions.inl
constexpr uintptr_t Actor_Owner          = 0x160;
constexpr uintptr_t Actor_Role           = 0x170;
constexpr uintptr_t Actor_NetDormancy    = 0x171;
constexpr uintptr_t Actor_RootComponent  = 0x1D0;
constexpr uintptr_t Pawn_PlayerState     = 0x2E0;
constexpr uintptr_t Pawn_Controller      = 0x2F0;
constexpr uintptr_t Controller_PlayerState = 0x2C8;
constexpr uintptr_t Controller_Pawn        = 0x300;
constexpr uintptr_t Character_Mesh              = 0x340;
constexpr uintptr_t Character_CharacterMovement = 0x348;

// Source: RSDWSDK/CppSDK/Assertions.inl. Locations and rotations are doubles:
constexpr uintptr_t SceneComp_RelativeLocation = 0x148;
constexpr uintptr_t SceneComp_RelativeRotation = 0x160;
constexpr uintptr_t MovementComp_Velocity      = 0xD8;
constexpr uintptr_t CharMove_MovementMode      = 0x241;

// pointers - Source: RSDWSDK/CppSDK/Assertions.inl. These are real pointers,
constexpr uintptr_t DomPlayerChar_HealthComponent = 0x13F0; // UPlayerHealthComponent*
constexpr uintptr_t DomPlayerController_SkillComponent = 0x798; // USkillComponent*

// Source: RSDWSDK/CppSDK/SDK/Dominion_classes.hpp:25533
constexpr uintptr_t HealthFromAttr_AttributesComponent = 0x188;

// UDominionAttributesComponent - Source: RSDWSDK/CppSDK/Assertions.inl.
constexpr uintptr_t Attributes_FloatAttributes       = 0xD0;
constexpr uintptr_t Attributes_SharedFloatAttributes = 0xE0;
constexpr uintptr_t Attributes_AttributeValues       = 0xF0;
constexpr uintptr_t Attributes_SharedAttributeValues = 0x100;
constexpr uintptr_t FloatAttribute_BaseValue         = 0x38;

// UHealthComponent - Source: RSDWSDK/CppSDK/SDK/Dominion_classes.hpp
constexpr uintptr_t Health_MaxHealth           = 0x150;
constexpr uintptr_t Health_AuthoritativeHealth = 0x154;
constexpr uintptr_t Health_bCanDie             = 0x158;

// shape - Source: RSDWSDK/CppSDK/SDK/Dominion_classes.hpp
constexpr uintptr_t SurvivalStat_Current    = 0x130;
constexpr uintptr_t SurvivalStat_CurrentMax = 0x134;
constexpr uintptr_t SurvivalStat_DecayBuffer = 0x138;

// USkillComponent - Source: RSDWSDK/CppSDK/SDK/Dominion_classes.hpp
constexpr uintptr_t Skill_Skills = 0x188; // TArray<FSkill>
constexpr uintptr_t FSkill_CurrentXP = 0x00;
constexpr uintptr_t FSkill_SkillData = 0x08;
constexpr uintptr_t FSkill_Size      = 0x10;

// UDedicatedServerSettings - Source: RSDWSDK/CppSDK/SDK/Dominion_classes.hpp
constexpr uintptr_t DediSettings_OwnerId         = 0x28;
constexpr uintptr_t DediSettings_ServerGuid      = 0x68;
constexpr uintptr_t DediSettings_ServerName      = 0x78;
constexpr uintptr_t DediSettings_DefaultWorldName = 0xA8;

}

struct SkillInfo {
    std::string skill;
    int32_t currentXP = 0;
};

struct AttributeInfo {
    std::string name;
    float value = 0.0f;
    float baseValue = 0.0f;
    bool shared = false;
};

struct PlayerInfo {
    std::string name;
    int32_t playerId = 0;
    int32_t pingMs = 0;
    float score = 0.0f;
    int32_t startTime = 0;
    bool isSpectator = false;
    bool isBot = false;
    bool isInactive = false;

    std::string characterGuid;
    std::string characterName;
    std::string platform;
    std::string uniqueNetId;
    std::string uniqueNetIdSource;
    std::string colorHex;
    float color[4] = {0, 0, 0, 0};
    bool hasColor = false;

    bool spawned = false;
    std::string pawnClass;
    bool hasLocation = false;
    double x = 0, y = 0, z = 0;
    double pitch = 0, yaw = 0, roll = 0;
    bool hasVelocity = false;
    double vx = 0, vy = 0, vz = 0;
    uint8_t movementMode = 0;

    bool hasHealth = false;
    float health = 0.0f;
    float maxHealth = 0.0f;
    bool canDie = false;
    bool isDead = false;

    bool hasSustenance = false;
    float sustenance = 0.0f;
    float maxSustenance = 0.0f;

    bool hasHydration = false;
    float hydration = 0.0f;
    float maxHydration = 0.0f;

    std::vector<SkillInfo> skills;
    std::vector<AttributeInfo> attributes;

    uintptr_t playerStatePtr = 0;
    uintptr_t pawnPtr = 0;
    uintptr_t controllerPtr = 0;
};

struct ServerInfo {
    std::string ownerId;
    std::string serverName;
    std::string worldName;
    std::string serverGuid;
    bool isDedicatedServer = false;
    bool crossplayEnabled = false;
    int32_t hardcoreState = -1;
    bool valid = false;
};

struct DiscoveryInfo {
    uintptr_t moduleBase = 0;
    uintptr_t moduleEnd = 0;
    uintptr_t gObjects = 0;
    uintptr_t gNames = 0;
    uintptr_t world = 0;
    std::string gObjectsSource;
    std::string gNamesSource;
    std::string worldSource;
    int32_t objectCount = 0;
};

class Engine {
private:
    uintptr_t moduleBase = 0;
    uintptr_t moduleEnd = 0;
    uintptr_t gObjects = 0;
    uintptr_t gNames = 0;
    bool initialized = false;

    std::string gObjectsSource;
    std::string gNamesSource;
    mutable std::string worldSource;
    int attempts = 0;

    bool ShouldLogDiscovery() const;

    bool ResolveModule();
    bool ResolveNamePool();
    bool ResolveObjectArray();
    bool ValidateNamePool(uintptr_t candidate) const;
    bool ValidateObjectArray(uintptr_t candidate) const;
    uintptr_t ScanForObjectArray() const;

public:
    bool Initialize();
    bool IsInitialized() const { return initialized; }

    uintptr_t GetModuleBase() const { return moduleBase; }
    uintptr_t GetGObjects() const { return gObjects; }
    uintptr_t GetGNames() const { return gNames; }

    int32_t GetObjectCount() const;
    uintptr_t GetObjectByIndex(int32_t index) const;
    std::string NameToString(uint32_t comparisonIndex, uint32_t number) const;
    std::string GetObjectName(uintptr_t object) const;
    std::string GetObjectClassName(uintptr_t object) const;
    bool IsA(uintptr_t object, const char* className) const;
    uintptr_t FindObject(const char* className, bool wantDefaultObject) const;
    uintptr_t GetWorld() const;
    uintptr_t GetGameState() const;

    std::string ReadFString(uintptr_t address) const;
    std::string ReadGuid(uintptr_t address) const;
    std::string ReadUniqueNetId(uintptr_t netIdRepl, std::string& outSource) const;

    std::vector<PlayerInfo> GetAllPlayers() const;

private:
    void ReadAttributeArrays(uintptr_t attributesComponent, uintptr_t attributesOffset,
                             uintptr_t valuesOffset, bool shared,
                             std::vector<AttributeInfo>& out) const;
    void ReadHealthAndAttributes(PlayerInfo& info) const;
    void ReadSkills(PlayerInfo& info) const;

public:

    bool KickPlayer(const std::string& identifier, const std::string& reason,
                    std::string& outError) const;
    uintptr_t FindFunction(const char* className, const char* functionName) const;

    void RememberKick(const std::string& netId, const std::string& name,
                      const std::string& reason) const;
    bool IsKicked(const std::string& netId) const;
    std::vector<std::string> KickedNetIds() const;
    bool ClearKick(const std::string& netId) const;
    void EnforceKicks() const;

    ServerInfo GetServerInfo() const;
    DiscoveryInfo GetDiscoveryInfo() const;
};

extern Engine* g_Engine;

}
