#include "runtime.h"

#include <atomic>
#include <chrono>

namespace Runtime {

namespace {

std::chrono::steady_clock::time_point g_Start = std::chrono::steady_clock::now();

bool g_RestEnabled = false;
bool g_RestRunning = false;
int g_RestPort = 0;

bool g_RconEnabled = false;
bool g_RconRunning = false;
int g_RconPort = 0;

std::atomic<bool> g_ShuttingDown{false};

}

void MarkStart() {
    g_Start = std::chrono::steady_clock::now();
}

double UptimeSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_Start).count();
}

void SetRestState(bool enabled, bool running, int port) {
    g_RestEnabled = enabled;
    g_RestRunning = running;
    g_RestPort = port;
}

void SetRconState(bool enabled, bool running, int port) {
    g_RconEnabled = enabled;
    g_RconRunning = running;
    g_RconPort = port;
}

bool RestEnabled() { return g_RestEnabled; }
bool RestRunning() { return g_RestRunning; }
int RestPort() { return g_RestPort; }

bool RconEnabled() { return g_RconEnabled; }
bool RconRunning() { return g_RconRunning; }
int RconPort() { return g_RconPort; }

const char* Version() { return "0.1.0"; }

void BeginShutdown() { g_ShuttingDown = true; }
bool ShuttingDown() { return g_ShuttingDown; }

}
