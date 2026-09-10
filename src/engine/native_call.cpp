#include "native_call.h"
#include "dom_engine.h"
#include "process_event.h"
#include "../utils/logger.h"
#include "../utils/memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <elf.h>
#include <link.h>

namespace NativeCall {

namespace {

// Source: RSDWSDK/CppSDK/Assertions.inl
constexpr uintptr_t kFunctionFlags = 0xB0;
constexpr uintptr_t kExecFunction  = 0xD8;
constexpr uint32_t  kFuncNative    = 0x00000400;

// UStruct::ChildProperties, Source: RSDWSDK/CppSDK/Assertions.inl
constexpr uintptr_t kChildProperties = 0x50;

typedef void (*ExecFn)(void* context, void* frame, void* result);

}

bool IsNative(uintptr_t function) {
    if (!function || !Mem::Readable((void*)function, 0xE0)) return false;
    uint32_t flags = (uint32_t)Mem::ReadI32(function + kFunctionFlags);
    return (flags & kFuncNative) != 0;
}

bool Invoke(uintptr_t context, uintptr_t function, void* params,
            uintptr_t returnValueOffset) {
    if (!context || !function || !params) return false;
    if (!Mem::Readable((void*)function, 0xE0)) return false;

    uint32_t flags = (uint32_t)Mem::ReadI32(function + kFunctionFlags);
    if ((flags & kFuncNative) == 0) {
        LogMessage("NativeCall: function is not FUNC_Native, refusing to call its thunk");
        return false;
    }

    uintptr_t exec = Mem::ReadPtr(function + kExecFunction);
    if (!exec || !Mem::InImage((void*)exec)) {
        LogMessage("NativeCall: ExecFunction does not point into the executable image");
        return false;
    }

    if (!GameThread::FrameLayoutReady()) {
        LogMessage("NativeCall: FFrame layout not learned yet, refusing to call a thunk");
        return false;
    }

    uintptr_t chain = Mem::ReadPtr(function + kChildProperties);
    if (!chain) {
        LogMessage("NativeCall: function has no property chain to walk");
        return false;
    }

    uint8_t frame[0x200];
    memset(frame, 0, sizeof(frame));
    *(uintptr_t*)(frame + GameThread::FrameNodeOffset())          = function;
    *(uintptr_t*)(frame + GameThread::FrameObjectOffset())        = context;
    *(uintptr_t*)(frame + GameThread::FrameCodeOffset())          = 0;
    *(uintptr_t*)(frame + GameThread::FrameLocalsOffset())        = (uintptr_t)params;
    *(uintptr_t*)(frame + GameThread::FramePropertyChainOffset()) = chain;

    ((ExecFn)exec)((void*)context, frame, (uint8_t*)params + returnValueOffset);
    return true;
}

void LogImageSymbolReport() {
    FILE* file = fopen("/proc/self/exe", "rb");
    if (!file) {
        LogMessage("Image: could not open /proc/self/exe");
        return;
    }

    Elf64_Ehdr header;
    if (fread(&header, sizeof(header), 1, file) != 1 ||
        memcmp(header.e_ident, ELFMAG, SELFMAG) != 0) {
        LogMessage("Image: /proc/self/exe is not a readable ELF");
        fclose(file);
        return;
    }

    size_t tableBytes = (size_t)header.e_shnum * header.e_shentsize;
    Elf64_Shdr* sections = (Elf64_Shdr*)malloc(tableBytes);
    char* names = nullptr;
    std::string found;
    bool haveSymtab = false;
    bool haveDynsym = false;

    if (sections && header.e_shoff && fseek(file, (long)header.e_shoff, SEEK_SET) == 0 &&
        fread(sections, tableBytes, 1, file) == 1 && header.e_shstrndx < header.e_shnum) {
        Elf64_Shdr& shstr = sections[header.e_shstrndx];
        names = (char*)malloc(shstr.sh_size);
        if (names && fseek(file, (long)shstr.sh_offset, SEEK_SET) == 0 &&
            fread(names, shstr.sh_size, 1, file) == 1) {
            for (int i = 0; i < header.e_shnum; i++) {
                if (sections[i].sh_name >= shstr.sh_size) continue;
                const char* name = names + sections[i].sh_name;
                if (strcmp(name, ".symtab") == 0) haveSymtab = true;
                if (strcmp(name, ".dynsym") == 0) haveDynsym = true;
                if (strcmp(name, ".text") == 0) {
                    found = " .text at " + HexString(sections[i].sh_addr) +
                            " size " + std::to_string(sections[i].sh_size);
                }
            }
        }
    }

    LogMessage("Image: " + std::to_string(header.e_shnum) + " sections, .symtab " +
               (haveSymtab ? "present" : "absent") + ", .dynsym " +
               (haveDynsym ? "present" : "absent") + "," + found);

    free(names);
    free(sections);
    fclose(file);
}

}
