#include "rcon_commands.h"
#include "../api/serialize.h"
#include "../engine/dom_engine.h"
#include "../runtime.h"

#include <cstdio>
#include <sstream>

namespace Rcon {

namespace {

bool EngineReady() {
    return DomEngine::g_Engine && DomEngine::g_Engine->IsInitialized();
}

std::string Pad(const std::string& value, size_t width) {
    std::string out = value;
    if (out.size() < width) out.append(width - out.size(), ' ');
    return out;
}

}

void RegisterSystemCommands() {
    Register({"help", "help", "List available commands.",
              [](const std::vector<std::string>&) -> std::string {
                  std::ostringstream oss;
                  oss << "RSDWRestAPI " << Runtime::Version() << " - available commands:\n";
                  for (const Command& command : All()) {
                      oss << "  " << Pad(command.usage, 32) << command.description << "\n";
                  }
                  return oss.str();
              }});

    Register({"health", "health", "Mod and engine readiness in one line.",
              [](const std::vector<std::string>&) -> std::string {
                  char buf[256];
                  snprintf(buf, sizeof(buf),
                           "status=%s uptime=%.1fs engine=%s rest=%s:%d rcon=%s:%d",
                           EngineReady() ? "healthy" : "starting", Runtime::UptimeSeconds(),
                           EngineReady() ? "ready" : "waiting",
                           Runtime::RestRunning() ? "up" : "down", Runtime::RestPort(),
                           Runtime::RconRunning() ? "up" : "down", Runtime::RconPort());
                  return buf;
              }});

    Register({"status", "status", "Server identity, world and engine discovery.",
              [](const std::vector<std::string>&) -> std::string {
                  if (!EngineReady()) return "Engine not initialized yet.";

                  DomEngine::ServerInfo server = DomEngine::g_Engine->GetServerInfo();
                  DomEngine::DiscoveryInfo discovery = DomEngine::g_Engine->GetDiscoveryInfo();

                  std::ostringstream oss;
                  oss << "Server name   : " << server.serverName << "\n"
                      << "World         : " << server.worldName << "\n"
                      << "Owner id      : " << server.ownerId << "\n"
                      << "Server guid   : " << server.serverGuid << "\n"
                      << "Dedicated     : " << (server.isDedicatedServer ? "yes" : "no") << "\n"
                      << "Crossplay     : " << (server.crossplayEnabled ? "yes" : "no") << "\n"
                      << "Hardcore state: " << server.hardcoreState << "\n"
                      << "Objects       : " << discovery.objectCount << "\n"
                      << "GObjects      : " << discovery.gObjectsSource << "\n"
                      << "GNames        : " << discovery.gNamesSource << "\n"
                      << "World lookup  : " << discovery.worldSource;
                  return oss.str();
              }});
}

}
