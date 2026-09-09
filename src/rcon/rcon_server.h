#pragma once
#include "../config/config.h"
#include "../net/access_control.h"

#include <atomic>
#include <string>
#include <thread>

namespace Rcon {

enum PacketType : int32_t {
    ResponseValue = 0,
    ExecCommand   = 2,
    AuthResponse  = 2,
    Auth          = 3,
};

class Server {
public:
    Server();
    ~Server();

    bool Start(const RconConfig& config);
    void Stop();
    bool IsRunning() const { return m_Running; }
    int Port() const { return m_Config.port; }

private:
    void ListenerLoop();
    void HandleClient(int clientSocket, uint32_t peer);
    bool ReadPacket(int clientSocket, int32_t& id, int32_t& type, std::string& body);
    bool SendPacket(int clientSocket, int32_t id, int32_t type, const std::string& body);
    bool SendResponseBody(int clientSocket, int32_t id, const std::string& body);

    RconConfig m_Config;
    int m_ListenSocket = -1;
    std::atomic<bool> m_Running{false};
    std::atomic<int> m_Connections{0};
    std::thread* m_ListenerThread = nullptr;

    Net::IpFilter m_IpFilter;
    Net::RateLimiter m_CommandLimiter;
    Net::FailureTracker m_AuthFailures;
};

}
