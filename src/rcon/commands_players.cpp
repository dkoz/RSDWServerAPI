#include "rcon_commands.h"
#include "../api/serialize.h"
#include "../engine/chat.h"
#include "../engine/dom_engine.h"

#include <algorithm>
#include <sstream>

namespace Rcon {

namespace {

bool EngineReady() {
    return DomEngine::g_Engine && DomEngine::g_Engine->IsInitialized();
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return value;
}

}

void RegisterPlayerCommands() {
    Register({"players", "players [json]", "List connected players. 'json' returns the API payload.",
              [](const std::vector<std::string>& args) -> std::string {
                  if (!EngineReady()) return "Engine not initialized yet.";

                  auto players = DomEngine::g_Engine->GetAllPlayers();
                  if (!args.empty() && Lower(args[0]) == "json") {
                      return Serialize::Players(players);
                  }
                  return Serialize::PlayersTable(players);
              }});

    Register({"playercount", "playercount", "Number of connected players.",
              [](const std::vector<std::string>&) -> std::string {
                  if (!EngineReady()) return "Engine not initialized yet.";
                  return std::to_string(DomEngine::g_Engine->GetAllPlayers().size());
              }});

    Register({"player", "player <name>", "Everything known about one player, as JSON.",
              [](const std::vector<std::string>& args) -> std::string {
                  if (!EngineReady()) return "Engine not initialized yet.";
                  if (args.empty()) return "Usage: player <name>";

                  std::string wanted = args[0];
                  for (size_t i = 1; i < args.size(); i++) wanted += " " + args[i];
                  wanted = Lower(wanted);

                  for (const DomEngine::PlayerInfo& p : DomEngine::g_Engine->GetAllPlayers()) {
                      if (Lower(p.name) == wanted || Lower(p.characterName) == wanted ||
                          Lower(p.uniqueNetId) == wanted) {
                          return Serialize::Player(p);
                      }
                  }
                  return "No connected player matches '" + wanted + "'.";
              }});

    Register({"kick", "kick <player> [reason]", "Disconnect a player from the server.",
              [](const std::vector<std::string>& args) -> std::string {
                  if (!EngineReady()) return "Engine not initialized yet.";
                  if (args.empty()) return "Usage: kick <player> [reason]";

                  std::string reason = "Kicked by an administrator";
                  if (args.size() > 1) {
                      reason = args[1];
                      for (size_t i = 2; i < args.size(); i++) reason += " " + args[i];
                  }

                  std::string error;
                  if (!DomEngine::g_Engine->KickPlayer(args[0], reason, error)) {
                      return "Kick failed: " + error;
                  }
                  return "Kicked " + args[0] + " (" + reason + ")";
              }});

    Register({"broadcast", "broadcast <message>", "Send a message to every player.",
              [](const std::vector<std::string>& args) -> std::string {
                  if (!EngineReady()) return "Engine not initialized yet.";
                  if (args.empty()) return "Usage: broadcast <message>";

                  std::string message = args[0];
                  for (size_t i = 1; i < args.size(); i++) message += " " + args[i];

                  std::string error;
                  if (!DomChat::Broadcast("[Server] " + message, error)) {
                      return "Broadcast failed: " + error;
                  }
                  return "Broadcast sent.";
              }});
}

}
