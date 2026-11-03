#!/bin/bash
# RuneScape: Dragonwilds + RSDWServerAPI - per-boot launcher for the Pterodactyl/Pelican egg.
# The egg's install script writes this file; reinstalling regenerates it. Every setting
# comes from the panel's startup variables, which Wings exports as environment variables.

cd /home/container || exit 1

GAME_BIN=./RSDragonwilds/Binaries/Linux/RSDragonwildsServer-Linux-Shipping
INI_DIR=RSDragonwilds/Saved/Config/LinuxServer
INI="${INI_DIR}/DedicatedServer.ini"
SECTION='[/Script/Dominion.DedicatedServerSettings]'

if [ ! -x "${GAME_BIN}" ]; then
    echo "[egg] ${GAME_BIN} is missing - reinstall the server (Settings > Reinstall)."
    exit 1
fi

# --- DedicatedServer.ini -------------------------------------------------------------
# Written on every boot: the game rewrites this file when it shuts down and strips
# AdminPassword from it at start, so panel values have to be re-applied each time.
# Other keys (ServerGuid in particular, generated on first run) are left untouched.
mkdir -p "${INI_DIR}"
[ -f "${INI}" ] || printf '%s\n' "${SECTION}" > "${INI}"
grep -qxF "${SECTION}" "${INI}" || printf '\n%s\n' "${SECTION}" >> "${INI}"

# set_key KEY VALUE - replace KEY inside SECTION, or add it to the end of SECTION.
# Values go through ENVIRON, not awk -v, so backslashes in passwords survive intact.
set_key() {
    # Blank lines inside the section are held back so a new key is added after the last
    # real line of the section, not after the gap before the next section header.
    INI_KEY="$1" INI_VAL="$2" INI_SEC="${SECTION}" awk '
        function flush() {
            if (insec && !done) { print ENVIRON["INI_KEY"] "=" ENVIRON["INI_VAL"]; done = 1 }
            for (; blanks > 0; blanks--) print ""
        }
        /^\[/ { flush(); insec = ($0 == ENVIRON["INI_SEC"]); print; next }
        insec && /^[ \t]*$/ { blanks++; next }
        { for (; blanks > 0; blanks--) print "" }
        insec && index($0, ENVIRON["INI_KEY"] "=") == 1 {
            if (!done) { print ENVIRON["INI_KEY"] "=" ENVIRON["INI_VAL"]; done = 1 }
            next
        }
        { print }
        END { flush() }
    ' "${INI}" > "${INI}.tmp" && mv "${INI}.tmp" "${INI}"
}

set_key ServerName       "${SERVER_NAME:-Dragonwilds Server}"
set_key DefaultWorldName "${DEFAULT_WORLD_NAME:-World}"
set_key OwnerId          "${OWNER_ID:-}"
set_key WorldPassword    "${WORLD_PASSWORD:-}"
set_key AdminPassword    "${ADMIN_PASSWORD:-}"

if [ -z "${OWNER_ID:-}" ]; then
    echo "[egg] WARNING: Owner ID is empty. The server will not finish starting without it."
    echo "[egg]          In game: Settings > scroll to the bottom > Player ID."
fi
if [ -n "${ADMIN_PASSWORD:-}" ] && [ "${ADMIN_PASSWORD}" = "${WORLD_PASSWORD:-}" ]; then
    echo "[egg] WARNING: Admin Password equals World Password - every player who can join"
    echo "[egg]          the world becomes an admin. Use two different values."
fi

# --- RSDWServerAPI -------------------------------------------------------------------
# Preloaded into the game binary only. The mod ignores every other process (bash, the
# stock .sh wrapper, crash reporters), so the export is safe. RSDWAPI_* variables from the
# panel override settings.ini on each boot; blank ones are ignored.
if [ "${RSDWAPI_ENABLED:-1}" = "1" ] || [ "${RSDWAPI_ENABLED:-}" = "true" ]; then
    if [ -f /home/container/librsdwapi.so ]; then
        export LD_PRELOAD=/home/container/librsdwapi.so
        export RSDWAPI_DIR=/home/container/rsdwapi
        echo "[egg] RSDWServerAPI enabled: REST on ${RSDWAPI_API_BIND:-0.0.0.0}:${RSDWAPI_API_PORT:-8080}," \
             "RCON $( [ "${RSDWAPI_RCON_ENABLED:-0}" = "1" ] || [ "${RSDWAPI_RCON_ENABLED:-}" = "true" ] \
                      && echo "on port ${RSDWAPI_RCON_PORT:-27020}" || echo "off")." \
             "Config and logs: rsdwapi/"
    else
        echo "[egg] RSDWServerAPI is enabled but librsdwapi.so is missing - reinstall, or upload"
        echo "[egg] the file to the server root. Starting without it."
    fi
fi

exec "${GAME_BIN}" -log -Port="${SERVER_PORT}" \
    "-ini:Game:[/Script/Engine.GameSession]:MaxPlayers=${MAX_PLAYERS:-6}"
