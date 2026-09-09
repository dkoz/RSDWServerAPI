#include "access_control.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>

namespace Net {

namespace {

std::vector<std::string> Split(const std::string& value, char sep) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= value.size()) {
        size_t next = value.find(sep, start);
        std::string part = (next == std::string::npos) ? value.substr(start)
                                                       : value.substr(start, next - start);
        part.erase(0, part.find_first_not_of(" \t"));
        size_t lastGood = part.find_last_not_of(" \t");
        part = (lastGood == std::string::npos) ? "" : part.substr(0, lastGood + 1);
        if (!part.empty()) parts.push_back(part);
        if (next == std::string::npos) break;
        start = next + 1;
    }
    return parts;
}

}

std::string IpToString(uint32_t addr) {
    in_addr in;
    in.s_addr = htonl(addr);
    char buf[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &in, buf, sizeof(buf))) return "?";
    return buf;
}

void IpFilter::Configure(const std::string& list) {
    m_Rules.clear();
    m_Rejected.clear();

    for (const std::string& entry : Split(list, ',')) {
        std::string address = entry;
        int bits = 32;

        size_t slash = entry.find('/');
        if (slash != std::string::npos) {
            address = entry.substr(0, slash);
            bits = atoi(entry.c_str() + slash + 1);
            if (bits < 0 || bits > 32) {
                m_Rejected.push_back(entry);
                continue;
            }
        }

        in_addr parsed;
        if (inet_pton(AF_INET, address.c_str(), &parsed) != 1) {
            m_Rejected.push_back(entry);
            continue;
        }

        uint32_t mask = (bits == 0) ? 0u : (0xFFFFFFFFu << (32 - bits));
        m_Rules.push_back({ntohl(parsed.s_addr) & mask, mask});
    }
}

bool IpFilter::Allows(uint32_t addr) const {
    if (m_Rules.empty()) return true;
    for (const Rule& rule : m_Rules) {
        if ((addr & rule.mask) == rule.network) return true;
    }
    return false;
}

void RateLimiter::Configure(int perMinute, int burst) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Buckets.clear();
    m_Enabled = perMinute > 0;
    m_RefillPerSecond = perMinute / 60.0;
    m_Burst = (burst > 0) ? (double)burst : (double)perMinute;
    if (m_Burst < 1.0) m_Burst = 1.0;
}

bool RateLimiter::Allow(uint32_t addr) {
    if (!m_Enabled) return true;

    std::lock_guard<std::mutex> lock(m_Mutex);
    auto now = std::chrono::steady_clock::now();

    auto it = m_Buckets.find(addr);
    if (it == m_Buckets.end()) {
        m_Buckets[addr] = {m_Burst - 1.0, now};
        return true;
    }

    Bucket& bucket = it->second;
    double elapsed = std::chrono::duration<double>(now - bucket.last).count();
    bucket.last = now;
    bucket.tokens += elapsed * m_RefillPerSecond;
    if (bucket.tokens > m_Burst) bucket.tokens = m_Burst;

    if (bucket.tokens < 1.0) return false;
    bucket.tokens -= 1.0;
    return true;
}

void RateLimiter::Forget(uint32_t addr) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Buckets.erase(addr);
}

void FailureTracker::Configure(int maxFailures, int windowSeconds, int banSeconds) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Entries.clear();
    m_MaxFailures = maxFailures;
    m_WindowSeconds = windowSeconds > 0 ? windowSeconds : 60;
    m_BanSeconds = banSeconds > 0 ? banSeconds : 300;
    m_Enabled = maxFailures > 0;
}

bool FailureTracker::IsBlocked(uint32_t addr) {
    if (!m_Enabled) return false;

    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Entries.find(addr);
    if (it == m_Entries.end()) return false;

    auto now = std::chrono::steady_clock::now();
    if (it->second.blockedUntil > now) return true;

    if (it->second.blockedUntil != std::chrono::steady_clock::time_point{}) {
        m_Entries.erase(it);
    }
    return false;
}

bool FailureTracker::RecordFailure(uint32_t addr) {
    if (!m_Enabled) return false;

    std::lock_guard<std::mutex> lock(m_Mutex);
    auto now = std::chrono::steady_clock::now();
    Entry& entry = m_Entries[addr];

    if (entry.failures == 0 ||
        now - entry.windowStart > std::chrono::seconds(m_WindowSeconds)) {
        entry.windowStart = now;
        entry.failures = 0;
    }

    entry.failures++;
    if (entry.failures < m_MaxFailures) return false;

    entry.blockedUntil = now + std::chrono::seconds(m_BanSeconds);
    entry.failures = 0;
    return true;
}

void FailureTracker::RecordSuccess(uint32_t addr) {
    if (!m_Enabled) return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Entries.find(addr);
    if (it == m_Entries.end()) return;
    if (it->second.blockedUntil <= std::chrono::steady_clock::now()) m_Entries.erase(it);
}

int FailureTracker::SecondsRemaining(uint32_t addr) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Entries.find(addr);
    if (it == m_Entries.end()) return 0;
    auto now = std::chrono::steady_clock::now();
    if (it->second.blockedUntil <= now) return 0;
    return (int)std::chrono::duration_cast<std::chrono::seconds>(it->second.blockedUntil - now).count();
}

}
