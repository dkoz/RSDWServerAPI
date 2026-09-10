# Plain g++ build. Run it through ./build.sh to get the Debian container,
# which is what the dedicated server actually runs on.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -fPIC -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -Isrc
# libstdc++ and libgcc are linked statically so the mod does not depend on the
# C++ runtime version of whatever image the server is hosted in.
LDFLAGS  ?= -shared -pthread -ldl -static-libstdc++ -static-libgcc

SRC := src/rsdwapi.cpp \
       src/runtime.cpp \
       src/api/api_routes.cpp \
       src/api/system_routes.cpp \
       src/api/player_routes.cpp \
       src/api/serialize.cpp \
       src/engine/engine_core.cpp \
       src/engine/uobject.cpp \
       src/engine/players.cpp \
       src/engine/kick.cpp \
       src/engine/chat.cpp \
       src/discord/webhook.cpp \
       src/engine/native_call.cpp \
       src/engine/process_event.cpp \
       src/engine/server.cpp \
       src/http/http_server.cpp \
       src/net/access_control.cpp \
       src/rcon/rcon_server.cpp \
       src/rcon/rcon_commands.cpp \
       src/rcon/commands_system.cpp \
       src/rcon/commands_players.cpp \
       src/utils/memory.cpp

OBJ := $(SRC:.cpp=.o)
OUT := dist/librsdwapi.so

all: $(OUT)

$(OUT): $(OBJ)
	@mkdir -p dist
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(OUT)

.PHONY: all clean
