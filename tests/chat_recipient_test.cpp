// Build with function sections and --gc-sections to isolate recipient lookup
// from the live engine hooks in chat.cpp.
#include "../src/engine/chat.cpp"
#include <array>
#include <cassert>
#include <iostream>

APIConfig g_Config;
namespace Runtime { bool ShuttingDown() { return true; } }

namespace {
std::vector<uintptr_t> objects;
std::vector<uintptr_t> chatObjects;
std::vector<std::pair<uintptr_t, size_t>> regions;
template<size_t N> uintptr_t Register(std::array<uint8_t, N>& data) {
    auto address = reinterpret_cast<uintptr_t>(data.data());
    regions.push_back({address, N});
    return address;
}
void SetPtr(uintptr_t address, uintptr_t value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
}
}

namespace Mem {
bool Readable(const void* address, size_t size) {
    auto p = reinterpret_cast<uintptr_t>(address);
    for (auto region : regions)
        if (p >= region.first && p - region.first <= region.second &&
            size <= region.second - (p - region.first)) return true;
    return false;
}
}
namespace DomEngine {
bool Engine::IsA(uintptr_t object, const char* name) const {
    return std::string(name) == "PlayerChatComponent" &&
           std::find(chatObjects.begin(), chatObjects.end(), object) != chatObjects.end();
}
int32_t Engine::GetObjectCount() const { return objects.size(); }
uintptr_t Engine::GetObjectByIndex(int32_t i) const { return objects.at(i); }
}

int main() {
    DomEngine::Engine engine;
    std::array<uint8_t, 0x1200> controllerData{}, otherControllerData{};
    std::array<uint8_t, 0x150> chatData{}, otherChatData{}, unrelatedData{}, duplicateData{};
    auto controller = Register(controllerData);
    auto otherController = Register(otherControllerData);
    auto chat = Register(chatData);
    auto otherChat = Register(otherChatData);
    auto unrelated = Register(unrelatedData);
    auto duplicate = Register(duplicateData);
    chatObjects = {chat, otherChat, duplicate};
    SetPtr(chat + DomEngine::Offsets::UObject_Outer, controller);
    SetPtr(otherChat + DomEngine::Offsets::UObject_Outer, otherController);
    SetPtr(duplicate + DomEngine::Offsets::UObject_Outer, controller);
    objects = {unrelated, otherChat, chat};

    // Valid SDK offset retains the fast path.
    SetPtr(controller + 0x11D0, chat);
    assert(DomChat::FindPlayersChatComponent(engine, controller) == chat);
    // A stale offset can point at a readable, unrelated component.
    SetPtr(controller + 0x11D0, unrelated);
    assert(DomChat::FindPlayersChatComponent(engine, controller) == chat);
    // Never send another player's component the recipient's message.
    SetPtr(controller + 0x11D0, otherChat);
    assert(DomChat::FindPlayersChatComponent(engine, controller) == chat);
    SetPtr(controller + 0x11D0, 0x1);
    assert(DomChat::FindPlayersChatComponent(engine, controller) == chat);
    objects = {unrelated, otherChat};
    assert(DomChat::FindPlayersChatComponent(engine, controller) == 0);
    objects = {chat, duplicate};
    assert(DomChat::FindPlayersChatComponent(engine, controller) == 0);
    assert(DomChat::FindPlayersChatComponent(engine, 0) == 0);
    std::cout << "chat recipient checks passed\n";
}
