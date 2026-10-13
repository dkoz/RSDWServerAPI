#include "api_routes.h"
#include "../engine/dom_engine.h"
#include "../utils/json.h"
#include <sstream>

#include "../runtime.h"

#include <cstdio>
#include <sstream>

namespace ApiRoutes {

void RegisterSystem(HttpServer& server) {
    server.Get("/api/hello", [](const HttpRequest&) -> HttpResponse {
        return JsonResponse(R"({"message":"Hello from RSDWRestAPI!","status":"ok"})");
    });

    server.Get("/api/status", [](const HttpRequest&) -> HttpResponse {
        if (!EngineReady()) return EngineUnavailable();

        DomEngine::ServerInfo server = DomEngine::g_Engine->GetServerInfo();
        DomEngine::DiscoveryInfo discovery = DomEngine::g_Engine->GetDiscoveryInfo();

        std::ostringstream oss;
        oss << "{";
        oss << "\"serverName\":\"" << Json::Escape(server.serverName) << "\",";
        oss << "\"worldName\":\"" << Json::Escape(server.worldName) << "\",";
        oss << "\"ownerId\":\"" << Json::Escape(server.ownerId) << "\",";
        oss << "\"serverGuid\":\"" << Json::Escape(server.serverGuid) << "\",";
        oss << "\"isDedicatedServer\":" << (server.isDedicatedServer ? "true" : "false") << ",";
        oss << "\"crossplayEnabled\":" << (server.crossplayEnabled ? "true" : "false") << ",";
        oss << "\"hardcoreState\":" << server.hardcoreState << ",";
        oss << "\"objectCount\":" << discovery.objectCount << ",";
        oss << "\"gObjectsSource\":\"" << Json::Escape(discovery.gObjectsSource) << "\",";
        oss << "\"gNamesSource\":\"" << Json::Escape(discovery.gNamesSource) << "\",";
        oss << "\"worldSource\":\"" << Json::Escape(discovery.worldSource) << "\"";
        oss << "}";
        return JsonResponse(oss.str());
    });

    server.Get("/api/health", [](const HttpRequest&) -> HttpResponse {
        bool ready = EngineReady();

        char uptime[32];
        snprintf(uptime, sizeof(uptime), "%.1f", Runtime::UptimeSeconds());

        std::ostringstream oss;
        oss << "{"
            << "\"status\":\"" << (ready ? "healthy" : "starting") << "\","
            << "\"version\":\"" << Runtime::Version() << "\","
            << "\"uptimeSeconds\":" << uptime << ","
            << "\"engineReady\":" << (ready ? "true" : "false")
            << "}";
        return JsonResponse(oss.str());
    });
}

}
