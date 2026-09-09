#include "memory.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <link.h>

namespace Mem {

namespace {

struct Region {
    uintptr_t start;
    uintptr_t end;
};

std::mutex g_Mutex;
std::vector<Region> g_Regions;
std::chrono::steady_clock::time_point g_LastRefresh;
bool g_Loaded = false;

uintptr_t g_ImageStart = 0;
uintptr_t g_ImageEnd = 0;

void LoadRegionsLocked() {
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) return;

    std::vector<Region> regions;
    regions.reserve(2048);

    char line[1024];
    while (fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0, end = 0;
        char perms[8] = {};
        if (sscanf(line, "%llx-%llx %7s", &start, &end, perms) != 3) continue;
        if (perms[0] != 'r') continue;

        if (!regions.empty() && regions.back().end == (uintptr_t)start) {
            regions.back().end = (uintptr_t)end;
        } else {
            regions.push_back({(uintptr_t)start, (uintptr_t)end});
        }
    }
    fclose(maps);

    std::sort(regions.begin(), regions.end(),
              [](const Region& a, const Region& b) { return a.start < b.start; });

    g_Regions.swap(regions);
    g_LastRefresh = std::chrono::steady_clock::now();
    g_Loaded = true;
}

bool ContainsLocked(uintptr_t addr, size_t size) {
    if (g_Regions.empty()) return false;

    auto it = std::upper_bound(g_Regions.begin(), g_Regions.end(), addr,
                               [](uintptr_t value, const Region& r) { return value < r.start; });
    if (it == g_Regions.begin()) return false;
    --it;
    return addr >= it->start && (addr + size) <= it->end;
}

int ImagePhdrCallback(struct dl_phdr_info* info, size_t, void* data) {
    if (info->dlpi_name && info->dlpi_name[0] != '\0') return 0;

    uintptr_t lowest = 0;
    uintptr_t highest = 0;
    bool first = true;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)& phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD) continue;
        uintptr_t begin = (uintptr_t)info->dlpi_addr + (uintptr_t)phdr.p_vaddr;
        uintptr_t stop = begin + (uintptr_t)phdr.p_memsz;
        if (first || begin < lowest) lowest = begin;
        if (first || stop > highest) highest = stop;
        first = false;
    }
    if (first) return 0;

    uintptr_t* out = (uintptr_t*)data;
    out[0] = lowest;
    out[1] = highest;
    return 1;
}

}

void Init() {
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (!g_Loaded) LoadRegionsLocked();

    if (!g_ImageStart) {
        uintptr_t bounds[2] = {0, 0};
        dl_iterate_phdr(&ImagePhdrCallback, bounds);
        g_ImageStart = bounds[0];
        g_ImageEnd = bounds[1];
    }
}

void Refresh() {
    std::lock_guard<std::mutex> lock(g_Mutex);
    LoadRegionsLocked();
}

bool Readable(const void* addr, size_t size) {
    uintptr_t value = (uintptr_t)addr;
    if (!value || size == 0) return false;
    if (value < 0x10000 || value >= 0x0000800000000000ULL) return false;
    if (value + size < value) return false;

    std::lock_guard<std::mutex> lock(g_Mutex);
    if (!g_Loaded) LoadRegionsLocked();
    if (ContainsLocked(value, size)) return true;

    auto now = std::chrono::steady_clock::now();
    if (now - g_LastRefresh < std::chrono::milliseconds(100)) return false;

    LoadRegionsLocked();
    return ContainsLocked(value, size);
}

bool InImage(const void* addr) {
    uintptr_t value = (uintptr_t)addr;
    return g_ImageStart && value >= g_ImageStart && value < g_ImageEnd;
}

}
