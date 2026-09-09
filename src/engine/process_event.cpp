#include "process_event.h"
#include "dom_engine.h"
#include "../utils/logger.h"
#include "../utils/memory.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <sys/mman.h>
#include <unistd.h>

namespace GameThread {

namespace {

typedef void (*ProcessEventFn)(void* self, void* function, void* params);

ProcessEventFn g_Original = nullptr;
uintptr_t g_ProcessEvent = 0;
int g_VTableIndex = -1;
bool g_Ready = false;
std::string g_Status = "not initialised";

std::mutex g_QueueMutex;
std::condition_variable g_QueueSignal;

struct Job {
    const std::function<void()>* work;
    bool done = false;
};

std::deque<Job*> g_Queue;

const char* kProcessEventSymbols[] = {
    "_ZN7UObject12ProcessEventEP9UFunctionPv",
    "_ZN7UObject12ProcessEventEP9UFunctionPKv",
};

uintptr_t FindByDlsym() {
    for (const char* name : kProcessEventSymbols) {
        void* address = dlsym(RTLD_DEFAULT, name);
        if (address) return (uintptr_t)address;
    }
    return 0;
}

uintptr_t FindInElfFile(const char* wanted, uintptr_t loadBias) {
    FILE* file = fopen("/proc/self/exe", "rb");
    if (!file) return 0;

    uintptr_t result = 0;
    Elf64_Shdr* sections = nullptr;
    char* sectionNames = nullptr;
    char* strtab = nullptr;
    Elf64_Sym* symbols = nullptr;

    do {
        Elf64_Ehdr header;
        if (fread(&header, sizeof(header), 1, file) != 1) break;
        if (memcmp(header.e_ident, ELFMAG, SELFMAG) != 0) break;
        if (header.e_shoff == 0 || header.e_shnum == 0) break;

        size_t tableBytes = (size_t)header.e_shnum * header.e_shentsize;
        sections = (Elf64_Shdr*)malloc(tableBytes);
        if (!sections) break;
        if (fseek(file, (long)header.e_shoff, SEEK_SET) != 0) break;
        if (fread(sections, tableBytes, 1, file) != 1) break;

        Elf64_Shdr& shstr = sections[header.e_shstrndx];
        sectionNames = (char*)malloc(shstr.sh_size);
        if (!sectionNames) break;
        if (fseek(file, (long)shstr.sh_offset, SEEK_SET) != 0) break;
        if (fread(sectionNames, shstr.sh_size, 1, file) != 1) break;

        for (int pass = 0; pass < 2 && !result; pass++) {
            const char* target = (pass == 0) ? ".symtab" : ".dynsym";

            for (int i = 0; i < header.e_shnum; i++) {
                Elf64_Shdr& section = sections[i];
                if (section.sh_name >= shstr.sh_size) continue;
                if (strcmp(sectionNames + section.sh_name, target) != 0) continue;
                if (section.sh_link >= header.e_shnum) continue;
                if (section.sh_entsize == 0) continue;

                Elf64_Shdr& strSection = sections[section.sh_link];
                free(strtab);
                strtab = (char*)malloc(strSection.sh_size);
                if (!strtab) break;
                if (fseek(file, (long)strSection.sh_offset, SEEK_SET) != 0) break;
                if (fread(strtab, strSection.sh_size, 1, file) != 1) break;

                free(symbols);
                symbols = (Elf64_Sym*)malloc(section.sh_size);
                if (!symbols) break;
                if (fseek(file, (long)section.sh_offset, SEEK_SET) != 0) break;
                if (fread(symbols, section.sh_size, 1, file) != 1) break;

                size_t count = section.sh_size / section.sh_entsize;
                for (size_t s = 0; s < count; s++) {
                    if (symbols[s].st_name == 0 || symbols[s].st_value == 0) continue;
                    if (symbols[s].st_name >= strSection.sh_size) continue;
                    if (strcmp(strtab + symbols[s].st_name, wanted) != 0) continue;

                    result = (uintptr_t)symbols[s].st_value + loadBias;
                    break;
                }
                if (result) break;
            }
        }
    } while (false);

    free(symbols);
    free(strtab);
    free(sectionNames);
    free(sections);
    fclose(file);
    return result;
}

uintptr_t GetLoadBias() {
    struct Collect {
        static int Callback(struct dl_phdr_info* info, size_t, void* data) {
            if (info->dlpi_name && info->dlpi_name[0] != '\0') return 0;
            *(uintptr_t*)data = (uintptr_t)info->dlpi_addr;
            return 1;
        }
    };
    uintptr_t bias = 0;
    dl_iterate_phdr(&Collect::Callback, &bias);
    return bias;
}

uintptr_t FindBySymbol(std::string& outSource) {
    uintptr_t address = FindByDlsym();
    if (address) {
        outSource = "dlsym";
        return address;
    }

    uintptr_t bias = GetLoadBias();
    for (const char* name : kProcessEventSymbols) {
        address = FindInElfFile(name, bias);
        if (address) {
            outSource = "elf-symtab";
            return address;
        }
    }
    return 0;
}

void Drain() {
    for (;;) {
        Job* job = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_QueueMutex);
            if (g_Queue.empty()) return;
            job = g_Queue.front();
            g_Queue.pop_front();
        }

        try {
            (*job->work)();
        } catch (...) {
        }

        {
            std::lock_guard<std::mutex> lock(g_QueueMutex);
            job->done = true;
        }
        g_QueueSignal.notify_all();
    }
}

void HookedProcessEvent(void* self, void* function, void* params) {
    bool hasWork;
    {
        std::lock_guard<std::mutex> lock(g_QueueMutex);
        hasWork = !g_Queue.empty();
    }
    if (hasWork) Drain();

    g_Original(self, function, params);
}

int FindVTableIndex(uintptr_t vtable, uintptr_t target) {
    for (int i = 0; i < 200; i++) {
        uintptr_t slot = Mem::ReadPtr(vtable + (uintptr_t)i * sizeof(uintptr_t));
        if (!slot) break;
        if (slot == target) return i;
    }
    return -1;
}

}

namespace {

typedef void (*ExecThunk)(void* context, void* frame, void* result);

// UFunction::ExecFunction, Source: RSDWSDK/CppSDK/Assertions.inl
constexpr uintptr_t kExecFunction = 0xD8;
constexpr uintptr_t kFunctionFlags = 0xB0;
constexpr uint32_t kFuncNative = 0x00000400;

ExecThunk g_PumpOriginal = nullptr;
uintptr_t g_PumpFunction = 0;

// UStruct::ChildProperties, Source: RSDWSDK/CppSDK/Assertions.inl
constexpr uintptr_t kChildProperties = 0x50;

bool g_LayoutReady = false;
uintptr_t g_OffNode = 0;
uintptr_t g_OffObject = 0;
uintptr_t g_OffCode = 0;
uintptr_t g_OffLocals = 0;
uintptr_t g_OffPropertyChain = 0;

void LearnFrameLayout(void* frame, void* context) {
    if (g_LayoutReady || !frame || !g_PumpFunction) return;

    uintptr_t chain = Mem::ReadPtr(g_PumpFunction + kChildProperties);
    uintptr_t base = (uintptr_t)frame;

    uintptr_t node = 0, object = 0, propertyChain = 0;
    for (uintptr_t offset = 0; offset < 0x180; offset += 8) {
        uintptr_t value = Mem::ReadPtr(base + offset);
        if (!value) continue;
        if (!node && value == g_PumpFunction) node = offset;
        if (!object && context && value == (uintptr_t)context) object = offset;
        if (!propertyChain && chain && value == chain) propertyChain = offset;
    }

    if (!node || !propertyChain) {
        LogMessage("GameThread: could not recover the FFrame layout; thunk calls stay disabled");
        return;
    }

    g_OffNode = node;
    g_OffObject = object ? object : node + 0x08;
    g_OffCode = node + 0x10;
    g_OffLocals = node + 0x18;
    g_OffPropertyChain = propertyChain;
    g_LayoutReady = true;

    LogMessage("GameThread: FFrame layout learned - Node +" + HexString(g_OffNode) +
               ", Object +" + HexString(g_OffObject) +
               ", Code +" + HexString(g_OffCode) +
               ", Locals +" + HexString(g_OffLocals) +
               ", PropertyChain +" + HexString(g_OffPropertyChain));
}

const char* kPumpCandidates[] = {
    "ServerUpdateCamera",
    "ServerUpdateLevelVisibility",
    "ServerAcknowledgePossession",
    "ServerCheckClientPossession",
};

void PumpThunk(void* context, void* frame, void* result) {
    LearnFrameLayout(frame, context);

    bool hasWork;
    {
        std::lock_guard<std::mutex> lock(g_QueueMutex);
        hasWork = !g_Queue.empty();
    }
    if (hasWork) Drain();

    if (g_PumpOriginal) g_PumpOriginal(context, frame, result);
}

}

bool InstallExecPump() {
    if (g_Ready) return true;

    DomEngine::Engine* engine = DomEngine::g_Engine;
    if (!engine || !engine->IsInitialized()) {
        g_Status = "engine not initialised";
        return false;
    }

    for (const char* name : kPumpCandidates) {
        uintptr_t function = engine->FindFunction("PlayerController", name);
        if (!function) continue;

        uint32_t flags = (uint32_t)Mem::ReadI32(function + kFunctionFlags);
        if ((flags & kFuncNative) == 0) continue;

        uintptr_t original = Mem::ReadPtr(function + kExecFunction);
        if (!original || !Mem::InImage((void*)original)) continue;

        g_PumpFunction = function;
        g_PumpOriginal = (ExecThunk)original;
        *(uintptr_t*)(function + kExecFunction) = (uintptr_t)&PumpThunk;

        g_Ready = true;
        g_Status = std::string("exec-pump on PlayerController.") + name;
        LogMessage("GameThread: pump installed on PlayerController." + std::string(name) +
                   " (original thunk " + HexString(original) + ")");
        return true;
    }

    g_Status = "no native server RPC available to pump from";
    LogMessage("GameThread: " + g_Status);
    return false;
}

void RemoveExecPump() {
    if (!g_PumpFunction || !g_PumpOriginal) return;
    *(uintptr_t*)(g_PumpFunction + kExecFunction) = (uintptr_t)g_PumpOriginal;
    g_PumpFunction = 0;
    g_PumpOriginal = nullptr;
    g_Ready = false;
}

bool FrameLayoutReady() { return g_LayoutReady; }
uintptr_t FrameNodeOffset() { return g_OffNode; }
uintptr_t FrameObjectOffset() { return g_OffObject; }
uintptr_t FrameCodeOffset() { return g_OffCode; }
uintptr_t FrameLocalsOffset() { return g_OffLocals; }
uintptr_t FramePropertyChainOffset() { return g_OffPropertyChain; }

bool IsReady() { return g_Ready; }
uintptr_t ProcessEventAddress() { return g_ProcessEvent; }
const std::string& Status() { return g_Status; }

bool Initialize(uintptr_t moduleBase, uintptr_t moduleEnd) {
    if (g_Ready) return true;

    DomEngine::Engine* engine = DomEngine::g_Engine;
    if (!engine || !engine->IsInitialized()) {
        g_Status = "engine not initialised";
        return false;
    }

    std::string source;
    uintptr_t address = FindBySymbol(source);
    if (address && address >= moduleBase && address < moduleEnd) {
        g_ProcessEvent = address;
        g_Status = source;
        LogMessage("GameThread: ProcessEvent found via " + source + " at " + HexString(address));
    } else {
        if (address) {
            LogMessage("GameThread: symbol lookup returned " + HexString(address) +
                       ", which is outside the image " + HexString(moduleBase) + " - " +
                       HexString(moduleEnd) + "; ignoring it");
        }
        g_Status = "ProcessEvent not found in .dynsym or .symtab";
        LogMessage("GameThread: " + g_Status + " - kick and every other UFunction "
                   "backed feature stays disabled");
        return false;
    }

    uintptr_t objectClass = engine->FindObject("Class", false);
    uintptr_t sample = engine->GetObjectByIndex(1);
    if (!sample) {
        g_Status = "no sample object to read a vtable from";
        return false;
    }

    uintptr_t vtable = Mem::ReadPtr(sample);
    if (!vtable || !Mem::InImage((void*)vtable)) {
        g_Status = "sample object vtable is not in the image";
        return false;
    }

    g_VTableIndex = FindVTableIndex(vtable, g_ProcessEvent);
    if (g_VTableIndex < 0) {
        g_Status = "ProcessEvent not present in the UObject vtable";
        LogMessage("GameThread: " + g_Status);
        return false;
    }

    uintptr_t slotAddress = vtable + (uintptr_t)g_VTableIndex * sizeof(uintptr_t);
    g_Original = (ProcessEventFn)g_ProcessEvent;

    long pageSize = sysconf(_SC_PAGESIZE);
    uintptr_t page = slotAddress & ~(uintptr_t)(pageSize - 1);
    if (mprotect((void*)page, (size_t)pageSize * 2, PROT_READ | PROT_WRITE) != 0) {
        g_Status = "could not make the vtable page writable";
        LogMessage("GameThread: " + g_Status);
        return false;
    }

    *(uintptr_t*)slotAddress = (uintptr_t)&HookedProcessEvent;
    mprotect((void*)page, (size_t)pageSize * 2, PROT_READ);

    (void)objectClass;
    g_Ready = true;
    g_Status = "ready (" + g_Status + ", vtable slot " + std::to_string(g_VTableIndex) + ")";
    LogMessage("GameThread: ProcessEvent at " + HexString(g_ProcessEvent) +
               ", pump installed at vtable slot " + std::to_string(g_VTableIndex));
    return true;
}

void CallFunction(uintptr_t object, uintptr_t function, void* params) {
    if (!g_Original || !object || !function) return;
    g_Original((void*)object, (void*)function, params);
}

bool RunSync(const std::function<void()>& work, int timeoutMs) {
    if (!g_Ready) return false;

    Job job;
    job.work = &work;

    {
        std::lock_guard<std::mutex> lock(g_QueueMutex);
        g_Queue.push_back(&job);
    }

    std::unique_lock<std::mutex> lock(g_QueueMutex);
    bool ran = g_QueueSignal.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                      [&job]() { return job.done; });
    if (!ran) {
        for (auto it = g_Queue.begin(); it != g_Queue.end(); ++it) {
            if (*it == &job) {
                g_Queue.erase(it);
                break;
            }
        }
    }
    return ran;
}

}
