#pragma once
#include "../engine/dom_engine.h"
#include "../http/http_server.h"
#include "../utils/json.h"

#include <string>

namespace ApiRoutes {

inline bool EngineReady() {
    return DomEngine::g_Engine && DomEngine::g_Engine->IsInitialized();
}

inline HttpResponse Error(int status, const std::string& message) {
    HttpResponse resp(status);
    resp.body = "{\"error\":\"" + Json::Escape(message) + "\"}";
    return resp;
}

inline HttpResponse EngineUnavailable() {
    return Error(503, "Engine not initialized");
}

inline HttpResponse JsonResponse(const std::string& body) {
    HttpResponse resp(200);
    resp.body = body;
    return resp;
}

inline HttpResponse Ok(const std::string& message) {
    return JsonResponse("{\"status\":\"ok\",\"message\":\"" + Json::Escape(message) + "\"}");
}

void RegisterSystem(HttpServer& server);
void RegisterPlayers(HttpServer& server);

void RegisterAll(HttpServer& server);

}
