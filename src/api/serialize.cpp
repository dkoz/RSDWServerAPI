#include "serialize.h"
#include "../utils/json.h"

#include <cstdio>
#include <sstream>

namespace Serialize {

namespace {

std::string Q(const std::string& value) {
    return "\"" + Json::Escape(value) + "\"";
}

std::string Num(double value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.3f", value);
    return buf;
}

std::string Bool(bool value) {
    return value ? "true" : "false";
}

std::string Pad(const std::string& value, size_t width) {
    std::string out = value.size() > width ? value.substr(0, width) : value;
    out.append(width - out.size(), ' ');
    return out;
}

}

std::string Player(const DomEngine::PlayerInfo& p) {
    std::ostringstream oss;
    oss << "{"
        << "\"name\":" << Q(p.name) << ","
        << "\"characterName\":" << Q(p.characterName) << ","
        << "\"playerId\":" << p.playerId << ","
        << "\"uniqueNetId\":" << Q(p.uniqueNetId) << ","
        << "\"uniqueNetIdSource\":" << Q(p.uniqueNetIdSource) << ","
        << "\"characterGuid\":" << Q(p.characterGuid) << ","
        << "\"platform\":" << Q(p.platform) << ","
        << "\"pingMs\":" << p.pingMs << ","
        << "\"score\":" << Num(p.score) << ","
        << "\"startTime\":" << p.startTime << ","
        << "\"isSpectator\":" << Bool(p.isSpectator) << ","
        << "\"isBot\":" << Bool(p.isBot) << ","
        << "\"isInactive\":" << Bool(p.isInactive) << ","
        << "\"spawned\":" << Bool(p.spawned) << ","
        << "\"pawnClass\":" << Q(p.pawnClass) << ",";

    oss << "\"color\":{"
        << "\"hex\":" << Q(p.colorHex) << ","
        << "\"r\":" << Num(p.color[0]) << ","
        << "\"g\":" << Num(p.color[1]) << ","
        << "\"b\":" << Num(p.color[2]) << ","
        << "\"a\":" << Num(p.color[3])
        << "},";

    if (p.hasLocation) {
        oss << "\"location\":{\"x\":" << Num(p.x) << ",\"y\":" << Num(p.y)
            << ",\"z\":" << Num(p.z) << "},"
            << "\"rotation\":{\"pitch\":" << Num(p.pitch) << ",\"yaw\":" << Num(p.yaw)
            << ",\"roll\":" << Num(p.roll) << "},";
    } else {
        oss << "\"location\":null,\"rotation\":null,";
    }

    if (p.hasVelocity) {
        oss << "\"velocity\":{\"x\":" << Num(p.vx) << ",\"y\":" << Num(p.vy)
            << ",\"z\":" << Num(p.vz) << "},"
            << "\"movementMode\":" << (int)p.movementMode << ",";
    } else {
        oss << "\"velocity\":null,\"movementMode\":null,";
    }

    if (p.hasHealth) {
        oss << "\"health\":{\"current\":" << Num(p.health) << ",\"max\":" << Num(p.maxHealth)
            << ",\"canDie\":" << Bool(p.canDie) << ",\"isDead\":" << Bool(p.isDead) << "},";
    } else {
        oss << "\"health\":null,";
    }

    if (p.hasSustenance) {
        oss << "\"sustenance\":{\"current\":" << Num(p.sustenance)
            << ",\"max\":" << Num(p.maxSustenance) << "},";
    } else {
        oss << "\"sustenance\":null,";
    }

    if (p.hasHydration) {
        oss << "\"hydration\":{\"current\":" << Num(p.hydration)
            << ",\"max\":" << Num(p.maxHydration) << "},";
    } else {
        oss << "\"hydration\":null,";
    }

    oss << "\"attributes\":[";
    for (size_t i = 0; i < p.attributes.size(); i++) {
        if (i) oss << ",";
        oss << "{\"name\":" << Q(p.attributes[i].name)
            << ",\"value\":" << Num(p.attributes[i].value)
            << ",\"baseValue\":" << Num(p.attributes[i].baseValue)
            << ",\"shared\":" << Bool(p.attributes[i].shared) << "}";
    }
    oss << "],";

    oss << "\"skills\":[";
    for (size_t i = 0; i < p.skills.size(); i++) {
        if (i) oss << ",";
        oss << "{\"skill\":" << Q(p.skills[i].skill) << ",\"currentXP\":" << p.skills[i].currentXP << "}";
    }
    oss << "],";

    char pointers[128];
    snprintf(pointers, sizeof(pointers),
             "\"pointers\":{\"playerState\":\"0x%llX\",\"pawn\":\"0x%llX\",\"controller\":\"0x%llX\"}",
             (unsigned long long)p.playerStatePtr, (unsigned long long)p.pawnPtr,
             (unsigned long long)p.controllerPtr);
    oss << pointers << "}";

    return oss.str();
}

std::string Players(const std::vector<DomEngine::PlayerInfo>& players) {
    std::ostringstream oss;
    oss << "{\"count\":" << players.size() << ",\"players\":[";
    for (size_t i = 0; i < players.size(); i++) {
        if (i) oss << ",";
        oss << Player(players[i]);
    }
    oss << "]}";
    return oss.str();
}

std::string Server(const DomEngine::ServerInfo& info) {
    std::ostringstream oss;
    oss << "{"
        << "\"valid\":" << Bool(info.valid) << ","
        << "\"ownerId\":" << Q(info.ownerId) << ","
        << "\"serverName\":" << Q(info.serverName) << ","
        << "\"worldName\":" << Q(info.worldName) << ","
        << "\"serverGuid\":" << Q(info.serverGuid) << ","
        << "\"isDedicatedServer\":" << Bool(info.isDedicatedServer) << ","
        << "\"crossplayEnabled\":" << Bool(info.crossplayEnabled) << ","
        << "\"hardcoreState\":" << info.hardcoreState
        << "}";
    return oss.str();
}

std::string Discovery(const DomEngine::DiscoveryInfo& info) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"moduleBase\":\"0x%llX\",\"gObjects\":\"0x%llX\",\"gNames\":\"0x%llX\","
             "\"world\":\"0x%llX\",\"objectCount\":%d,",
             (unsigned long long)info.moduleBase, (unsigned long long)info.gObjects,
             (unsigned long long)info.gNames, (unsigned long long)info.world, info.objectCount);

    std::ostringstream oss;
    oss << buf
        << "\"gObjectsSource\":" << Q(info.gObjectsSource) << ","
        << "\"gNamesSource\":" << Q(info.gNamesSource) << ","
        << "\"worldSource\":" << Q(info.worldSource)
        << "}";
    return oss.str();
}

std::string PlayersTable(const std::vector<DomEngine::PlayerInfo>& players) {
    if (players.empty()) return "No players connected.";

    std::ostringstream oss;
    oss << Pad("NAME", 20) << Pad("CHARACTER", 18) << Pad("ID", 5) << Pad("PING", 6)
        << Pad("HP", 12) << Pad("FOOD", 8) << Pad("WATER", 8) << "LOCATION\n";

    for (const DomEngine::PlayerInfo& p : players) {
        char health[32] = "-";
        if (p.hasHealth) snprintf(health, sizeof(health), "%.0f/%.0f", p.health, p.maxHealth);

        char food[32] = "-";
        if (p.hasSustenance) snprintf(food, sizeof(food), "%.0f", p.sustenance);

        char water[32] = "-";
        if (p.hasHydration) snprintf(water, sizeof(water), "%.0f", p.hydration);

        char location[64] = "not spawned";
        if (p.hasLocation) snprintf(location, sizeof(location), "%.0f %.0f %.0f", p.x, p.y, p.z);

        oss << Pad(p.name, 20) << Pad(p.characterName, 18) << Pad(std::to_string(p.playerId), 5)
            << Pad(std::to_string(p.pingMs), 6) << Pad(health, 12) << Pad(food, 8)
            << Pad(water, 8) << location << "\n";
    }

    oss << "\n" << players.size() << " player(s) connected.";
    return oss.str();
}

}
