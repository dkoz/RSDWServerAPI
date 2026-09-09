#pragma once
#include <string>
#include <cctype>
#include <cstddef>

namespace Json {

inline std::string Escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) continue;
                out += (char)c;
        }
    }
    return out;
}

inline size_t FindValue(const std::string& body, const char* key) {
    std::string quoted = std::string("\"") + key + "\"";
    size_t keyPos = body.find(quoted);
    if (keyPos == std::string::npos) return std::string::npos;

    size_t colon = body.find(':', keyPos + quoted.length());
    if (colon == std::string::npos) return std::string::npos;

    size_t i = colon + 1;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) i++;
    return i;
}

inline bool GetString(const std::string& body, const char* key, std::string& out) {
    size_t i = FindValue(body, key);
    if (i == std::string::npos || i >= body.size() || body[i] != '"') return false;
    i++;

    std::string value;
    while (i < body.size() && body[i] != '"') {
        if (body[i] == '\\' && i + 1 < body.size()) {
            i++;
            switch (body[i]) {
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                case 'b': value += '\b'; break;
                case 'f': value += '\f'; break;
                case '/': value += '/'; break;
                case '"': value += '"'; break;
                case '\\': value += '\\'; break;
                default: value += body[i];
            }
        } else {
            value += body[i];
        }
        i++;
    }

    out = value;
    return true;
}

inline bool GetNumber(const std::string& body, const char* key, std::string& out) {
    size_t i = FindValue(body, key);
    if (i == std::string::npos) return false;

    std::string num;
    while (i < body.size() && (isdigit((unsigned char)body[i]) || body[i] == '.' || body[i] == '-')) {
        num += body[i];
        i++;
    }
    if (num.empty()) return false;

    out = num;
    return true;
}

inline bool GetFloat(const std::string& body, const char* key, float& out) {
    std::string num;
    if (!GetNumber(body, key, num)) return false;
    try {
        out = std::stof(num);
    } catch (...) {
        return false;
    }
    return true;
}

inline bool GetBool(const std::string& body, const char* key, bool& out) {
    size_t i = FindValue(body, key);
    if (i == std::string::npos) return false;

    if (body.compare(i, 4, "true") == 0)  { out = true;  return true; }
    if (body.compare(i, 5, "false") == 0) { out = false; return true; }
    return false;
}

inline bool GetInt(const std::string& body, const char* key, int& out) {
    std::string num;
    if (!GetNumber(body, key, num)) return false;
    try {
        out = std::stoi(num);
    } catch (...) {
        return false;
    }
    return true;
}

}
