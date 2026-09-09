#pragma once
#include "../engine/dom_engine.h"

#include <string>
#include <vector>

namespace Serialize {

std::string Player(const DomEngine::PlayerInfo& player);
std::string Players(const std::vector<DomEngine::PlayerInfo>& players);
std::string Server(const DomEngine::ServerInfo& info);
std::string Discovery(const DomEngine::DiscoveryInfo& info);

std::string PlayersTable(const std::vector<DomEngine::PlayerInfo>& players);

}
