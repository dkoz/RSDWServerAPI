#include "api/api_routes.h"
#include "config/config.h"
#include "discord/webhook.h"
#include "engine/bans.h"
#include "engine/chat.h"
#include "engine/dom_engine.h"
#include "engine/native_call.h"
#include "engine/process_event.h"
#include "http/http_server.h"
#include "rcon/rcon_commands.h"
#include "rcon/rcon_server.h"
#include "runtime.h"
#include "utils/logger.h"
#include "utils/memory.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

APIConfig g_Config;

namespace {

// LD_PRELOAD is inherited by every process the panel starts: bash running the startup line,
// the stock RSDragonwildsServer.sh wrapper, crash reporters. Before this check each of them
// loaded the mod, and whichever bound the REST port first won - usually the wrapper, so
// /api/health reported engineReady=false forever (PR #3). Only the server binary itself
// gets listeners. RSDWAPI_FORCE=1 overrides, for testing or a renamed binary.
bool IsTargetProcess() {
    const char* force = getenv("RSDWAPI_FORCE");
    if (force && force[0] == '1') return true;

    char exe[4096] = {};
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0) return false;

    std::string path(exe, (size_t)len);
    size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    return name.rfind("RSDragonwildsServer", 0) == 0 &&
           name.find(".sh") == std::string::npos;
}

HttpServer* g_HttpServer = nullptr;
Rcon::Server* g_RconServer = nullptr;
std::thread* g_InitThread = nullptr;
std::atomic<bool> g_Shutdown{false};
std::atomic<bool> g_StoppedCleanly{false};

void ShutdownListeners() {
    if (g_StoppedCleanly.exchange(true)) return;
    g_Shutdown = true;

    if (g_HttpServer) g_HttpServer->Stop();
    if (g_RconServer) g_RconServer->Stop();
    Discord::Stop();

    if (g_InitThread && g_InitThread->joinable()) g_InitThread->join();

    Runtime::BeginShutdown();
}

void StartListeners() {
    if (g_Config.rest.enabled) {
        g_HttpServer = new HttpServer();
        g_HttpServer->SetIpWhitelist(g_Config.rest.ipWhitelist);
        ApiRoutes::RegisterAll(*g_HttpServer);
        if (g_Config.rest.bearerToken.empty()) {
            LogMessage("RSDWRestAPI: REST bearer token is empty - the API is UNAUTHENTICATED. "
                       "Bind to 127.0.0.1 or set IPWhitelist unless that is intended.");
        }
        bool running = g_HttpServer->Start(g_Config.rest.port, g_Config.rest.bindAddress);
        Runtime::SetRestState(true, running, g_Config.rest.port);
        if (!running) LogMessage("RSDWRestAPI: REST listener failed to start");
    } else {
        Runtime::SetRestState(false, false, g_Config.rest.port);
        LogMessage("RSDWRestAPI: REST API disabled by settings.ini");
    }

    if (g_Config.rcon.enabled) {
        Rcon::RegisterAll();
        g_RconServer = new Rcon::Server();
        bool running = g_RconServer->Start(g_Config.rcon);
        Runtime::SetRconState(true, running, g_Config.rcon.port);
        if (!running) LogMessage("RSDWRestAPI: RCON listener failed to start");
    } else {
        Runtime::SetRconState(false, false, g_Config.rcon.port);
        LogMessage("RSDWRestAPI: RCON disabled by settings.ini");
    }
}

void InitThread() {
    Mem::Init();

    APIConfig::EnsureConfigDirectory();
    std::string configPath = APIConfig::GetConfigPath();
    if (!g_Config.Load(configPath)) g_Config.GenerateSecrets();
    std::vector<std::string> fromEnv = g_Config.ApplyEnvironment();
    g_Config.Save(configPath);

    GetActiveLogPath() = InitLogFile();
    Runtime::MarkStart();

    LogMessage("RSDWRestAPI " + std::string(Runtime::Version()) + " loaded");
    LogMessage("RSDWRestAPI: data directory " + GetBaseDir());
    if (!fromEnv.empty()) {
        std::string names;
        for (const std::string& name : fromEnv) names += (names.empty() ? "" : ", ") + name;
        LogMessage("RSDWRestAPI: settings from environment: " + names);
    }

    StartListeners();
    atexit(&ShutdownListeners);

    if (!g_Config.rest.enabled && !g_Config.rcon.enabled) {
        LogMessage("RSDWRestAPI: both listeners are disabled, nothing to do");
        return;
    }

    DomEngine::g_Engine = new DomEngine::Engine();

    for (int attempt = 1; !g_Shutdown && !DomEngine::g_Engine->IsInitialized(); attempt++) {
        if (DomEngine::g_Engine->Initialize()) break;

        int waitSeconds = attempt < 10 ? 1 : (attempt < 30 ? 5 : 30);

        if (attempt == 1) {
            LogMessage("RSDWRestAPI: waiting for the engine to finish loading");
        }

        for (int i = 0; i < waitSeconds * 10 && !g_Shutdown; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    if (!g_Shutdown && DomEngine::g_Engine->IsInitialized()) {
        LogMessage("RSDWRestAPI: engine ready, endpoints are live");

        NativeCall::LogImageSymbolReport();

        // The game-thread pump and the chat hook both depend on classes that can register
        // after the engine itself is up. Each used to get exactly one attempt; a miss
        // disabled kicks, broadcasts and chat relay until restart (issue #6). They are now
        // retried from the main loop below until they succeed.
        DomEngine::DiscoveryInfo discovery = DomEngine::g_Engine->GetDiscoveryInfo();
        auto tryPump = [&]() -> bool {
            return GameThread::IsReady() ||
                   GameThread::Initialize(discovery.moduleBase, discovery.moduleEnd) ||
                   GameThread::InstallExecPump();
        };

        bool pumpReady = tryPump();
        if (!pumpReady) {
            LogMessage("RSDWRestAPI: game-thread pump not available yet (" + GameThread::Status() +
                       "); kick, give and broadcast wait for it, retrying every 15s");
        }
        bool chatReady = pumpReady && DomChat::Initialize();
        auto lastRetry = std::chrono::steady_clock::now();
        constexpr auto kRetryInterval = std::chrono::seconds(15);

        if (g_Config.discord.enabled) {
            Discord::Start(Discord::Channel::Chat, g_Config.discord.webhookUrl, g_Config.discord.username);
            std::string deathUrl = g_Config.discord.deathWebhookUrl.empty() ? g_Config.discord.webhookUrl
                                                                            : g_Config.discord.deathWebhookUrl;
            Discord::Start(Discord::Channel::Deaths, deathUrl, g_Config.discord.username);
            if (Discord::IsEnabled(Discord::Channel::Chat)) {
                DomChat::AddListener([](const DomChat::Message& message) {
                    std::string sender = message.senderName.empty() ? "Unknown" : message.senderName;
                    Discord::Post(Discord::Channel::Chat, "**" + sender + "**: " + message.body);
                });
            }
        }

        DomBans::Load();

        std::map<std::string, bool> wasDead;
        std::map<std::string, std::chrono::steady_clock::time_point> deathPosted;
        std::mutex deathMutex;

        auto reportDeath = [&](const std::string& key, const std::string& name, const std::string& where) {
            auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(deathMutex);
                auto it = deathPosted.find(key);
                if (it != deathPosted.end() && now - it->second < std::chrono::seconds(10)) return;
                deathPosted[key] = now;
            }
            LogMessage("Deaths: " + name + " died" + (where.empty() ? "" : " at " + where));
            if (Discord::IsEnabled(Discord::Channel::Deaths)) {
                Discord::Post(Discord::Channel::Deaths,
                              ":skull: **" + name + "** died" + (where.empty() ? "" : " at `" + where + "`"));
            }
        };

        DomChat::AddEventListener([&](const DomChat::PlayerEvent& event) {
            std::string tag = event.tag;
            std::transform(tag.begin(), tag.end(), tag.begin(),
                           [](unsigned char c) { return (char)tolower(c); });
            if (tag.find("death") == std::string::npos && tag.find("died") == std::string::npos &&
                tag.find("die") == std::string::npos) {
                return;
            }
            std::string name = event.senderName.empty() ? "playerId " + std::to_string(event.playerId)
                                                        : event.senderName;
            std::string key = event.senderNetId.empty() ? std::to_string(event.playerId) : event.senderNetId;
            std::string where;
            for (const DomEngine::PlayerInfo& player : DomEngine::g_Engine->GetAllPlayers()) {
                if (player.playerId != event.playerId) continue;
                if (!player.hasLocation) break;
                char buffer[96];
                snprintf(buffer, sizeof(buffer), "%.0f, %.0f, %.0f", player.x, player.y, player.z);
                where = buffer;
                break;
            }
            reportDeath(key, name, where);
        });

        while (!g_Shutdown) {
            for (int i = 0; i < 20 && !g_Shutdown; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (g_Shutdown) break;

            auto now = std::chrono::steady_clock::now();
            if ((!pumpReady || !chatReady) && now - lastRetry >= kRetryInterval) {
                lastRetry = now;
                if (!pumpReady && (pumpReady = tryPump())) {
                    LogMessage("RSDWRestAPI: game-thread pump installed (" + GameThread::Status() + ")");
                }
                if (pumpReady && !chatReady) chatReady = DomChat::Initialize();
            }

            DomEngine::g_Engine->EnforceKicks();
            DomBans::Enforce();
            DomChat::Poll();

            std::map<std::string, bool> nowDead;
            for (const DomEngine::PlayerInfo& player : DomEngine::g_Engine->GetAllPlayers()) {
                if (player.uniqueNetId.empty() || !player.hasHealth) continue;
                nowDead[player.uniqueNetId] = player.isDead;

                auto previous = wasDead.find(player.uniqueNetId);
                bool justDied = player.isDead && previous != wasDead.end() && !previous->second;
                if (!justDied) continue;

                std::string name = player.characterName.empty() ? player.name : player.characterName;
                char where[96];
                snprintf(where, sizeof(where), "%.0f, %.0f, %.0f", player.x, player.y, player.z);
                reportDeath(player.uniqueNetId, name, where);
            }
            wasDead.swap(nowDead);
        }
    }
}

bool g_Active = false;

}

extern "C" __attribute__((constructor)) void RSDWApiLoad() {
    if (!IsTargetProcess()) return;
    g_Active = true;
    g_InitThread = new std::thread(InitThread);
}

extern "C" __attribute__((destructor)) void RSDWApiUnload() {
    if (!g_Active) return;
    g_Shutdown = true;
    Runtime::BeginShutdown();
}
