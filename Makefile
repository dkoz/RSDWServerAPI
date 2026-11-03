# Plain g++ build. Run it through ./build.sh to get the Debian container,
# which is what the dedicated server actually runs on.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -fPIC -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -Isrc
# libstdc++ and libgcc are linked statically so the mod does not depend on the
# C++ runtime version of whatever image the server is hosted in.
LDFLAGS  ?= -shared -pthread -ldl -static-libstdc++ -static-libgcc

VERSION := $(shell sed -n 's/^\#define RSDWAPI_VERSION "\(.*\)"/\1/p' src/version.h)

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
       src/engine/items.cpp \
       src/engine/bans.cpp \
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
       src/utils/memory.cpp \
       src/utils/glibc_compat.cpp

# Newest glibc the result may need: Debian bookworm, which the server image runs on.
MAX_GLIBC := 2.36

OBJ := $(SRC:.cpp=.o)
DEP := $(OBJ:.o=.d)
OUT := dist/librsdwapi.so

all: $(OUT)

$(OUT): $(OBJ)
	@mkdir -p dist
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

# -MMD -MP: every object depends on the headers it includes. Without this, changing a
# struct in a header (PlayerInfo in dom_engine.h) rebuilt only the edited .cpp files and
# left the rest compiled against the old layout - a mismatched sizeof that crashed a live
# server the moment a player joined.
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEP)

# Forces a full build from scratch (release builds should use this).
rebuild: clean all

version:
	@echo $(VERSION)

# Unit tests run against a synthetic UObject world; chat.cpp is compiled into the test itself.
TEST_OBJ := $(filter-out src/engine/chat.o,$(OBJ))

test: $(TEST_OBJ)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) tests/chat_discovery_test.cpp $(TEST_OBJ) -o build/chat_discovery_test -pthread -ldl
	./build/chat_discovery_test

# Fails if the library would not load on the server image's glibc.
glibc-check: $(OUT)
	@need=$$(objdump -T $(OUT) | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -V | tail -1); \
	if [ "$$(printf '%s\n%s\n' "$$need" "$(MAX_GLIBC)" | sort -V | tail -1)" != "$(MAX_GLIBC)" ]; then \
		echo "glibc-check: needs GLIBC_$$need, server image has $(MAX_GLIBC)"; \
		objdump -T $(OUT) | grep -E "GLIBC_$$need"; exit 1; \
	else echo "glibc-check: OK (needs GLIBC_$$need, limit $(MAX_GLIBC))"; fi

clean:
	rm -f $(OBJ) $(DEP) $(OUT) build/chat_discovery_test

.PHONY: all clean rebuild version glibc-check test
