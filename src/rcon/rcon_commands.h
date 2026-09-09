#pragma once
#include <functional>
#include <string>
#include <vector>

namespace Rcon {

struct Command {
    std::string name;
    std::string usage;
    std::string description;
    std::function<std::string(const std::vector<std::string>& args)> handler;
};

void Register(const Command& command);
const std::vector<Command>& All();

std::string Dispatch(const std::string& line);

void RegisterSystemCommands();
void RegisterPlayerCommands();
void RegisterAll();

}
