#include "api_routes.h"
#include "serialize.h"
#include "../utils/json.h"

#include <algorithm>

namespace ApiRoutes {

namespace {

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return value;
}

}

void RegisterPlayers(HttpServer& server) {
    server.Get("/api/players", [](const HttpRequest& req) -> HttpResponse {
        if (!EngineReady()) return EngineUnavailable();

        auto players = DomEngine::g_Engine->GetAllPlayers();

        auto filter = [&](const char* key, std::string DomEngine::PlayerInfo::*field) {
            auto it = req.query.find(key);
            if (it == req.query.end() || it->second.empty()) return;
            std::string wanted = Lower(it->second);
            players.erase(std::remove_if(players.begin(), players.end(),
                                         [&](const DomEngine::PlayerInfo& p) {
                                             return Lower(p.*field) != wanted;
                                         }),
                          players.end());
        };

        filter("name", &DomEngine::PlayerInfo::name);
        filter("characterName", &DomEngine::PlayerInfo::characterName);
        filter("netId", &DomEngine::PlayerInfo::uniqueNetId);
        filter("characterGuid", &DomEngine::PlayerInfo::characterGuid);

        return JsonResponse(Serialize::Players(players));
    });

    server.Post("/api/kick", [](const HttpRequest& req) -> HttpResponse {
        if (!EngineReady()) return EngineUnavailable();

        std::string target;
        if (!Json::GetString(req.body, "player", target) || target.empty()) {
            return Error(400, "player required (name, characterName, netId or playerId)");
        }

        std::string reason = "Kicked by an administrator";
        Json::GetString(req.body, "reason", reason);

        std::string error;
        if (!DomEngine::g_Engine->KickPlayer(target, reason, error)) {
            return Error(500, error);
        }
        return Ok("kicked " + target);
    });
}

}
