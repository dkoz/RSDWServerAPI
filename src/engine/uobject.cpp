#include "dom_engine.h"
#include "../utils/logger.h"
#include "../utils/memory.h"

#include <cstdio>
#include <cstring>

namespace DomEngine {

namespace {

void AppendUtf8(std::string& out, uint32_t codepoint) {
    if (codepoint < 0x80) {
        out += (char)codepoint;
    } else if (codepoint < 0x800) {
        out += (char)(0xC0 | (codepoint >> 6));
        out += (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        out += (char)(0xE0 | (codepoint >> 12));
        out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out += (char)(0x80 | (codepoint & 0x3F));
    } else {
        out += (char)(0xF0 | (codepoint >> 18));
        out += (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out += (char)(0x80 | (codepoint & 0x3F));
    }
}

std::string Utf16ToUtf8(const char16_t* data, int32_t length) {
    std::string out;
    out.reserve((size_t)length);
    for (int32_t i = 0; i < length; i++) {
        uint32_t unit = data[i];
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < length) {
            uint32_t low = data[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                AppendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                i++;
                continue;
            }
        }
        AppendUtf8(out, unit);
    }
    return out;
}

bool IsPrintableAscii(const std::string& value) {
    if (value.empty()) return false;
    for (unsigned char c : value) {
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

}

int32_t Engine::GetObjectCount() const {
    if (!gObjects) return 0;
    return Mem::ReadI32(gObjects + Offsets::ObjArray_NumElements);
}

uintptr_t Engine::GetObjectByIndex(int32_t index) const {
    if (!gObjects || index < 0) return 0;

    int32_t numElements = Mem::ReadI32(gObjects + Offsets::ObjArray_NumElements);
    int32_t numChunks = Mem::ReadI32(gObjects + Offsets::ObjArray_NumChunks);
    if (index >= numElements) return 0;

    int32_t chunkIndex = index / Offsets::ObjectsPerChunk;
    int32_t inChunk = index % Offsets::ObjectsPerChunk;
    if (chunkIndex >= numChunks) return 0;

    uintptr_t chunks = Mem::ReadPtr(gObjects + Offsets::ObjArray_Objects);
    if (!chunks) return 0;

    uintptr_t chunk = Mem::ReadPtr(chunks + (uintptr_t)chunkIndex * sizeof(uintptr_t));
    if (!chunk) return 0;

    return Mem::ReadPtr(chunk + (uintptr_t)inChunk * Offsets::ObjectItem_Size);
}

std::string Engine::NameToString(uint32_t comparisonIndex, uint32_t number) const {
    if (!gNames) return "";

    uint32_t blockIndex = comparisonIndex >> Offsets::NameBlockOffsetBits;
    uint32_t inBlock = comparisonIndex & ((1u << Offsets::NameBlockOffsetBits) - 1u);
    if (blockIndex >= (uint32_t)Offsets::NamePool_MaxBlocks) return "";

    uintptr_t block = Mem::ReadPtr(gNames + Offsets::NamePool_Blocks +
                                   (uintptr_t)blockIndex * sizeof(uintptr_t));
    if (!block) return "";

    uintptr_t entry = block + (uintptr_t)inBlock * Offsets::NameEntryStride;
    uint16_t header = 0;
    if (!Mem::Read(entry, header)) return "";

    bool isWide = (header & 1) != 0;
    int32_t length = (int32_t)(header >> 6);
    if (length <= 0 || length > 1024) return "";

    std::string result;
    if (isWide) {
        size_t bytes = (size_t)length * sizeof(char16_t);
        if (!Mem::Readable((void*)(entry + 2), bytes)) return "";
        result = Utf16ToUtf8((const char16_t*)(entry + 2), length);
    } else {
        if (!Mem::Readable((void*)(entry + 2), (size_t)length)) return "";
        result.assign((const char*)(entry + 2), (size_t)length);
    }

    if (number > 0) result += "_" + std::to_string(number - 1);
    return result;
}

std::string Engine::GetObjectName(uintptr_t object) const {
    if (!object || !Mem::Readable((void*)object, 0x28)) return "";
    uint32_t comparisonIndex = (uint32_t)Mem::ReadI32(object + Offsets::UObject_Name);
    uint32_t number = (uint32_t)Mem::ReadI32(object + Offsets::UObject_Name + 4);
    return NameToString(comparisonIndex, number);
}

std::string Engine::GetObjectClassName(uintptr_t object) const {
    if (!object || !Mem::Readable((void*)object, 0x28)) return "";
    uintptr_t classPtr = Mem::ReadPtr(object + Offsets::UObject_Class);
    return GetObjectName(classPtr);
}

bool Engine::IsA(uintptr_t object, const char* className) const {
    if (!object || !className) return false;

    uintptr_t classPtr = Mem::ReadPtr(object + Offsets::UObject_Class);
    for (int depth = 0; classPtr && depth < 32; depth++) {
        if (GetObjectName(classPtr) == className) return true;
        classPtr = Mem::ReadPtr(classPtr + Offsets::UStruct_SuperStruct);
    }
    return false;
}

uintptr_t Engine::FindObject(const char* className, bool wantDefaultObject) const {
    if (!className) return 0;

    int32_t count = GetObjectCount();
    for (int32_t i = 0; i < count; i++) {
        uintptr_t object = GetObjectByIndex(i);
        if (!object) continue;
        if (GetObjectClassName(object) != className) continue;

        bool isDefault = GetObjectName(object).rfind("Default__", 0) == 0;
        if (isDefault != wantDefaultObject) continue;
        return object;
    }
    return 0;
}

uintptr_t Engine::GetWorld() const {
    uintptr_t world = Mem::ReadPtr(moduleBase + Offsets::GWorld);
    if (world && GetObjectClassName(world) == "World" &&
        Mem::ReadPtr(world + Offsets::World_PersistentLevel)) {
        worldSource = "gworld";
        return world;
    }

    int32_t count = GetObjectCount();
    for (int32_t i = 0; i < count; i++) {
        uintptr_t candidate = GetObjectByIndex(i);
        if (!candidate) continue;
        if (GetObjectClassName(candidate) != "World") continue;
        if (GetObjectName(candidate).rfind("Default__", 0) == 0) continue;
        if (!Mem::ReadPtr(candidate + Offsets::World_PersistentLevel)) continue;

        worldSource = "gobjects-sweep";
        return candidate;
    }

    worldSource = "";
    return 0;
}

uintptr_t Engine::GetGameState() const {
    uintptr_t world = GetWorld();
    if (!world) return 0;
    return Mem::ReadPtr(world + Offsets::World_GameState);
}

std::string Engine::ReadFString(uintptr_t address) const {
    if (!address || !Mem::Readable((void*)address, sizeof(FString))) return "";

    FString value = {};
    memcpy(&value, (void*)address, sizeof(FString));
    if (!value.Data || value.Num <= 1 || value.Num > 8192) return "";
    if (value.Max < value.Num) return "";

    size_t bytes = (size_t)value.Num * sizeof(char16_t);
    if (!Mem::Readable(value.Data, bytes)) return "";

    return Utf16ToUtf8(value.Data, value.Num - 1);
}

std::string Engine::ReadGuid(uintptr_t address) const {
    if (!address || !Mem::Readable((void*)address, 16)) return "";

    uint32_t parts[4] = {};
    memcpy(parts, (void*)address, sizeof(parts));
    if (!parts[0] && !parts[1] && !parts[2] && !parts[3]) return "";

    char buf[33];
    snprintf(buf, sizeof(buf), "%08X%08X%08X%08X", parts[0], parts[1], parts[2], parts[3]);
    return buf;
}

std::string Engine::ReadUniqueNetId(uintptr_t netIdRepl, std::string& outSource) const {
    outSource.clear();
    if (!netIdRepl || !Mem::Readable((void*)netIdRepl, Offsets::NetIdRepl_Size)) return "";

    const uintptr_t candidates[] = {Offsets::NetIdRepl_SharedPtrA, Offsets::NetIdRepl_SharedPtrB};
    for (uintptr_t candidate : candidates) {
        uintptr_t netId = Mem::ReadPtr(netIdRepl + candidate);
        if (!netId || !Mem::Readable((void*)netId, Offsets::NetId_FirstMember + sizeof(FString))) {
            continue;
        }

        if (!Mem::InImage((void*)Mem::ReadPtr(netId))) continue;

        std::string value = ReadFString(netId + Offsets::NetId_FirstMember);
        if (!IsPrintableAscii(value) || value.size() > 128) continue;

        outSource = "net-id-string";
        return value;
    }

    return "";
}

}
