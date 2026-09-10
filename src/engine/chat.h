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

using Listener = std::function<void(const Message&)>;

bool Initialize();
bool IsHooked();
const std::string& Status();

void AddListener(Listener listener);

bool Broadcast(const std::string& body, std::string& outError);

}
