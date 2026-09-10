#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace GameThread {

bool Initialize(uintptr_t moduleBase, uintptr_t moduleEnd);

bool InstallExecPump();

bool IsReady();

bool HasProcessEvent();

bool FrameLayoutReady();
uintptr_t FrameNodeOffset();
uintptr_t FrameObjectOffset();
uintptr_t FrameCodeOffset();
uintptr_t FrameLocalsOffset();
uintptr_t FramePropertyChainOffset();
uintptr_t ProcessEventAddress();
const std::string& Status();

bool RunSync(const std::function<void()>& work, int timeoutMs = 5000);

void CallFunction(uintptr_t object, uintptr_t function, void* params);

}
