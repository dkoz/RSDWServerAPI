#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace DomChat {

struct Message {
    std::string senderName;
    std::string senderNetId;
    std::string characterGuid;
    int32_t playerId = 0;
    std::string body;
};

struct PlayerEvent {
    std::string senderName;
    std::string senderNetId;
    std::string characterGuid;
    int32_t playerId = 0;
    std::string tag;
};

using Listener = std::function<void(const Message&)>;
using EventListener = std::function<void(const PlayerEvent&)>;

bool Initialize();
bool IsHooked();
const std::string& Status();

void AddListener(Listener listener);
void AddEventListener(EventListener listener);

void Poll();

// Returns true once the message was dispatched to at least one player. Dispatch is not
// confirmation of display - the game sends no acknowledgement for a client RPC.
bool Broadcast(const std::string& body, std::string& outError, int* outDelivered = nullptr);

}
