// Exercises PlayerChatComponent discovery (issues #5, #6) against a synthetic UObject world:
// a real FNamePool layout, classes with SuperStruct chains and PropertiesSize, controllers,
// components and a chunked object registry. chat.cpp is included directly so its internal
// functions can be called. Run with `make test`.

#include <cstdint>
#include <string>
#include <vector>

#define private public  // reach Engine's discovery state without changing the header
#include "../src/engine/dom_engine.h"
#undef private

#include "../src/engine/chat.cpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace DomEngine;
using namespace DomChat;  // chat.cpp's internals live in DomChat's anonymous namespace
namespace O = DomEngine::Offsets;

namespace {

int g_Failures = 0;

void Check(bool ok, const char* what) {
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_Failures++;
}

// vtables must live inside the executable image for LooksLikeObject() to accept them.
uintptr_t g_FakeVTable[8];

// ---- FNamePool: one block, entries at 2-byte stride, header = (len << 6) | wide ----------
alignas(8) uint8_t g_NameBlock[0x10000];  // one full FNamePool block (16-bit offsets)
uintptr_t g_NamePool[1 + 1 + 0x2000];  // CurrentBlock/ByteCursor, then Blocks[]
size_t g_NameCursor = 0;
std::map<std::string, uint32_t> g_Interned;

uint32_t AddName(const char* text) {
    auto known = g_Interned.find(text);
    if (known != g_Interned.end()) return known->second;
    size_t len = strlen(text);
    if (g_NameCursor + 2 + len + 2 > sizeof(g_NameBlock)) {
        fprintf(stderr, "test name block exhausted\n");
        abort();
    }
    uint32_t index = (uint32_t)(g_NameCursor / 2);
    g_Interned[text] = index;
    uint16_t header = (uint16_t)(len << 6);
    memcpy(g_NameBlock + g_NameCursor, &header, 2);
    memcpy(g_NameBlock + g_NameCursor + 2, text, len);
    g_NameCursor += (2 + len + 1) & ~(size_t)1;
    return index;
}

// ---- Objects ---------------------------------------------------------------------------
std::vector<uintptr_t> g_Registry;

uintptr_t NewObject(size_t size, uintptr_t cls, const char* name, uintptr_t outer) {
    void* memory = nullptr;
    if (posix_memalign(&memory, 16, size < 0x80 ? 0x80 : size) != 0) abort();
    memset(memory, 0, size < 0x80 ? 0x80 : size);
    uintptr_t object = (uintptr_t)memory;
    *(uintptr_t*)(object + 0) = (uintptr_t)g_FakeVTable;
    *(uintptr_t*)(object + O::UObject_Class) = cls;
    *(uint32_t*)(object + O::UObject_Name) = AddName(name);
    *(uintptr_t*)(object + O::UObject_Outer) = outer;
    g_Registry.push_back(object);
    return object;
}

uintptr_t NewClass(const char* name, uintptr_t super, int32_t propertiesSize) {
    uintptr_t cls = NewObject(0x80, 0, name, 0);
    *(uintptr_t*)(cls + O::UStruct_SuperStruct) = super;
    *(int32_t*)(cls + O::UStruct_PropertiesSize) = propertiesSize;
    return cls;
}

// Contiguous storage, so a controller can be followed directly by a neighbour object.
uintptr_t PlaceObject(uintptr_t memory, uintptr_t cls, const char* name, uintptr_t outer) {
    *(uintptr_t*)(memory + 0) = (uintptr_t)g_FakeVTable;
    *(uintptr_t*)(memory + O::UObject_Class) = cls;
    *(uint32_t*)(memory + O::UObject_Name) = AddName(name);
    *(uintptr_t*)(memory + O::UObject_Outer) = outer;
    g_Registry.push_back(memory);
    return memory;
}

// TUObjectArray with one chunk of FUObjectItem (object pointer first, 0x18 stride).
uint8_t g_ObjArray[0x20];
std::vector<uint8_t> g_Chunk;
uintptr_t g_Chunks[1];

void PublishRegistry() {
    g_Chunk.assign(O::ObjectItem_Size * g_Registry.size() + 64, 0);
    for (size_t i = 0; i < g_Registry.size(); i++) {
        memcpy(g_Chunk.data() + i * O::ObjectItem_Size, &g_Registry[i], sizeof(uintptr_t));
    }
    g_Chunks[0] = (uintptr_t)g_Chunk.data();
    uintptr_t chunks = (uintptr_t)g_Chunks;
    int32_t count = (int32_t)g_Registry.size();
    memcpy(g_ObjArray + O::ObjArray_Objects, &chunks, 8);
    memcpy(g_ObjArray + O::ObjArray_MaxElements, &count, 4);
    memcpy(g_ObjArray + O::ObjArray_NumElements, &count, 4);
    int32_t one = 1;
    memcpy(g_ObjArray + O::ObjArray_MaxChunks, &one, 4);
    memcpy(g_ObjArray + O::ObjArray_NumChunks, &one, 4);
}

void ResetCaches() {
    std::lock_guard<std::mutex> lock(g_ComponentMutex);
    g_ChatOffsetByClass.clear();
    g_ChatDiscoveryFailedAt.clear();
}

}

int main() {
    setenv("RSDWAPI_DIR", "/tmp/rsdwapi-test", 1);
    Mem::Init();

    AddName("None");
    g_NamePool[1] = (uintptr_t)g_NameBlock;  // Blocks[0] sits at +0x08

    uintptr_t uobject = NewClass("Object", 0, 0x28);
    uintptr_t actorComponent = NewClass("ActorComponent", uobject, 0xB0);
    uintptr_t chatClass = NewClass("PlayerChatComponent", actorComponent, 0x150);
    uintptr_t buildClass = NewClass("BuildInteractionComponent", actorComponent, 0x150);
    uintptr_t pcBase = NewClass("PlayerController", uobject, 0x900);
    uintptr_t pcClass = NewClass("BP_PlayerController_C", pcBase, 0x1500);
    uintptr_t pcSmall = NewClass("BP_SmallController_C", pcBase, 0x1300);
    uintptr_t pcNoSize = NewClass("BP_NoSizeController_C", pcBase, 0);

    Engine engine;
    engine.gNames = (uintptr_t)g_NamePool;
    engine.initialized = true;
    g_Engine = &engine;

    Check(engine.IsA(NewObject(0x80, chatClass, "probe", 0), "ActorComponent"),
          "fake name pool and class chain resolve through Engine::IsA");

    // 1. The 10-14 patch layout: 0x1288 holds BuildInteractionComponent, the real chat
    //    component sits at 0x12E0. The old code sent broadcasts to the build component.
    {
        uintptr_t pc = NewObject(0x1500, pcClass, "PC_A", 0);
        *(uintptr_t*)(pc + 0x1288) = NewObject(0x150, buildClass, "BuildInteraction", pc);
        uintptr_t chat = NewObject(0x150, chatClass, "PlayerChat", pc);
        *(uintptr_t*)(pc + 0x12E0) = chat;
        std::string via;
        Mem::Refresh();
        uintptr_t offset = DiscoverChatOffset(pc, via);
        Check(offset == 0x12E0 && via == "hint", "issue #5/#6 layout: chat component found at +0x12E0, not +0x1288");
        Check(ResolveChatComponent(pc) == chat, "ResolveChatComponent returns the PlayerChatComponent");
    }

    // 2. A future patch moves it somewhere no hint covers: the member scan finds it.
    ResetCaches();
    {
        uintptr_t pc = NewObject(0x1500, pcClass, "PC_B", 0);
        *(uintptr_t*)(pc + 0x1288) = NewObject(0x150, buildClass, "BuildInteraction", pc);
        uintptr_t chat = NewObject(0x150, chatClass, "PlayerChat", pc);
        *(uintptr_t*)(pc + 0x1410) = chat;
        std::string via;
        Mem::Refresh();
        Check(DiscoverChatOffset(pc, via) == 0x1410 && via == "member scan",
              "moved component found by the member scan");
        Check(ResolveChatComponent(pc) == chat, "and resolved");
    }

    // 3. Our component is missing; the object allocated right after the controller holds a
    //    different player's chat component. A fixed-size scan would cross into it.
    ResetCaches();
    {
        void* memory = nullptr;
        if (posix_memalign(&memory, 16, 0x3000) != 0) return 2;
        memset(memory, 0, 0x3000);
        uintptr_t pc = PlaceObject((uintptr_t)memory, pcSmall, "PC_Small", 0);  // PropertiesSize 0x1300
        uintptr_t neighbour = PlaceObject((uintptr_t)memory + 0x1300, pcSmall, "PC_Neighbour", 0);
        uintptr_t theirChat = NewObject(0x150, chatClass, "TheirChat", neighbour);
        *(uintptr_t*)(neighbour + 0x300) = theirChat;  // lands at controller +0x1600
        std::string via;
        Mem::Refresh();
        Check(DiscoverChatOffset(pc, via) == 0, "scan stops at PropertiesSize: neighbour's component not taken");

        // Same shape but the foreign component is *inside* our controller's range: ownership
        // (Outer) must still reject it.
        *(uintptr_t*)(pc + 0x800) = theirChat;
        Check(DiscoverChatOffset(pc, via) == 0, "a foreign component inside our members is rejected by Outer");
    }

    // 4. Two distinct owned components: ambiguous, refuse.
    ResetCaches();
    {
        uintptr_t pc = NewObject(0x1500, pcClass, "PC_Two", 0);
        *(uintptr_t*)(pc + 0x1300) = NewObject(0x150, chatClass, "Chat1", pc);
        *(uintptr_t*)(pc + 0x1308) = NewObject(0x150, chatClass, "Chat2", pc);
        std::string via;
        Mem::Refresh();
        Check(DiscoverChatOffset(pc, via) == 0, "two owned components: refuses to guess");
    }

    // 5. Cache behaviour across controllers of one class.
    ResetCaches();
    {
        uintptr_t first = NewObject(0x1500, pcClass, "PC_First", 0);
        uintptr_t chat = NewObject(0x150, chatClass, "Chat", first);
        *(uintptr_t*)(first + 0x1410) = chat;
        Check(ResolveChatComponent(first) == chat, "first controller discovered and cached");

        uintptr_t joining = NewObject(0x1500, pcClass, "PC_Joining", 0);  // component not created yet
        Check(ResolveChatComponent(joining) == 0, "joining player without a component: no result");
        {
            std::lock_guard<std::mutex> lock(g_ComponentMutex);
            Check(g_ChatOffsetByClass.count(pcClass) == 1, "...and the class cache survives it");
        }
        uintptr_t joinedChat = NewObject(0x150, chatClass, "ChatLater", joining);
        *(uintptr_t*)(joining + 0x1410) = joinedChat;
        Check(ResolveChatComponent(joining) == joinedChat, "once created, it resolves from the cache");

        // Something else now sits at the cached offset (a patch, or a different subclass).
        uintptr_t moved = NewObject(0x1500, pcClass, "PC_Moved", 0);
        uintptr_t movedChat = NewObject(0x150, chatClass, "ChatMoved", moved);
        *(uintptr_t*)(moved + 0x1410) = NewObject(0x150, buildClass, "NotChat", moved);
        *(uintptr_t*)(moved + 0x1418) = movedChat;
        Check(ResolveChatComponent(moved) == movedChat, "stale cached offset is dropped and rediscovered");
    }

    // 6. No PropertiesSize available: member scan is skipped, the object registry is used.
    ResetCaches();
    {
        uintptr_t pc = NewObject(0x1500, pcNoSize, "PC_NoSize", 0);
        uintptr_t chat = NewObject(0x150, chatClass, "RegistryChat", pc);
        *(uintptr_t*)(pc + 0x1438) = chat;
        PublishRegistry();
        engine.gObjects = (uintptr_t)g_ObjArray;
        Mem::Refresh();
        Check(engine.GetObjectByIndex(0) == g_Registry[0], "fake object registry readable");
        Check(ResolveChatComponent(pc) == chat, "registry fallback finds the owned component");
        std::lock_guard<std::mutex> lock(g_ComponentMutex);
        Check(g_ChatOffsetByClass[pcNoSize] == 0x1438, "...and works back to its member offset");
    }

    // 7. Failure is throttled, not repeated on every call.
    ResetCaches();
    {
        uintptr_t pc = NewObject(0x1500, pcClass, "PC_None", 0);
        PublishRegistry();
        Check(ResolveChatComponent(pc) == 0, "controller without a chat component: no result");
        std::lock_guard<std::mutex> lock(g_ComponentMutex);
        Check(g_ChatDiscoveryFailedAt.count(pcClass) == 1, "...and the failure is recorded for the 15s back-off");
    }

    printf("\n%s (%d failure%s)\n", g_Failures ? "FAILED" : "ALL PASSED", g_Failures,
           g_Failures == 1 ? "" : "s");
    return g_Failures ? 1 : 0;
}
