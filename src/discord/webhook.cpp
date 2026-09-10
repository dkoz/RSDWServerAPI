#include "webhook.h"
#include "../utils/json.h"
#include "../utils/logger.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace Discord {

namespace {

std::string g_Url;
std::string g_Username = "Dragonwilds";
std::atomic<bool> g_Enabled{false};
std::atomic<bool> g_Running{false};
std::thread* g_Worker = nullptr;

std::mutex g_Mutex;
std::condition_variable g_Signal;
std::deque<std::string> g_Queue;

bool UrlIsSafe(const std::string& url) {
    if (url.rfind("https://", 0) != 0) return false;
    for (char c : url) {
        if (c == '\'' || c == '"' || c == '`' || c == '$' || c == ';' || c == '|' ||
            c == '&' || c == '<' || c == '>' || c == '\\' || c == '\n' || c == '\r') {
            return false;
        }
    }
    return true;
}

void Send(const std::string& content) {
    std::string payload = "{\"username\":\"" + Json::Escape(g_Username) +
                          "\",\"content\":\"" + Json::Escape(content) +
                          "\",\"allowed_mentions\":{\"parse\":[]}}";

    std::string quoted = "'";
    for (char c : payload) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";

    std::string command = "curl -s -o /dev/null -m 10 -X POST -H 'Content-Type: application/json' -d " +
                          quoted + " '" + g_Url + "'";

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        LogMessage("Discord: could not run curl");
        return;
    }
    pclose(pipe);
}

void Worker() {
    while (g_Running) {
        std::string batch;
        {
            std::unique_lock<std::mutex> lock(g_Mutex);
            g_Signal.wait_for(lock, std::chrono::seconds(2),
                              [] { return !g_Queue.empty() || !g_Running; });
            if (!g_Running && g_Queue.empty()) return;

            while (!g_Queue.empty()) {
                const std::string& next = g_Queue.front();
                if (!batch.empty() && batch.size() + next.size() + 1 > 1900) break;
                if (!batch.empty()) batch += "\n";
                batch += next;
                g_Queue.pop_front();
            }
        }

        if (!batch.empty()) Send(batch);

        for (int i = 0; i < 10 && g_Running; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

}

bool IsEnabled() { return g_Enabled; }

void Start(const std::string& webhookUrl, const std::string& username) {
    if (g_Enabled) return;
    if (webhookUrl.empty()) return;

    if (!UrlIsSafe(webhookUrl)) {
        LogMessage("Discord: WebhookUrl rejected, it must be an https URL with no shell characters");
        return;
    }

    g_Url = webhookUrl;
    if (!username.empty()) g_Username = username;

    g_Enabled = true;
    g_Running = true;
    g_Worker = new std::thread(Worker);

    LogMessage("Discord: chat relay enabled");
}

void Stop() {
    if (!g_Running) return;
    g_Running = false;
    g_Signal.notify_all();

    if (g_Worker && g_Worker->joinable()) {
        g_Worker->join();
        delete g_Worker;
        g_Worker = nullptr;
    }
    g_Enabled = false;
}

void Post(const std::string& line) {
    if (!g_Enabled || line.empty()) return;

    std::lock_guard<std::mutex> lock(g_Mutex);
    if (g_Queue.size() >= 200) g_Queue.pop_front();
    g_Queue.push_back(line);
    g_Signal.notify_all();
}

}
