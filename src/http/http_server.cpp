#include "http_server.h"
#include "../config/config.h"
#include "../utils/logger.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sstream>
#include <sys/select.h>
#include <sys/socket.h>
#include <chrono>
#include <thread>
#include <unistd.h>

HttpServer::HttpServer()
    : listenSocket(-1), running(false), m_Port(0), listenerThread(nullptr) {}

HttpServer::~HttpServer() {
    Stop();
}

void HttpServer::SetIpWhitelist(const std::string& whitelist) {
    ipFilter.Configure(whitelist);
    for (const std::string& bad : ipFilter.RejectedRules()) {
        LogMessage("HTTP: ignoring unparseable IPWhitelist entry '" + bad + "'");
    }
}

bool HttpServer::Start(int port, const std::string& bindAddress) {
    if (running) return false;

    m_Port = port;
    m_BindAddress = bindAddress;

    listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket < 0) {
        LogMessage("HTTP: Failed to create socket");
        return false;
    }

    int reuse = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons((uint16_t)m_Port);

    if (bindAddress.empty() || bindAddress == "0.0.0.0") {
        serverAddr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, bindAddress.c_str(), &serverAddr.sin_addr) != 1) {
        LogMessage("HTTP: Invalid BindAddress '" + bindAddress + "', falling back to 0.0.0.0");
        serverAddr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        LogMessage("HTTP: Failed to bind on " + bindAddress + ":" + std::to_string(m_Port) +
                   " (" + strerror(errno) + ")");
        close(listenSocket);
        listenSocket = -1;
        return false;
    }

    if (listen(listenSocket, SOMAXCONN) < 0) {
        LogMessage("HTTP: Failed to listen on socket");
        close(listenSocket);
        listenSocket = -1;
        return false;
    }

    running = true;
    listenerThread = new std::thread(&HttpServer::ListenerLoop, this);

    LogMessage("HTTP: Server started on " + bindAddress + ":" + std::to_string(m_Port));
    return true;
}

void HttpServer::Stop() {
    if (!running) return;
    running = false;

    if (listenSocket >= 0) shutdown(listenSocket, SHUT_RDWR);

    if (listenerThread && listenerThread->joinable()) {
        listenerThread->join();
        delete listenerThread;
        listenerThread = nullptr;
    }

    if (listenSocket >= 0) {
        close(listenSocket);
        listenSocket = -1;
    }

    for (int i = 0; i < 20 && activeClients.load() > 0; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    LogMessage("HTTP: Server stopped");
}

void HttpServer::ListenerLoop() {
    while (running) {
        int fd = listenSocket;
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
        if (!ipFilter.Allows(peer)) {
            LogVerbose("HTTP: rejected " + Net::IpToString(peer) + " (not whitelisted)");
            close(clientSocket);
            continue;
        }

        activeClients++;
        std::thread([this, clientSocket]() {
            HandleClient(clientSocket);
            activeClients--;
        }).detach();
    }
}

std::string HttpServer::ParseRequest(int clientSocket, HttpRequest& req) {
    char buf[8192];
    ssize_t received = recv(clientSocket, buf, sizeof(buf) - 1, 0);
    if (received <= 0) return "recv failed";

    std::string rawRequest(buf, (size_t)received);

    size_t headerEnd = rawRequest.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return "incomplete headers";

    std::string headers = rawRequest.substr(0, headerEnd);
    std::string body = rawRequest.substr(headerEnd + 4);

    std::istringstream headerStream(headers);
    std::string requestLine;
    std::getline(headerStream, requestLine);
    if (!requestLine.empty() && requestLine.back() == '\r') requestLine.pop_back();

    std::istringstream reqLineStream(requestLine);
    reqLineStream >> req.method >> req.path;

    std::string headerLine;
    while (std::getline(headerStream, headerLine)) {
        if (!headerLine.empty() && headerLine.back() == '\r') headerLine.pop_back();
        size_t colon = headerLine.find(':');
        if (colon == std::string::npos) continue;

        std::string key = headerLine.substr(0, colon);
        std::string val = headerLine.substr(colon + 1);
        while (!val.empty() && val[0] == ' ') val.erase(0, 1);
        req.headers[key] = val;
    }

    std::string fullPath = req.path;
    std::string pathOnly;
    ParseQuery(fullPath, pathOnly, req.query);
    req.path = pathOnly;

    auto it = req.headers.find("Content-Length");
    if (it != req.headers.end()) {
        long contentLength = atol(it->second.c_str());
        if (contentLength > 0 && contentLength < 8 * 1024 * 1024) {
            while ((long)body.length() < contentLength) {
                long remaining = contentLength - (long)body.length();
                size_t toRead = remaining < (long)sizeof(buf) ? (size_t)remaining : sizeof(buf);
                received = recv(clientSocket, buf, toRead, 0);
                if (received <= 0) break;
                body.append(buf, (size_t)received);
            }
        }
    }

    req.body = body;
    return "";
}

void HttpServer::ParseQuery(const std::string& fullPath, std::string& path,
                            std::unordered_map<std::string, std::string>& query) {
    size_t qmark = fullPath.find('?');
    if (qmark == std::string::npos) {
        path = fullPath;
        return;
    }
    path = fullPath.substr(0, qmark);
    std::string queryString = fullPath.substr(qmark + 1);

    size_t start = 0;
    while (start < queryString.size()) {
        size_t amp = queryString.find('&', start);
        std::string pair;
        if (amp == std::string::npos) {
            pair = queryString.substr(start);
            start = queryString.size();
        } else {
            pair = queryString.substr(start, amp - start);
            start = amp + 1;
        }
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            query[UrlDecode(pair.substr(0, eq))] = UrlDecode(pair.substr(eq + 1));
        } else {
            query[UrlDecode(pair)] = "";
        }
    }
}

std::string HttpServer::UrlDecode(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.size(); i++) {
        if (str[i] == '%' && i + 2 < str.size()) {
            int hex = 0;
            std::istringstream iss(str.substr(i + 1, 2));
            iss >> std::hex >> hex;
            result += (char)hex;
            i += 2;
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

void HttpServer::SendResponse(int clientSocket, const HttpResponse& resp) {
    std::string statusText;
    switch (resp.status) {
        case 200: statusText = "OK"; break;
        case 201: statusText = "Created"; break;
        case 400: statusText = "Bad Request"; break;
        case 401: statusText = "Unauthorized"; break;
        case 404: statusText = "Not Found"; break;
        case 405: statusText = "Method Not Allowed"; break;
        case 500: statusText = "Internal Server Error"; break;
        case 503: statusText = "Service Unavailable"; break;
        default: statusText = "OK"; break;
    }

    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status << " " << statusText << "\r\n";
    oss << "Content-Type: " << resp.contentType << "\r\n";
    oss << "Content-Length: " << resp.body.length() << "\r\n";
    oss << "Access-Control-Allow-Origin: *\r\n";
    oss << "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n";
    oss << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n";
    oss << "Connection: close\r\n";
    oss << "\r\n";
    oss << resp.body;

    std::string responseStr = oss.str();
    size_t sent = 0;
    while (sent < responseStr.size()) {
        ssize_t n = send(clientSocket, responseStr.data() + sent, responseStr.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += (size_t)n;
    }
}

void HttpServer::HandleClient(int clientSocket) {
    timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    HttpRequest req;
    std::string error = ParseRequest(clientSocket, req);
    if (!error.empty()) {
        close(clientSocket);
        return;
    }

    if (req.method == "OPTIONS") {
        SendResponse(clientSocket, HttpResponse(200));
        close(clientSocket);
        return;
    }

    if (!g_Config.rest.bearerToken.empty()) {
        auto authIt = req.headers.find("Authorization");
        bool authorized = false;
        if (authIt != req.headers.end()) {
            const std::string& authHeader = authIt->second;
            const std::string prefix = "Bearer ";
            if (authHeader.size() > prefix.size() &&
                authHeader.compare(0, prefix.size(), prefix) == 0) {
                if (authHeader.substr(prefix.size()) == g_Config.rest.bearerToken) authorized = true;
            }
        }
        if (!authorized) {
            HttpResponse resp(401);
            resp.body = R"({"error":"Unauthorized: invalid or missing bearer token"})";
            SendResponse(clientSocket, resp);
            close(clientSocket);
            return;
        }
    }

    HttpResponse resp;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(routesMutex);
        for (const auto& route : routes) {
            if (route.method != req.method || route.path != req.path) continue;
            try {
                resp = route.handler(req);
            } catch (...) {
                resp = HttpResponse(500);
                resp.body = R"({"error":"Internal server error"})";
            }
            found = true;
            break;
        }
    }

    if (!found) {
        resp = HttpResponse(404);
        resp.body = R"({"error":"Not found"})";
    }

    LogVerbose("HTTP: " + req.method + " " + req.path + " -> " + std::to_string(resp.status));

    SendResponse(clientSocket, resp);
    close(clientSocket);
}

void HttpServer::Get(const std::string& path, RouteHandler handler) {
    std::lock_guard<std::mutex> lock(routesMutex);
    routes.push_back({"GET", path, handler});
}

void HttpServer::Post(const std::string& path, RouteHandler handler) {
    std::lock_guard<std::mutex> lock(routesMutex);
    routes.push_back({"POST", path, handler});
}

void HttpServer::Put(const std::string& path, RouteHandler handler) {
    std::lock_guard<std::mutex> lock(routesMutex);
    routes.push_back({"PUT", path, handler});
}

void HttpServer::Delete(const std::string& path, RouteHandler handler) {
    std::lock_guard<std::mutex> lock(routesMutex);
    routes.push_back({"DELETE", path, handler});
}

void HttpServer::Route(const std::string& method, const std::string& path, RouteHandler handler) {
    std::lock_guard<std::mutex> lock(routesMutex);
    routes.push_back({method, path, handler});
}
