# RSDWServerAPI

A server management API for **RuneScape: Dragonwilds** dedicated servers on Linux.

It adds a REST API and a Source RCON listener to a running server, so you can see who is
connected, read their live stats, and kick them remotely - from a panel, a script, a
Discord bot, or anything else that speaks HTTP.

The mod loads alongside the server with `LD_PRELOAD`. Game files are left untouched: no
paks are repacked and no assets are replaced. It reads state straight out of the running
process and calls the game's own functions when it needs to act, and the only thing it
writes is its own config and log folder.

**Target:** `RSDragonwildsServer-Linux-Shipping` (Steam app `4019830`).

## Features

- **Live player data** - names, character names, SteamID64, character GUID, ping,
  position, rotation, velocity, health, sustenance, hydration, skills and every float
  attribute the character carries.
- **Remote kick** through the game's own kick function, persistent until the server
  restarts, exactly like an in-game kick.
- **Broadcast** to all players through the game's own broadcast function.
- **Two independent listeners** - REST and Source RCON, each switchable on its own.
- **Access control on both** - bearer token or RCON password, IP whitelisting, per-address
  rate limiting and automatic blocking after repeated failed logins.
- **Offsets validated at runtime.** Every discovery step is checked and cross-checked
  before anything is read, and a mismatch after a game patch degrades to a `503` and a log
  line rather than serving nonsense.

## Build

Docker is the only requirement. The build container is Debian bookworm, matching the image
the dedicated server runs in, so the result never asks the host for a newer glibc than it
has. `libstdc++` and `libgcc` are linked statically.

```bash
./build.sh
```

Output: `dist/librsdwapi.so`.

A bare `make` or `xmake` also works if you already have a Linux toolchain.

## Install

Copy `dist/librsdwapi.so` to the server and prefix the startup command:

```bash
LD_PRELOAD=./librsdwapi.so ./RSDragonwilds/Binaries/Linux/RSDragonwildsServer-Linux-Shipping -log -port=7777
```

On Pterodactyl or Pelican, put the `.so` in the server root and set the startup to:

```
LD_PRELOAD=/home/container/librsdwapi.so ./RSDragonwilds/Binaries/Linux/RSDragonwildsServer-Linux-Shipping -log -port={{SERVER_PORT}} -ini:Game:[/Script/Engine.GameSession]:MaxPlayers={{MAX_PLAYERS}}
```

Allocate the API port (and the RCON port, if you turn it on) in the panel.

## Configuration

On first run the mod writes `rsdwapi/settings.ini` beside the server binary, which on a
stock install is `RSDragonwilds/Binaries/Linux/rsdwapi/`. Set `RSDWAPI_DIR` to put it
somewhere easier to reach - on a panel, `RSDWAPI_DIR=/home/container/rsdwapi`.

A bearer token and an RCON password are generated on that first run and never regenerated
afterwards, so an empty value stays empty.

**The REST API is on by default. RCON is off by default** - set `Enabled=true` under
`[RCON]` to use it; the password is already generated and waiting.

```ini
[General]
EnableLogging=true
VerboseLogging=true

[API]
Enabled=true
BindAddress=0.0.0.0
Port=8080
BearerToken=<generated>
IPWhitelist=

[RCON]
Enabled=false
BindAddress=0.0.0.0
Port=27020
Password=<generated>
IPWhitelist=
CommandsPerMinute=60
CommandBurst=15
MaxFailedAuth=5
FailWindowSeconds=60
BanSeconds=300
MaxConnections=16
```

`IPWhitelist` takes a comma separated list of addresses and CIDR blocks
(`127.0.0.1,10.0.0.0/8`). Empty allows everyone. It applies to both listeners
independently, and an unparseable entry is logged and ignored rather than silently locking
everyone out.

Clearing `BearerToken` disables REST authentication entirely. That is deliberate and it
sticks, but bind to `127.0.0.1` or set an `IPWhitelist` if you do it. A warning is logged
at startup either way.

An empty RCON `Password` keeps RCON from starting at all, by design.

Logs go to `<data dir>/logs/api.log`, rotated on each boot with ten kept, and mirrored to
the server's stdout so a panel console shows them.

## REST endpoints

Every request needs `Authorization: Bearer <token>` unless `BearerToken` is empty.

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/hello` | Load check |
| GET | `/api/health` | Mod status and uptime |
| GET | `/api/players` | Every connected player |
| POST | `/api/kick` | Disconnect a player |
| POST | `/api/broadcast` | Send a message to every player |

### GET /api/players

Accepts `?name=`, `?characterName=`, `?netId=` and `?characterGuid=` to narrow the list.

```bash
curl -H "Authorization: Bearer $TOKEN" http://localhost:8080/api/players
```

```json
{
  "count": 1,
  "players": [{
    "name": "ExamplePlayer",
    "characterName": "Adventurer",
    "playerId": 256,
    "uniqueNetId": "76561190000000000",
    "uniqueNetIdSource": "net-id-string",
    "characterGuid": "00000000000000000000000000000000",
    "platform": "WIN",
    "pingMs": 60,
    "score": 0.0,
    "startTime": 80,
    "isSpectator": false, "isBot": false, "isInactive": false,
    "spawned": true,
    "pawnClass": "BP_PlayerCharacter_C",
    "color": { "hex": "D47247FF", "r": 0.658, "g": 0.168, "b": 0.063, "a": 1.0 },
    "location": { "x": 10360.0, "y": 186410.0, "z": -3070.0 },
    "rotation": { "pitch": 0.0, "yaw": 128.2, "roll": 0.0 },
    "velocity": { "x": 0.0, "y": 0.0, "z": 0.0 },
    "movementMode": 1,
    "health":     { "current": 100.0, "max": 100.0, "canDie": true, "isDead": false },
    "sustenance": { "current": 72.0, "max": 100.0 },
    "hydration":  { "current": 65.0, "max": 100.0 },
    "attributes": [{ "name": "Health", "value": 100.0, "baseValue": 100.0, "shared": false }],
    "skills":     [{ "skill": "SKILL_Woodcutting", "currentXP": 4210 }],
    "pointers": { "playerState": "0x...", "pawn": "0x...", "controller": "0x..." }
  }]
}
```

Health and max health come from the character's attributes component, not from
`UHealthComponent`'s own fields - on a player those are always zero, because
`UPlayerHealthComponent` sources both from attributes.

### POST /api/kick

`player` accepts a SteamID64, player id, character GUID, character name or display name.
They are matched in that order, most stable first.

```bash
curl -X POST http://localhost:8080/api/kick \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"player":"76561190000000000","reason":"Kicked by an administrator"}'
```

From Windows `cmd.exe`, escape the quotes instead:

```
curl -X POST http://localhost:8080/api/kick -H "Authorization: Bearer %TOKEN%" -H "Content-Type: application/json" -d "{\"player\":\"76561190000000000\",\"reason\":\"Kicked by an administrator\"}"
```

A kick holds until the server restarts. The kicked player is remembered and removed again
if they reconnect.

### POST /api/broadcast

Sends a line to every connected player's chat box.

```bash
curl -X POST http://localhost:8080/api/broadcast \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"message":"Restarting in 5 minutes"}'
```

From Windows `cmd.exe`:

```
curl -X POST http://localhost:8080/api/broadcast -H "Authorization: Bearer %TOKEN%" -H "Content-Type: application/json" -d "{\"message\":\"Restarting in 5 minutes\"}"
```

`sender` is optional and defaults to `Server`, giving `[Server] Restarting in 5 minutes`.

The game has no server identity in chat, so a broadcast is delivered under the receiving
player's own name. Everyone sees the `[Server]` prefix, but it appears as though they sent
it themselves.

### GET /api/health

Deliberately minimal, because authentication can be turned off and this therefore has to be
safe to serve to anyone. No ports, no owner id, no server GUID, no memory addresses.

```json
{"status":"healthy","version":"0.1.0","uptimeSeconds":41.2,"engineReady":true}
```

`status` is `starting` until the world has loaded. Engine discovery detail goes to the log
at startup and to the authenticated RCON `status` command.

## RCON

Standard Valve Source RCON, so `mcrcon`, `rcon-cli` and panel consoles all work. Off by
default.

```bash
mcrcon -H 127.0.0.1 -P 27020 -p "$RCON_PASSWORD" players
```

| Command | Description |
|---------|-------------|
| `help` | List commands |
| `health` | Mod and engine readiness in one line |
| `status` | Server identity, world and engine discovery |
| `players [json]` | Player table, or the API payload |
| `playercount` | Number of connected players |
| `player <name>` | Everything known about one player, as JSON |
| `kick <player> [reason]` | Disconnect a player |
| `broadcast <message>` | Send a message to every player |

Rate limiting is a per-address token bucket: `CommandsPerMinute` is the sustained rate,
`CommandBurst` is how much can be spent at once. Separately, `MaxFailedAuth` failed logins
inside `FailWindowSeconds` refuse that address for `BanSeconds`, enforced at `accept`
before the password is read.

## SDK

The engine layer is driven by a Dumper-7 dump of the server binary, which is not included
in this repository. Generate your own and place it at `RSDWSDK/`. Every offset the mod uses
carries a `// Source:` comment naming the file it came from, so they can be re-verified
against a fresh dump after a game patch.

## License

Released under the MIT License. See [LICENSE](LICENSE) for the full text.

This software is provided as is, without warranty of any kind. Use it at your own risk.

If you use this code in your own project, please provide credit and keep a link back to
this repository.
