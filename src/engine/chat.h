#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "dom_engine.h"

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

bool Broadcast(const std::string& body, std::string& outError);
// Private message to ONE player (chat component resolved by ownership).
bool SendToPlayer(const DomEngine::PlayerInfo& player, const std::string& body);
bool Tell(const std::string& identifier, const std::string& body,
          std::string& outError);

}
