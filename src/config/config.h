#pragma once
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

inline std::string GetBaseDir() {
    static std::string cached;
    if (!cached.empty()) return cached;

    const char* overrideDir = getenv("RSDWAPI_DIR");
    if (overrideDir && *overrideDir) {
        cached = overrideDir;
        return cached;
    }

    char exePath[4096] = {};
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    std::string dir = ".";
    if (len > 0) {
        std::string path(exePath, (size_t)len);
        size_t slash = path.find_last_of('/');
        if (slash != std::string::npos) dir = path.substr(0, slash);
    }
    cached = dir + "/rsdwapi";
    return cached;
}

inline void EnsureDir(const std::string& path) {
    mkdir(path.c_str(), 0755);
}

struct RestConfig {
    bool enabled = true;
    int port = 8080;
    std::string bindAddress = "0.0.0.0";
    std::string bearerToken;
    std::string ipWhitelist;
};

struct RconConfig {
    bool enabled = false;
    int port = 27020;
    std::string bindAddress = "0.0.0.0";
    std::string password;
    std::string ipWhitelist;
    int commandsPerMinute = 60;
    int commandBurst = 15;
    int maxFailedAuth = 5;
    int failWindowSeconds = 60;
    int banSeconds = 300;
    int maxConnections = 16;
};

struct DiscordConfig {
    bool enabled = false;
    std::string webhookUrl;
    std::string username = "Dragonwilds";
};

struct APIConfig {
    bool enableLogging = true;
    bool verboseLogging = true;
    RestConfig rest;
    RconConfig rcon;
    DiscordConfig discord;

    bool Load(const std::string& configPath) {
        std::ifstream file(configPath);
        if (!file.is_open()) return false;

        std::string section;
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;

            if (line[0] == '[') {
                size_t close = line.find(']');
                section = (close == std::string::npos) ? line.substr(1) : line.substr(1, close - 1);
                continue;
            }

            size_t pos = line.find('=');
            if (pos == std::string::npos) continue;

            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            size_t lastGood = value.find_last_not_of(" \t");
            value = (lastGood == std::string::npos) ? "" : value.substr(0, lastGood + 1);

            Apply(section, key, value);
        }
        return true;
    }

    void GenerateSecrets() {
        rest.bearerToken = GenerateSecureToken(32);
        rcon.password = GenerateSecureToken(12);
    }

    void Save(const std::string& configPath) {
        std::ofstream file(configPath);
        if (!file.is_open()) return;

        file << "# RSDWRestAPI configuration\n";
        file << "# Generated automatically - edit as needed, then restart the server.\n\n";

        file << "[General]\n";
        file << "EnableLogging=" << Bool(enableLogging) << "\n";
        file << "# Logs every discovery step and every request. Leave on unless the volume hurts.\n";
        file << "VerboseLogging=" << Bool(verboseLogging) << "\n\n";

        file << "[API]\n";
        file << "# Set to false to disable the REST API.\n";
        file << "Enabled=" << Bool(rest.enabled) << "\n";
        file << "# Bind to 127.0.0.1 to restrict to localhost only (recommended).\n";
        file << "BindAddress=" << rest.bindAddress << "\n";
        file << "Port=" << rest.port << "\n";
        file << "# Bearer token for authentication. Leave it empty to disable auth entirely -\n";
        file << "# an empty value is kept as written, not regenerated on the next start.\n";
        file << "BearerToken=" << rest.bearerToken << "\n";
        file << "# Comma separated addresses and CIDR blocks. Empty allows everyone.\n";
        file << "IPWhitelist=" << rest.ipWhitelist << "\n\n";

        file << "[RCON]\n";
        file << "# Source RCON, off by default. Set to true to enable it; the password\n";
        file << "# below was generated on first run and is ready to use.\n";
        file << "Enabled=" << Bool(rcon.enabled) << "\n";
        file << "BindAddress=" << rcon.bindAddress << "\n";
        file << "Port=" << rcon.port << "\n";
        file << "# An empty password keeps RCON from starting at all, by design.\n";
        file << "Password=" << rcon.password << "\n";
        file << "# Comma separated addresses and CIDR blocks. Empty allows everyone.\n";
        file << "IPWhitelist=" << rcon.ipWhitelist << "\n";
        file << "# Sustained command rate per client address, and the burst it may spend at once.\n";
        file << "CommandsPerMinute=" << rcon.commandsPerMinute << "\n";
        file << "CommandBurst=" << rcon.commandBurst << "\n";
        file << "# Failed logins allowed inside FailWindowSeconds before the address is refused\n";
        file << "# connections for BanSeconds.\n";
        file << "MaxFailedAuth=" << rcon.maxFailedAuth << "\n";
        file << "FailWindowSeconds=" << rcon.failWindowSeconds << "\n";
        file << "BanSeconds=" << rcon.banSeconds << "\n";
        file << "MaxConnections=" << rcon.maxConnections << "\n\n";

        file << "[Discord]\n";
        file << "# Relays in-game chat to a Discord webhook. Off by default.\n";
        file << "Enabled=" << Bool(discord.enabled) << "\n";
        file << "WebhookUrl=" << discord.webhookUrl << "\n";
        file << "Username=" << discord.username << "\n";
    }

    static std::string GenerateSecureToken(size_t bytes) {
        std::vector<unsigned char> buf(bytes);
        FILE* urandom = fopen("/dev/urandom", "rb");
        if (!urandom) return "";
        size_t got = fread(buf.data(), 1, bytes, urandom);
        fclose(urandom);
        if (got != bytes) return "";

        static const char hex[] = "0123456789abcdef";
        std::string token;
        token.reserve(bytes * 2);
        for (unsigned char c : buf) {
            token += hex[c >> 4];
            token += hex[c & 0xF];
        }
        return token;
    }

    static std::string GetConfigPath() { return GetBaseDir() + "/settings.ini"; }
    static void EnsureConfigDirectory() { EnsureDir(GetBaseDir()); }

private:
    static const char* Bool(bool value) { return value ? "true" : "false"; }
    static bool ParseBool(const std::string& value) { return value == "true" || value == "1"; }

    void Apply(const std::string& section, const std::string& key, const std::string& value) {
        if (section == "General") {
            if (key == "EnableLogging") enableLogging = ParseBool(value);
            else if (key == "VerboseLogging") verboseLogging = ParseBool(value);
            return;
        }

        if (section == "API") {
            if (key == "Enabled") rest.enabled = ParseBool(value);
            else if (key == "Port") rest.port = atoi(value.c_str());
            else if (key == "BindAddress") rest.bindAddress = value;
            else if (key == "BearerToken") rest.bearerToken = value;
            else if (key == "IPWhitelist") rest.ipWhitelist = value;
            return;
        }

        if (section == "RCON") {
            if (key == "Enabled") rcon.enabled = ParseBool(value);
            else if (key == "Port") rcon.port = atoi(value.c_str());
            else if (key == "BindAddress") rcon.bindAddress = value;
            else if (key == "Password") rcon.password = value;
            else if (key == "IPWhitelist") rcon.ipWhitelist = value;
            else if (key == "CommandsPerMinute") rcon.commandsPerMinute = atoi(value.c_str());
            else if (key == "CommandBurst") rcon.commandBurst = atoi(value.c_str());
            else if (key == "MaxFailedAuth") rcon.maxFailedAuth = atoi(value.c_str());
            else if (key == "FailWindowSeconds") rcon.failWindowSeconds = atoi(value.c_str());
            else if (key == "BanSeconds") rcon.banSeconds = atoi(value.c_str());
            else if (key == "MaxConnections") rcon.maxConnections = atoi(value.c_str());
            return;
        }

        if (section == "Discord") {
            if (key == "Enabled") discord.enabled = ParseBool(value);
            else if (key == "WebhookUrl") discord.webhookUrl = value;
            else if (key == "Username") discord.username = value;
            return;
        }
    }
};

extern APIConfig g_Config;
