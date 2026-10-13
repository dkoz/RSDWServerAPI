#include "api_routes.h"
#include "../engine/dom_engine.h"
#include "../utils/json.h"
#include <sstream>
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
        if (!DomEngine::g_Engine || !DomEngine::g_Engine->IsInitialized()) {
            return EngineUnavailable();
        }
        DomEngine::ServerInfo server = DomEngine::g_Engine->GetServerInfo();
        DomEngine::DiscoveryInfo discovery = DomEngine::g_Engine->GetDiscoveryInfo();
        std::ostringstream oss;
        oss << "{\"status\":\"ok\""
            << ",\"serverName\":\"" << Json::Escape(server.serverName) << "\""
            << ",\"worldName\":\"" << Json::Escape(server.worldName) << "\""
            << ",\"ownerId\":\"" << Json::Escape(server.ownerId) << "\""
            << ",\"serverGuid\":\"" << Json::Escape(server.serverGuid) << "\""
            << ",\"isDedicatedServer\":" << (server.isDedicatedServer ? "true" : "false")
            << ",\"crossplayEnabled\":" << (server.crossplayEnabled ? "true" : "false")
            << ",\"hardcoreState\":" << server.hardcoreState
            << ",\"objectCount\":" << discovery.objectCount
            << ",\"gObjectsSource\":\"" << Json::Escape(discovery.gObjectsSource) << "\""
            << ",\"gNamesSource\":\"" << Json::Escape(discovery.gNamesSource) << "\""
            << "}";
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
