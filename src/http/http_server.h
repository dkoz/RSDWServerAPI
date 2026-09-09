#pragma once

#include "../net/access_control.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    std::unordered_map<std::string, std::string> query;
};

struct HttpResponse {
    int status = 200;
    std::string body;
    std::string contentType = "application/json";

    HttpResponse(int s = 200) : status(s) {}
};

using RouteHandler = std::function<HttpResponse(const HttpRequest&)>;

struct RouteEntry {
    std::string method;
    std::string path;
    RouteHandler handler;
};

class HttpServer {
private:
    int listenSocket;
    std::vector<RouteEntry> routes;
    std::mutex routesMutex;
    bool running;
    int m_Port;
    std::string m_BindAddress;
    std::thread* listenerThread;
    Net::IpFilter ipFilter;
    std::atomic<int> activeClients{0};

    void ListenerLoop();
    void HandleClient(int clientSocket);
    std::string ParseRequest(int clientSocket, HttpRequest& req);
    void SendResponse(int clientSocket, const HttpResponse& resp);
    void ParseQuery(const std::string& fullPath, std::string& path,
                    std::unordered_map<std::string, std::string>& query);
    std::string UrlDecode(const std::string& str);

public:
    HttpServer();
    ~HttpServer();
    void SetIpWhitelist(const std::string& whitelist);
    bool Start(int port, const std::string& bindAddress);
    void Stop();
    bool IsRunning() const { return running; }

    void Get(const std::string& path, RouteHandler handler);
    void Post(const std::string& path, RouteHandler handler);
    void Put(const std::string& path, RouteHandler handler);
    void Delete(const std::string& path, RouteHandler handler);
    void Route(const std::string& method, const std::string& path, RouteHandler handler);
};
