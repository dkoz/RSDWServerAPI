#pragma once
#include <cstdint>
#include <string>

namespace NativeCall {

bool IsNative(uintptr_t function);

bool Invoke(uintptr_t context, uintptr_t function, void* params,
            uintptr_t returnValueOffset);

void LogImageSymbolReport();

}
