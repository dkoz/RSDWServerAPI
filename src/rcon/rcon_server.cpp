#include "rcon_server.h"
#include "rcon_commands.h"
#include "../utils/logger.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <chrono>
#include <thread>
#include <unistd.h>

namespace Rcon {

namespace {

constexpr size_t kMaxBody = 3900;
constexpr int32_t kMaxPacketSize = 8192;

bool ReadExactly(int socketFd, void* buffer, size_t length) {
    uint8_t* out = (uint8_t*)buffer;
    size_t got = 0;
    while (got < length) {
        ssize_t n = recv(socketFd, out + got, length - got, 0);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

bool SendExactly(int socketFd, const void* buffer, size_t length) {
    const uint8_t* in = (const uint8_t*)buffer;
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = send(socketFd, in + sent, length - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

bool SecretsMatch(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

}

Server::Server() {}

Server::~Server() {
    Stop();
}

bool Server::Start(const RconConfig& config) {
    if (m_Running) return false;
    m_Config = config;

    if (m_Config.password.empty()) {
        LogMessage("RCON: refusing to start with an empty password. Set Password in settings.ini.");
        return false;
    }

    m_IpFilter.Configure(m_Config.ipWhitelist);
    for (const std::string& bad : m_IpFilter.RejectedRules()) {
        LogMessage("RCON: ignoring unparseable IPWhitelist entry '" + bad + "'");
    }
    m_CommandLimiter.Configure(m_Config.commandsPerMinute, m_Config.commandBurst);
    m_AuthFailures.Configure(m_Config.maxFailedAuth, m_Config.failWindowSeconds, m_Config.banSeconds);

    m_ListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_ListenSocket < 0) {
        LogMessage("RCON: failed to create socket");
        return false;
    }

    int reuse = 1;
    setsockopt(m_ListenSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons((uint16_t)m_Config.port);

    if (m_Config.bindAddress.empty() || m_Config.bindAddress == "0.0.0.0") {
        serverAddr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, m_Config.bindAddress.c_str(), &serverAddr.sin_addr) != 1) {
        LogMessage("RCON: invalid BindAddress '" + m_Config.bindAddress + "', falling back to 0.0.0.0");
        serverAddr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(m_ListenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        LogMessage("RCON: failed to bind on " + m_Config.bindAddress + ":" +
                   std::to_string(m_Config.port) + " (" + strerror(errno) + ")");
        close(m_ListenSocket);
        m_ListenSocket = -1;
        return false;
    }

    if (listen(m_ListenSocket, 16) < 0) {
        LogMessage("RCON: failed to listen on socket");
        close(m_ListenSocket);
        m_ListenSocket = -1;
        return false;
    }

    m_Running = true;
    m_ListenerThread = new std::thread(&Server::ListenerLoop, this);

    LogMessage("RCON: listening on " + m_Config.bindAddress + ":" + std::to_string(m_Config.port) +
               " (whitelist " + (m_IpFilter.IsConfigured() ? m_Config.ipWhitelist : std::string("off")) +
               ", " + std::to_string(m_Config.commandsPerMinute) + " commands/min per address)");
    return true;
}

void Server::Stop() {
    if (!m_Running) return;
    m_Running = false;

    // Same ordering as the HTTP listener: shutdown, join, then close.
    if (m_ListenSocket >= 0) shutdown(m_ListenSocket, SHUT_RDWR);

    if (m_ListenerThread && m_ListenerThread->joinable()) {
        m_ListenerThread->join();
        delete m_ListenerThread;
        m_ListenerThread = nullptr;
    }

    if (m_ListenSocket >= 0) {
        close(m_ListenSocket);
        m_ListenSocket = -1;
    }

    for (int i = 0; i < 20 && m_Connections.load() > 0; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    LogMessage("RCON: stopped");
}

void Server::ListenerLoop() {
    while (m_Running) {
        int fd = m_ListenSocket;
        if (fd < 0) break;

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(fd, &readSet);

        timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int result = select(fd + 1, &readSet, nullptr, nullptr, &timeout);
        if (result <= 0 || !FD_ISSET(fd, &readSet)) continue;

        sockaddr_in clientAddr = {};
        socklen_t addrLen = sizeof(clientAddr);
        int clientSocket = accept(fd, (sockaddr*)&clientAddr, &addrLen);
        if (clientSocket < 0) continue;

        uint32_t peer = ntohl(clientAddr.sin_addr.s_addr);

        if (!m_IpFilter.Allows(peer)) {
            LogMessage("RCON: rejected " + Net::IpToString(peer) + " (not whitelisted)");
            close(clientSocket);
            continue;
        }

        if (m_AuthFailures.IsBlocked(peer)) {
            LogMessage("RCON: rejected " + Net::IpToString(peer) + " (blocked for another " +
                       std::to_string(m_AuthFailures.SecondsRemaining(peer)) + "s)");
            close(clientSocket);
            continue;
        }

        if (m_Connections.load() >= m_Config.maxConnections) {
            LogMessage("RCON: rejected " + Net::IpToString(peer) + " (connection limit reached)");
            close(clientSocket);
            continue;
        }

        m_Connections++;
        std::thread([this, clientSocket, peer]() {
            HandleClient(clientSocket, peer);
            m_Connections--;
        }).detach();
    }
}

bool Server::ReadPacket(int clientSocket, int32_t& id, int32_t& type, std::string& body) {
    int32_t size = 0;
    if (!ReadExactly(clientSocket, &size, sizeof(size))) return false;
    if (size < 10 || size > kMaxPacketSize) return false;

    std::vector<char> payload((size_t)size);
    if (!ReadExactly(clientSocket, payload.data(), payload.size())) return false;

    memcpy(&id, payload.data(), 4);
    memcpy(&type, payload.data() + 4, 4);

    const char* start = payload.data() + 8;
    size_t maxLen = payload.size() - 8;
    size_t len = 0;
    while (len < maxLen && start[len] != '\0') len++;
    body.assign(start, len);
    return true;
}

bool Server::SendPacket(int clientSocket, int32_t id, int32_t type, const std::string& body) {
    int32_t size = (int32_t)(4 + 4 + body.size() + 2);
    std::string packet;
    packet.reserve((size_t)size + 4);
    packet.append((const char*)&size, 4);
    packet.append((const char*)&id, 4);
    packet.append((const char*)&type, 4);
    packet.append(body);
    packet.append(2, '\0');
    return SendExactly(clientSocket, packet.data(), packet.size());
}

bool Server::SendResponseBody(int clientSocket, int32_t id, const std::string& body) {
    if (body.size() <= kMaxBody) {
        return SendPacket(clientSocket, id, ResponseValue, body);
    }

    for (size_t offset = 0; offset < body.size(); offset += kMaxBody) {
        if (!SendPacket(clientSocket, id, ResponseValue, body.substr(offset, kMaxBody))) {
            return false;
        }
    }
    return true;
}

void Server::HandleClient(int clientSocket, uint32_t peer) {
    timeval timeout;
    timeout.tv_sec = 30;
    timeout.tv_usec = 0;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    std::string peerName = Net::IpToString(peer);
    bool authenticated = false;

    while (m_Running) {
        int32_t id = 0;
        int32_t type = 0;
        std::string body;
        if (!ReadPacket(clientSocket, id, type, body)) break;

        if (type == Auth) {
            if (SecretsMatch(body, m_Config.password)) {
                authenticated = true;
                m_AuthFailures.RecordSuccess(peer);
                SendPacket(clientSocket, id, ResponseValue, "");
                SendPacket(clientSocket, id, AuthResponse, "");
                LogMessage("RCON: " + peerName + " authenticated");
                continue;
            }

            bool nowBlocked = m_AuthFailures.RecordFailure(peer);
            SendPacket(clientSocket, id, ResponseValue, "");
            SendPacket(clientSocket, -1, AuthResponse, "");
            LogMessage("RCON: " + peerName + " failed authentication" +
                       (nowBlocked ? " - blocked for " + std::to_string(m_Config.banSeconds) + "s"
                                   : ""));
            break;
        }

        if (!authenticated) {
            SendPacket(clientSocket, -1, AuthResponse, "");
            break;
        }

        if (type != ExecCommand) {
            SendResponseBody(clientSocket, id, "Unsupported packet type " + std::to_string(type));
            continue;
        }

        if (!m_CommandLimiter.Allow(peer)) {
            LogVerbose("RCON: rate limited " + peerName);
            SendResponseBody(clientSocket, id,
                             "Rate limit reached. This address may run " +
                                 std::to_string(m_Config.commandsPerMinute) +
                                 " commands per minute.");
            continue;
        }

        LogVerbose("RCON: " + peerName + " > " + body);
        SendResponseBody(clientSocket, id, Dispatch(body));
    }

    close(clientSocket);
}

}
