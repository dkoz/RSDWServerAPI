#!/bin/bash
# RuneScape: Dragonwilds (Linux) + RSDWServerAPI - install script for the Pterodactyl/Pelican egg.
# Runs in ghcr.io/parkervcp/installers:debian with the server's files mounted at /mnt/server.

SERVER_DIR=/mnt/server
APPID="${SRCDS_APPID:-4019830}"

need_tools() {
    for tool in curl jq unzip tar; do
        command -v "${tool}" >/dev/null 2>&1 && continue
        apt-get update -qq && apt-get install -y -qq --no-install-recommends \
            curl jq unzip tar ca-certificates >/dev/null
        return
    done
}

install_game() {
    echo "[install] SteamCMD"
    mkdir -p "${SERVER_DIR}/steamcmd" "${SERVER_DIR}/steamapps"
    curl -fsSL -o /tmp/steamcmd.tar.gz \
        https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz || return 1
    tar -xzf /tmp/steamcmd.tar.gz -C "${SERVER_DIR}/steamcmd" || return 1

    export HOME="${SERVER_DIR}"
    cd "${SERVER_DIR}/steamcmd" || return 1

    # SteamCMD frequently exits non-zero on its first run while it self-updates.
    local tries=0
    until [ -x "${SERVER_DIR}/RSDragonwilds/Binaries/Linux/RSDragonwildsServer-Linux-Shipping" ]; do
        tries=$((tries + 1))
        [ "${tries}" -gt 3 ] && { echo "[install] SteamCMD failed after 3 attempts"; return 1; }
        echo "[install] Dragonwilds dedicated server, app ${APPID} (attempt ${tries})"
        ./steamcmd.sh +force_install_dir "${SERVER_DIR}" +login anonymous \
            +app_update "${APPID}" validate +quit
    done

    # Unreal's Steam subsystem looks for this in ~/.steam/sdk64 (HOME is /home/container).
    mkdir -p "${SERVER_DIR}/.steam/sdk64"
    cp -f linux64/steamclient.so "${SERVER_DIR}/.steam/sdk64/steamclient.so"
    chmod +x "${SERVER_DIR}/RSDragonwilds/Binaries/Linux/RSDragonwildsServer-Linux-Shipping"
}

# Picks the release asset by name, never by position. An earlier community installer took
# assets[0] and ended up running a different build than the one it reported (issue #5).
release_asset_url() {
    local repo="${RSDWAPI_REPO:-dkoz/RSDWServerAPI}"
    local release="${RSDWAPI_RELEASE:-latest}"
    local api="https://api.github.com/repos/${repo}/releases"
    if [ "${release}" = "latest" ]; then api="${api}/latest"; else api="${api}/tags/${release}"; fi

    local json
    json=$(curl -fsSL -H "Accept: application/vnd.github+json" "${api}") || return 1
    echo "${json}" | jq -r '
        [ (.assets[] | select(.name == "librsdwapi.so")),
          (.assets[] | select(.name | test("\\.so$"))),
          (.assets[] | select(.name | test("(?i)linux.*\\.zip$"))),
          (.assets[] | select(.name | test("\\.zip$"))) ]
        | first | .browser_download_url // empty'
}

install_rsdwapi() {
    if [ "${RSDWAPI_ENABLED:-1}" != "1" ] && [ "${RSDWAPI_ENABLED:-}" != "true" ]; then
        echo "[install] RSDWServerAPI disabled, skipping"
        return 0
    fi

    local url="${RSDWAPI_URL:-}"
    if [ -z "${url}" ]; then
        url=$(release_asset_url)
        if [ -z "${url}" ]; then
            echo "[install] WARNING: no RSDWServerAPI release asset found in" \
                 "${RSDWAPI_REPO:-dkoz/RSDWServerAPI} (${RSDWAPI_RELEASE:-latest})."
            echo "[install]          Upload librsdwapi.so to the server root or set the Download URL."
            return 0
        fi
    fi

    echo "[install] RSDWServerAPI from ${url}"
    local work=/tmp/rsdwapi
    rm -rf "${work}" && mkdir -p "${work}"
    if ! curl -fsSL -o "${work}/download" "${url}"; then
        echo "[install] WARNING: download failed, the server will start without the API"
        return 0
    fi

    local so="${work}/download"
    if unzip -tq "${so}" >/dev/null 2>&1; then
        unzip -oq "${so}" -d "${work}/x"
        so=$(find "${work}/x" -type f -name 'librsdwapi.so' | head -n 1)
    fi
    if [ -z "${so}" ] || [ "$(head -c 4 "${so}" | od -An -c | tr -d ' ')" != '177ELF' ]; then
        echo "[install] WARNING: download is not a Linux shared library, skipping"
        return 0
    fi

    install -m 0755 "${so}" "${SERVER_DIR}/librsdwapi.so"
    mkdir -p "${SERVER_DIR}/rsdwapi"
    echo "[install] RSDWServerAPI installed ($(stat -c %s "${SERVER_DIR}/librsdwapi.so") bytes)"
}

write_launcher() {
    cat > "${SERVER_DIR}/start.sh" <<'__START_SH__'
@@START_SH@@
__START_SH__
    chmod +x "${SERVER_DIR}/start.sh"
}

need_tools
install_game || { echo "[install] game install failed"; exit 1; }
install_rsdwapi
write_launcher
echo "[install] done"
