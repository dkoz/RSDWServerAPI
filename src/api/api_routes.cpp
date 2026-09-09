#include "api_routes.h"

namespace ApiRoutes {

void RegisterAll(HttpServer& server) {
    RegisterSystem(server);
    RegisterPlayers(server);
}

}
