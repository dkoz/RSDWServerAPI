#include "rcon_commands.h"

#include <algorithm>
#include <mutex>
#include <sstream>

namespace Rcon {

namespace {

std::mutex& RegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<Command>& Registry() {
    static std::vector<Command> registry;
    return registry;
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return value;
}

}

void Register(const Command& command) {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    Registry().push_back(command);
}

const std::vector<Command>& All() {
    return Registry();
}

std::string Dispatch(const std::string& line) {
    std::istringstream stream(line);
    std::string name;
    stream >> name;
    if (name.empty()) return "";

    std::vector<std::string> args;
    std::string arg;
    while (stream >> arg) args.push_back(arg);

    std::string wanted = Lower(name);

    Command matched;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(RegistryMutex());
        for (const Command& command : Registry()) {
            if (Lower(command.name) != wanted) continue;
            matched = command;
            found = true;
            break;
        }
    }

    if (!found) return "Unknown command '" + name + "'. Try 'help'.";

    try {
        return matched.handler(args);
    } catch (const std::exception& e) {
        return std::string("Command failed: ") + e.what();
    } catch (...) {
        return "Command failed.";
    }
}

void RegisterAll() {
    RegisterSystemCommands();
    RegisterPlayerCommands();
}

}
