#include "dom_engine.h"
#include "../utils/logger.h"
#include "../utils/memory.h"

#include <link.h>
#include <vector>

namespace DomEngine {

Engine* g_Engine = nullptr;

namespace {

struct ImageInfo {
    uintptr_t start = 0;
    uintptr_t end = 0;
    std::vector<std::pair<uintptr_t, uintptr_t>> writableSegments;
    bool found = false;
};

int CollectImage(struct dl_phdr_info* info, size_t, void* data) {
    if (info->dlpi_name && info->dlpi_name[0] != '\0') return 0;

    ImageInfo* out = (ImageInfo*)data;
    bool first = true;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)& phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD) continue;

        uintptr_t begin = (uintptr_t)info->dlpi_addr + (uintptr_t)phdr.p_vaddr;
        uintptr_t stop = begin + (uintptr_t)phdr.p_memsz;
        if (first || begin < out->start) out->start = begin;
        if (first || stop > out->end) out->end = stop;
        first = false;

        if (phdr.p_flags & PF_W) out->writableSegments.push_back({begin, stop});
    }

    out->found = !first;
    return 1;
}

bool LooksLikeText(const std::string& value) {
    if (value.empty()) return false;
    for (unsigned char c : value) {
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

}

bool Engine::ShouldLogDiscovery() const {
    return attempts <= 1 || (attempts % 10) == 0;
}

bool Engine::ResolveModule() {
    ImageInfo image;
    dl_iterate_phdr(&CollectImage, &image);
    if (!image.found || !image.start) {
        LogMessage("Engine: could not locate the main executable image");
        return false;
    }

    moduleBase = image.start;
    moduleEnd = image.end;

    if (ShouldLogDiscovery()) {
        LogMessage("Engine: module base " + HexString(moduleBase) +
                   " end " + HexString(moduleEnd) +
                   " size " + std::to_string(moduleEnd - moduleBase));
    }
    return true;
}

bool Engine::ValidateObjectArray(uintptr_t candidate) const {
    if (!Mem::Readable((void*)candidate, 0x20)) return false;

    uintptr_t chunks = Mem::ReadPtr(candidate + Offsets::ObjArray_Objects);
    int32_t maxElements = Mem::ReadI32(candidate + Offsets::ObjArray_MaxElements);
    int32_t numElements = Mem::ReadI32(candidate + Offsets::ObjArray_NumElements);
    int32_t maxChunks = Mem::ReadI32(candidate + Offsets::ObjArray_MaxChunks);
    int32_t numChunks = Mem::ReadI32(candidate + Offsets::ObjArray_NumChunks);

    if (!chunks) return false;
    if (numElements < 1000 || numElements > 20000000) return false;
    if (maxElements < numElements) return false;
    if (numChunks < 1 || numChunks > 512) return false;
    if (maxChunks < numChunks || maxChunks > 512) return false;
    if (numChunks * Offsets::ObjectsPerChunk < numElements) return false;

    if (!Mem::Readable((void*)chunks, sizeof(uintptr_t) * (size_t)numChunks)) return false;

    uintptr_t firstChunk = Mem::ReadPtr(chunks);
    if (!firstChunk || !Mem::Readable((void*)firstChunk, Offsets::ObjectItem_Size)) return false;

    uintptr_t firstObject = Mem::ReadPtr(firstChunk);
    if (!firstObject || !Mem::Readable((void*)firstObject, 0x28)) return false;

    uintptr_t vtable = Mem::ReadPtr(firstObject);
    if (!Mem::InImage((void*)vtable)) return false;

    return true;
}

bool Engine::ResolveObjectArray() {
    uintptr_t candidate = moduleBase + Offsets::GObjects;
    if (ValidateObjectArray(candidate)) {
        gObjects = candidate;
        gObjectsSource = "sdk-offset";
        LogMessage("Engine: GObjects at " + HexString(gObjects) + " (SDK offset)");
        return true;
    }

    if (ShouldLogDiscovery()) {
        LogMessage("Engine: GObjects SDK offset " + HexString(candidate) +
                   " did not validate, scanning writable segments");
    }

    candidate = ScanForObjectArray();
    if (!candidate) {
        if (ShouldLogDiscovery()) LogMessage("Engine: GObjects scan found nothing");
        return false;
    }

    gObjects = candidate;
    gObjectsSource = "segment-scan";
    LogMessage("Engine: GObjects at " + HexString(gObjects) + " (segment scan)");
    return true;
}

uintptr_t Engine::ScanForObjectArray() const {
    ImageInfo image;
    dl_iterate_phdr(&CollectImage, &image);
    if (!image.found) return 0;

    for (const auto& segment : image.writableSegments) {
        LogVerbose("Engine: scanning segment " + HexString(segment.first) + " - " +
                   HexString(segment.second));
        for (uintptr_t addr = segment.first; addr + 0x20 <= segment.second; addr += 8) {
            int32_t numElements = *(const int32_t*)(addr + Offsets::ObjArray_NumElements);
            if (numElements < 1000 || numElements > 20000000) continue;

            int32_t numChunks = *(const int32_t*)(addr + Offsets::ObjArray_NumChunks);
            if (numChunks < 1 || numChunks > 512) continue;

            if (ValidateObjectArray(addr)) return addr;
        }
    }
    return 0;
}

bool Engine::ValidateNamePool(uintptr_t candidate) const {
    if (!Mem::Readable((void*)candidate, Offsets::NamePool_Blocks + 8)) return false;

    uint32_t currentBlock = (uint32_t)Mem::ReadI32(candidate + Offsets::NamePool_CurrentBlock);
    if (currentBlock >= (uint32_t)Offsets::NamePool_MaxBlocks) return false;

    uintptr_t firstBlock = Mem::ReadPtr(candidate + Offsets::NamePool_Blocks);
    if (!firstBlock || !Mem::Readable((void*)firstBlock, 0x40)) return false;

    uint16_t header = 0;
    if (!Mem::Read(firstBlock, header)) return false;
    bool isWide = (header & 1) != 0;
    int32_t length = (int32_t)(header >> 6);
    if (isWide || length != 4) return false;
    if (!Mem::Readable((void*)(firstBlock + 2), 4)) return false;

    char text[5] = {};
    memcpy(text, (void*)(firstBlock + 2), 4);
    return std::string(text) == "None";
}

bool Engine::ResolveNamePool() {
    uintptr_t candidate = moduleBase + Offsets::GNames;
    if (ValidateNamePool(candidate)) {
        gNames = candidate;
        gNamesSource = "sdk-offset";
        LogMessage("Engine: GNames at " + HexString(gNames) + " (SDK offset)");
        return true;
    }

    if (ShouldLogDiscovery()) {
        LogMessage("Engine: GNames SDK offset " + HexString(candidate) +
                   " did not validate, scanning writable segments");
    }

    ImageInfo image;
    dl_iterate_phdr(&CollectImage, &image);
    for (const auto& segment : image.writableSegments) {
        for (uintptr_t addr = segment.first;
             addr + Offsets::NamePool_Blocks + 8 <= segment.second; addr += 8) {
            uint32_t currentBlock = *(const uint32_t*)(addr + Offsets::NamePool_CurrentBlock);
            if (currentBlock >= (uint32_t)Offsets::NamePool_MaxBlocks) continue;

            uintptr_t firstBlock = *(const uintptr_t*)(addr + Offsets::NamePool_Blocks);
            if (!firstBlock || (firstBlock & 7) != 0) continue;

            if (!ValidateNamePool(addr)) continue;
            gNames = addr;
            gNamesSource = "segment-scan";
            LogMessage("Engine: GNames at " + HexString(gNames) + " (segment scan)");
            return true;
        }
    }

    if (ShouldLogDiscovery()) LogMessage("Engine: GNames scan found nothing");
    return false;
}

bool Engine::Initialize() {
    attempts++;
    Mem::Init();

    if (!ResolveModule()) return false;
    if (!ResolveNamePool()) return false;
    if (!ResolveObjectArray()) return false;

    uintptr_t firstObject = GetObjectByIndex(0);
    std::string firstName = GetObjectName(firstObject);
    if (!LooksLikeText(firstName) || firstName.find("CoreUObject") == std::string::npos) {
        if (ShouldLogDiscovery()) {
            LogMessage("Engine: cross-check failed - object 0 resolved to '" + firstName +
                       "', expected the CoreUObject package. Refusing to serve engine data.");
        }
        return false;
    }

    LogMessage("Engine: object 0 resolves to '" + firstName + "', " +
               std::to_string(GetObjectCount()) + " objects tracked");

    initialized = true;
    LogMessage("Engine: initialized");
    return true;
}

DiscoveryInfo Engine::GetDiscoveryInfo() const {
    DiscoveryInfo info;
    info.moduleBase = moduleBase;
    info.moduleEnd = moduleEnd;
    info.gObjects = gObjects;
    info.gNames = gNames;
    info.gObjectsSource = gObjectsSource;
    info.gNamesSource = gNamesSource;
    if (initialized) {
        info.world = GetWorld();
        info.objectCount = GetObjectCount();
    }
    info.worldSource = worldSource;
    return info;
}

}
