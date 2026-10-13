#include "api_routes.h"
#include "serialize.h"
#include "../engine/bans.h"
#include "../engine/chat.h"
#include "../engine/items.h"
#include "../utils/json.h"

#include <algorithm>
#include <sstream>

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

    server.Post("/api/broadcast", [](const HttpRequest& req) -> HttpResponse {
        if (!EngineReady()) return EngineUnavailable();

        std::string message;
        if (!Json::GetString(req.body, "message", message) || message.empty()) {
            return Error(400, "message required");
        }

        std::string sender = "Server";
        Json::GetString(req.body, "sender", sender);

        std::string line = sender.empty() ? message : "[" + sender + "] " + message;

        std::string error;
        if (!DomChat::Broadcast(line, error)) return Error(500, error);
        return Ok("broadcast sent");
    });

    server.Post("/api/teleport", [](const HttpRequest& req) -> HttpResponse {
        if (!EngineReady()) return EngineUnavailable();

        std::string from, to;
        if (!Json::GetString(req.body, "from", from) || from.empty() ||
            !Json::GetString(req.body, "to", to) || to.empty()) {
            return Error(400, "from and to required (playerId, name, characterName, netId or characterGuid)");
        }

        std::string error;
        if (!DomEngine::g_Engine->TeleportPlayer(from, to, error)) {
            return Error(500, error);
        }
        return Ok("teleported " + from + " to " + to);
    });

    server.Post("/api/tpto", [](const HttpRequest& req) -> HttpResponse {
        if (!EngineReady()) return EngineUnavailable();

        std::string player, xRaw, yRaw, zRaw;
        if (!Json::GetString(req.body, "player", player) || player.empty() ||
            !Json::GetNumber(req.body, "x", xRaw) ||
            !Json::GetNumber(req.body, "y", yRaw) ||
            !Json::GetNumber(req.body, "z", zRaw)) {
            return Error(400, "player, x, y and z required (doubles)");
        }

        std::string error;
        if (!DomEngine::g_Engine->TeleportToCoords(player, std::stod(xRaw),
                                                   std::stod(yRaw), std::stod(zRaw), error)) {
            return Error(500, error);
        }
        return Ok("teleported " + player);
    });

    server.Post("/api/give", [](const HttpRequest& req) -> HttpResponse {
        if (!EngineReady()) return EngineUnavailable();

        std::string player, item;
        if (!Json::GetString(req.body, "player", player) || player.empty()) {
            return Error(400, "player required");
        }
        if (!Json::GetString(req.body, "item", item) || item.empty()) {
            return Error(400, "item required");
        }
        int count = 1;
        Json::GetInt(req.body, "count", count);

        std::string message;
        if (!DomItems::GiveItem(player, item, count, message)) return Error(500, message);
        return Ok(message);
    });

    server.Get("/api/items", [](const HttpRequest& req) -> HttpResponse {
        if (!EngineReady()) return EngineUnavailable();

        std::string filter;
        auto it = req.query.find("search");
        if (it != req.query.end()) filter = Lower(it->second);

        std::ostringstream oss;
        oss << "{\"items\":[";
        bool first = true;
        for (const DomItems::ItemInfo& item : DomItems::ListLoaded()) {
            if (!filter.empty() && Lower(item.name).find(filter) == std::string::npos) continue;
            if (!first) oss << ",";
            first = false;
            oss << "{\"name\":\"" << Json::Escape(item.name) << "\",\"maxStackSize\":"
                << item.maxStackSize << "}";
        }
        oss << "]}";
        return JsonResponse(oss.str());
    });

    server.Post("/api/ban", [](const HttpRequest& req) -> HttpResponse {
        if (!EngineReady()) return EngineUnavailable();

        std::string player;
        if (!Json::GetString(req.body, "player", player) || player.empty()) {
            return Error(400, "player required (name, netId or SteamID64 for offline players)");
        }
        std::string reason = "Banned by an administrator";
        Json::GetString(req.body, "reason", reason);

        std::string message;
        if (!DomBans::Ban(player, reason, message)) return Error(500, message);
        return Ok(message);
    });

    server.Post("/api/unban", [](const HttpRequest& req) -> HttpResponse {
        std::string player;
        if (!Json::GetString(req.body, "player", player) || player.empty()) {
            return Error(400, "player required (name or SteamID64)");
        }

        std::string message;
        if (!DomBans::Unban(player, message)) return Error(404, message);
        return Ok(message);
    });

    server.Get("/api/bans", [](const HttpRequest&) -> HttpResponse {
        std::ostringstream oss;
        oss << "{\"bans\":[";
        bool first = true;
        for (const DomBans::BanEntry& ban : DomBans::All()) {
            if (!first) oss << ",";
            first = false;
            oss << "{\"netId\":\"" << Json::Escape(ban.netId)
                << "\",\"name\":\"" << Json::Escape(ban.name)
                << "\",\"reason\":\"" << Json::Escape(ban.reason)
                << "\",\"bannedAt\":\"" << Json::Escape(ban.bannedAt) << "\"}";
        }
        oss << "]}";
        return JsonResponse(oss.str());
    });
}

}
