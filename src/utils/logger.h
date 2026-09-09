#pragma once
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>
#include "../config/config.h"
#include "../runtime.h"

extern APIConfig g_Config;

inline std::string GetLogDir() {
    return GetBaseDir() + "/logs";
}

inline void CleanupOldLogs(const std::string& logDir, int keepCount) {
    DIR* dir = opendir(logDir.c_str());
    if (!dir) return;

    struct LogEntry {
        std::string path;
        time_t mtime;
    };
    std::vector<LogEntry> logs;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name(entry->d_name);
        if (name.rfind("api-", 0) != 0) continue;
        if (name.size() < 5 || name.substr(name.size() - 4) != ".log") continue;

        std::string full = logDir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        logs.push_back({full, st.st_mtime});
    }
    closedir(dir);

    if ((int)logs.size() <= keepCount) return;

    std::sort(logs.begin(), logs.end(),
              [](const LogEntry& a, const LogEntry& b) { return a.mtime > b.mtime; });

    for (size_t i = (size_t)keepCount; i < logs.size(); i++) {
        remove(logs[i].path.c_str());
    }
}

inline std::string InitLogFile() {
    EnsureDir(GetBaseDir());
    std::string logDir = GetLogDir();
    EnsureDir(logDir);

    std::string activePath = logDir + "/api.log";

    struct stat st;
    if (stat(activePath.c_str(), &st) == 0) {
        time_t now = time(nullptr);
        struct tm tmval;
        localtime_r(&now, &tmval);

        char ts[128];
        strftime(ts, sizeof(ts), "%Y.%m.%d-%H%M%S", &tmval);
        rename(activePath.c_str(), (logDir + "/api-" + ts + ".log").c_str());
    }

    CleanupOldLogs(logDir, 10);

    std::ofstream f(activePath, std::ios::out | std::ios::trunc);
    if (f.is_open()) {
        time_t now = time(nullptr);
        struct tm tmval;
        localtime_r(&now, &tmval);
        char ts[100];
        strftime(ts, sizeof(ts), "%a %b %d %H:%M:%S %Y", &tmval);
        f << "[RSDWRestAPI] Log session started " << ts << std::endl;
    }

    return activePath;
}

inline std::string& GetActiveLogPath() {
    static std::string activePath;
    return activePath;
}

inline void LogMessage(const std::string& message) {
    if (!g_Config.enableLogging || Runtime::ShuttingDown()) return;

    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);

    std::string& logPath = GetActiveLogPath();
    if (logPath.empty()) logPath = InitLogFile();

    std::ofstream logFile(logPath, std::ios::app);
    if (!logFile.is_open()) return;

    time_t now = time(nullptr);
    struct tm tmval;
    localtime_r(&now, &tmval);
    char timestamp[100];
    strftime(timestamp, sizeof(timestamp), "%a %b %d %H:%M:%S %Y", &tmval);
    logFile << "[" << timestamp << "] " << message << std::endl;

    fprintf(stderr, "[RSDWRestAPI] %s\n", message.c_str());
}

inline void LogVerbose(const std::string& message) {
    if (!g_Config.verboseLogging) return;
    LogMessage(message);
}

inline std::string HexString(unsigned long long value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llX", value);
    return buf;
}
