#pragma once
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Net {

std::string IpToString(uint32_t addr);

class IpFilter {
public:
    void Configure(const std::string& list);
    bool IsConfigured() const { return !m_Rules.empty(); }
    bool Allows(uint32_t addr) const;
    const std::vector<std::string>& RejectedRules() const { return m_Rejected; }

private:
    struct Rule {
        uint32_t network;
        uint32_t mask;
    };
    std::vector<Rule> m_Rules;
    std::vector<std::string> m_Rejected;
};

class RateLimiter {
public:
    void Configure(int perMinute, int burst);
    bool Allow(uint32_t addr);
    void Forget(uint32_t addr);

private:
    struct Bucket {
        double tokens;
        std::chrono::steady_clock::time_point last;
    };

    std::mutex m_Mutex;
    std::unordered_map<uint32_t, Bucket> m_Buckets;
    double m_RefillPerSecond = 0.0;
    double m_Burst = 0.0;
    bool m_Enabled = false;
};

class FailureTracker {
public:
    void Configure(int maxFailures, int windowSeconds, int banSeconds);
    bool IsBlocked(uint32_t addr);
    bool RecordFailure(uint32_t addr);
    void RecordSuccess(uint32_t addr);
    int SecondsRemaining(uint32_t addr);

private:
    struct Entry {
        int failures = 0;
        std::chrono::steady_clock::time_point windowStart;
        std::chrono::steady_clock::time_point blockedUntil;
    };

    std::mutex m_Mutex;
    std::unordered_map<uint32_t, Entry> m_Entries;
    int m_MaxFailures = 0;
    int m_WindowSeconds = 60;
    int m_BanSeconds = 300;
    bool m_Enabled = false;
};

}
