#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Mem {

void Init();
bool Readable(const void* addr, size_t size);
void Refresh();

bool InImage(const void* addr);

template <typename T>
inline bool Read(uintptr_t addr, T& out) {
    if (!addr || !Readable((const void*)addr, sizeof(T))) return false;
    std::memcpy(&out, (const void*)addr, sizeof(T));
    return true;
}

inline uintptr_t ReadPtr(uintptr_t addr) {
    uintptr_t value = 0;
    if (!Read(addr, value)) return 0;
    return value;
}

inline float ReadFloat(uintptr_t addr, float fallback = 0.0f) {
    float value = fallback;
    if (!Read(addr, value)) return fallback;
    return value;
}

inline double ReadDouble(uintptr_t addr, double fallback = 0.0) {
    double value = fallback;
    if (!Read(addr, value)) return fallback;
    return value;
}

inline int32_t ReadI32(uintptr_t addr, int32_t fallback = 0) {
    int32_t value = fallback;
    if (!Read(addr, value)) return fallback;
    return value;
}

inline uint8_t ReadU8(uintptr_t addr, uint8_t fallback = 0) {
    uint8_t value = fallback;
    if (!Read(addr, value)) return fallback;
    return value;
}

}
