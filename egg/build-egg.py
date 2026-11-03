#!/usr/bin/env python3
"""Assembles egg-runescape-dragonwilds-rsdwapi.json from install.sh and start.sh.

Edit the shell scripts, not the JSON, then run:  python3 egg/build-egg.py
"""
import json
import pathlib
from datetime import datetime, timezone

HERE = pathlib.Path(__file__).resolve().parent

start_sh = (HERE / "start.sh").read_text()
install_sh = (HERE / "install.sh").read_text()
assert "__START_SH__" not in start_sh, "start.sh must not contain the heredoc delimiter"
install_sh = install_sh.replace("@@START_SH@@", start_sh.rstrip("\n"))


def var(name, env, default, description, rules, viewable=True, editable=True):
    return {
        "name": name,
        "description": description,
        "env_variable": env,
        "default_value": default,
        "user_viewable": viewable,
        "user_editable": editable,
        "rules": rules,
        "field_type": "text",
    }


variables = [
    var("Server Name", "SERVER_NAME", "Dragonwilds Server",
        "Name shown in the in-game server browser.", "required|string|max:64"),
    var("Owner ID", "OWNER_ID", "",
        "Your in-game Player ID (Settings menu, bottom of the list). Required - the server "
        "will not finish starting without it. The owner can grant admin and unban.",
        "required|string|max:64"),
    var("World Name", "DEFAULT_WORLD_NAME", "World",
        "Save name of the world to load or create.", "required|string|max:64"),
    var("World Password", "WORLD_PASSWORD", "",
        "Password players need to join. Leave blank for an open server.",
        "nullable|string|max:64"),
    var("Admin Password", "ADMIN_PASSWORD", "",
        "Entered in Pause > Settings > Server Management to become admin. Must differ from "
        "the World Password.", "nullable|string|max:64"),
    var("Max Players", "MAX_PLAYERS", "6",
        "Dedicated servers support up to 6 players.", "required|integer|between:1,6"),
    var("Auto Update", "AUTO_UPDATE", "1",
        "Update the game through SteamCMD on every start (1 = on, 0 = off). Client and server "
        "versions must match exactly or players cannot see the server.", "required|boolean"),
    var("Steam App ID", "SRCDS_APPID", "4019830",
        "RuneScape: Dragonwilds Dedicated Server tool.", "required|string|in:4019830",
        viewable=False, editable=False),

    var("RSDWServerAPI: Enabled", "RSDWAPI_ENABLED", "1",
        "Preload the RSDWServerAPI mod (REST API, RCON, chat admin commands, Discord relay).",
        "required|boolean"),
    var("RSDWServerAPI: GitHub Repository", "RSDWAPI_REPO", "dkoz/RSDWServerAPI",
        "owner/repo to download librsdwapi.so from on install. Point this at your own fork "
        "to run a patched build.", "required|string|regex:/^[A-Za-z0-9_.-]+\\/[A-Za-z0-9_.-]+$/"),
    var("RSDWServerAPI: Release", "RSDWAPI_RELEASE", "latest",
        "Release tag to install, or 'latest'. Applied on (re)install.", "required|string|max:64"),
    var("RSDWServerAPI: Download URL", "RSDWAPI_URL", "",
        "Optional direct link to librsdwapi.so or a zip containing it. Overrides the repository "
        "and release above.", "nullable|url|max:512"),
    var("RSDWServerAPI: REST Port", "RSDWAPI_API_PORT", "8080",
        "TCP port for the REST API. Must be an allocation assigned to this server.",
        "required|integer|between:1024,65535"),
    var("RSDWServerAPI: REST Bind Address", "RSDWAPI_API_BIND", "0.0.0.0",
        "0.0.0.0 to reach the API from outside the container (a bot or panel on another "
        "host); 127.0.0.1 to keep it inside.", "required|ip"),
    var("RSDWServerAPI: REST Bearer Token", "RSDWAPI_API_TOKEN", "",
        "Leave blank to keep the token generated on first start (see rsdwapi/settings.ini). "
        "Set it here to manage it from the panel.", "nullable|string|min:16|max:128"),
    var("RSDWServerAPI: REST IP Whitelist", "RSDWAPI_API_WHITELIST", "",
        "Comma separated addresses or CIDR blocks allowed to connect. Blank allows everyone.",
        "nullable|string|max:512"),
    var("RSDWServerAPI: RCON Enabled", "RSDWAPI_RCON_ENABLED", "0",
        "Source RCON listener (works with mcrcon, rcon-cli and panel RCON consoles).",
        "required|boolean"),
    var("RSDWServerAPI: RCON Port", "RSDWAPI_RCON_PORT", "27020",
        "TCP port for RCON. Must be an allocation assigned to this server.",
        "required|integer|between:1024,65535"),
    var("RSDWServerAPI: RCON Password", "RSDWAPI_RCON_PASSWORD", "",
        "Leave blank to keep the password generated on first start.",
        "nullable|string|min:8|max:128"),
    var("RSDWServerAPI: Discord Relay", "RSDWAPI_DISCORD_ENABLED", "0",
        "Post in-game chat and player deaths to Discord webhooks.", "required|boolean"),
    var("RSDWServerAPI: Discord Chat Webhook", "RSDWAPI_DISCORD_WEBHOOK", "",
        "Webhook URL for in-game chat.", "nullable|url|max:512"),
    var("RSDWServerAPI: Discord Death Webhook", "RSDWAPI_DISCORD_DEATH_WEBHOOK", "",
        "Webhook URL for deaths. Blank sends deaths to the chat webhook.",
        "nullable|url|max:512"),
]

egg = {
    "_comment": "DO NOT EDIT: generated by egg/build-egg.py from install.sh and start.sh",
    "meta": {"version": "PTDL_v2", "update_url": None},
    "exported_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S+00:00"),
    "name": "RuneScape: Dragonwilds (Linux + RSDWServerAPI)",
    "author": "cobratrinity@proton.me",
    "description": (
        "RuneScape: Dragonwilds dedicated server (Steam app 4019830, native Linux build) with "
        "the RSDWServerAPI mod preloaded: REST API, Source RCON, in-game /kick /ban /unban "
        "/give, broadcasts and a Discord chat/death relay. The game port is the primary "
        "allocation (UDP). Assign extra allocations for the REST and RCON ports and enter them "
        "in the variables. Note: this is the Linux build, so UE4SS mods (Windows build only) "
        "are not supported."
    ),
    "features": ["steam_disk_space"],
    "docker_images": {
        "SteamCMD Debian": "ghcr.io/parkervcp/steamcmd:debian",
    },
    "file_denylist": [],
    "startup": "bash ./start.sh",
    "config": {
        "files": "{}",
        "startup": json.dumps({"done": ["RSDWRestAPI: engine ready", "Engine is initialized"]}),
        "logs": "{}",
        "stop": "^C",
    },
    "scripts": {
        "installation": {
            "script": install_sh,
            "container": "ghcr.io/parkervcp/installers:debian",
            "entrypoint": "bash",
        }
    },
    "variables": variables,
}

out = HERE / "egg-runescape-dragonwilds-rsdwapi.json"
out.write_text(json.dumps(egg, indent=4) + "\n")
print(f"wrote {out.name}: {len(variables)} variables, install script {len(install_sh)} bytes")
