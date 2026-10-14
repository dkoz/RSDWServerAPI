# 🐉 RSDWServerAPI

[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)[![Version](https://img.shields.io/badge/version-0.1.6-blue.svg)](#-compatibility)
[![Game](https://img.shields.io/badge/game-RuneScape%3A%20Dragonwilds%201.0.1-orange.svg)](#-compatibility)
[![Engine](https://img.shields.io/badge/engine-UE%205.6.1-purple.svg)](#-compatibility)

A server management API for **RuneScape: Dragonwilds** dedicated servers on Linux.

It injects a REST API, a Source RCON listener, an in-game admin command set and an
optional Discord relay into a running server — so you can see who is connected, read
their live stats, teleport them, heal them, kick or ban them remotely: from a panel,
a script, a Discord bot, or anything else that speaks HTTP.

> 🇫🇷 [Version française](README.fr.md)
>
> 💡 This fork carries the production-hardened fixes and the build 1.0.1 offsets
> (`fix/engine-ready-listeners-and-header-case` branch, see
> [PR #3](https://github.com/dkoz/RSDWServerAPI/pull/3) upstream).

---

## 📦 Compatibility

Verified live on a production dedicated server (Ubuntu, systemd + `LD_PRELOAD`).

| Game version | Build (CL) | Steam build | Linux depot | Status |
|---|---|---|---|---|
| 1.0.0.x | 247725 (`++dominion+hotfix`) | — | — | ✅ verified (offsets SDK) |
| **1.0.1** "Eye on Ashenfall" | **248854** (`++dominion+staging`) | **25811348** | **3501791** | ✅ verified (offsets updated) |

- Engine: **UE 5.6.1** — unchanged between the two builds above.
- Platform: Linux dedicated server (`RSDragonwildsServer-Linux-Shipping`, image base
  `0x200000`, stripped: no `.symtab`, `UObject::ProcessEvent` not exported).

## ✨ Features

- 📡 **Live player data** — names, character GUID, platform, ping, position, rotation,
  velocity, health, sustenance, hydration, every skill with XP, every float attribute.
- 🚪 **Remote kick** through the game's own kick function, persistent until restart.
- 📢 **Broadcast** and 💬 **private tell** through the game's own chat.
- 🎁 **Give items** through the game's own give function.
- 🔨 **Bans** that survive restarts, offline players bannable by SteamID64.
- 🌀 **Teleport** — player onto player, or to coordinates — via the game's native
  `K2_TeleportTo` on the game thread (engine-acknowledged, replicated).
- 💚 **Admin actions** — heal (native `IncreaseHealth`), feed, hydrate, god mode.
- 🎮 **In-game admin commands** — `/kick`, `/ban`, `/unban`, `/give`, gated by the
  game's own owner/admin list.
- 📣 **Discord relay** — in-game chat and player deaths posted to webhooks.
- 🔌 **Two independent listeners** — REST and Source RCON, each switchable on its own.
- 🔐 **Access control on both** — bearer token or RCON password, IP whitelisting,
  per-address rate limiting and auto-blocking after repeated failed logins.
- 🛡️ **Offsets validated at runtime** — every discovery step is checked and
  cross-checked before anything is read; a mismatch after a game patch degrades to a
  `503` and a log line rather than serving nonsense.

## 🚀 Quick start

### Build

Docker is the only requirement. The build container is Debian bookworm, matching the
image the dedicated server runs in, so the result never asks the host for a newer
glibc than it has. `libstdc++` and `libgcc` are linked statically.

```bash
./build.sh
```

Output: `dist/librsdwapi.so`.

A bare `make` also works with a local toolchain; on hosts without static `libstdc++`
(Fedora, …), drop the static flags:

```bash
make LDFLAGS="-shared -pthread -ldl"
```

### Install

Copy `dist/librsdwapi.so` to the server root and prefix the startup command:

```bash
LD_PRELOAD=./librsdwapi.so ./RSDragonwilds/Binaries/Linux/RSDragonwildsServer-Linux-Shipping -log -port=7777
```

### systemd (production setup)

The drop-in below is what runs in production: `LD_PRELOAD` is applied service-wide
(the launcher script *and* the game binary get injected — harmless, see
[🏗️ How it works](#-how-it-works)), and the data directory is pinned with
`RSDWAPI_DIR`:

```ini
# /etc/systemd/system/dragonwilds.service.d/rsdwapi.conf
[Service]
Environment=RSDWAPI_DIR=/home/steam/dragonwilds_linux_server/rsdwapi
Environment=LD_PRELOAD=/home/steam/dragonwilds_linux_server/librsdwapi.so
```

On Pterodactyl or Pelican, put the `.so` in the server root and set the startup to:

```
LD_PRELOAD=/home/container/librsdwapi.so ./RSDragonwilds/Binaries/Linux/RSDragonwildsServer-Linux-Shipping -log -port={{SERVER_PORT}} -ini:Game:[/Script/Engine.GameSession]:MaxPlayers={{MAX_PLAYERS}}
```

Allocate the API port (and the RCON port, if you turn it on) in the panel.

## ⚙️ Configuration

On first run the mod writes `rsdwapi/settings.ini` beside the server binary
(`RSDragonwilds/Binaries/Linux/rsdwapi/` on a stock install). Set `RSDWAPI_DIR` to put
it somewhere easier to reach. The file is re-read at server start.

A bearer token and an RCON password are generated on that first run and never
regenerated afterwards — an empty value stays empty.

**The REST API is on by default. RCON is off by default.**

```ini
[General]
EnableLogging=true        # discovery steps + every request
VerboseLogging=true       # extra scan detail; leave on unless noisy

[API]
Enabled=true
BindAddress=0.0.0.0       # 🔒 prefer 127.0.0.1 + a token in production
Port=8080
BearerToken=              # empty = no auth at all (not recommended)
IPWhitelist=              # comma-separated IPs / CIDRs; empty = everyone

[RCON]
Enabled=false             # Source RCON, off by default
BindAddress=0.0.0.0
Port=27020
Password=                 # empty = RCON does not start, by design
IPWhitelist=
CommandsPerMinute=60
CommandBurst=15
MaxFailedAuth=5
FailWindowSeconds=60
BanSeconds=300
MaxConnections=16

[Discord]
Enabled=false
WebhookUrl=               # chat relay
DeathWebhookUrl=          # deaths; falls back to WebhookUrl when empty
Username=Dragonwilds
```

## 🌐 REST API

All endpoints live under `/api/`. When a bearer token is configured, send
`Authorization: Bearer <token>` (header names are case-insensitive — reqwest/hyper
lowercase them and that is handled).

- `200` with `{"status":"ok","message":"…"}` on success.
- `401` missing/wrong token · `404` unknown route · `503` engine not ready
  (`{"error":"engine not initialised"}`).

### Reads

| Route | Description |
|---|---|
| `GET /api/hello` | Liveness ping. |
| `GET /api/health` | `{"status","version","uptimeSeconds","engineReady"}`. |
| `GET /api/players` | All connected players, full detail. Optional query filters: `name`, `netId`, `characterName`, `characterGuid`. |
| `GET /api/items` | `{"items":[{"name","maxStackSize"}]}` — every `ItemData` object. |
| `GET /api/bans` | `{"bans":[{"netId","name","reason","bannedAt"}]}` — persistent bans. |
| `GET /api/status` | Server identity: `serverName`, `worldName`, `ownerId`, `serverGuid`, `isDedicatedServer`, `crossplayEnabled`, world state. |

`/api/players` entry (fields are all optional — not every platform exposes every one):

```json
{
  "name": "ApachaiZ", "characterName": "ApachaiZ", "playerId": 256,
  "uniqueNetId": "1405941075059060974", "characterGuid": "57D6786F…",
  "platform": "PS5", "pingMs": 60, "spawned": true,
  "health": {"current": 180.0, "max": 180.0, "canDie": true, "isDead": false},
  "sustenance": {"current": 74.2, "max": 100.0},
  "hydration": {"current": 64.4, "max": 100.0},
  "location": {"x": 66887.4, "y": 185273.2, "z": 138.2},
  "rotation": {"pitch": 0.0, "yaw": 145.7, "roll": 0.0},
  "velocity": {"x": 0.0, "y": 0.0, "z": 0.0},
  "skills": [{"skill": "SKILL_Woodcutting", "currentXP": 69561}],
  "attributes": [{"name": "Health", "value": 180.0, "baseValue": 103.5, "shared": true}]
}
```

### Actions

Player identifiers are matched against `playerId`, `uniqueNetId`, `characterGuid`,
`characterName` and `name` — the same string works everywhere.

| Route | Body | Notes |
|---|---|---|
| `POST /api/kick` | `{"player","reason"?}` | Game-native kick, re-enforced while the server runs. |
| `POST /api/broadcast` | `{"message","sender"?}` | Game-native chat to everyone. |
| `POST /api/tell` | `{"player","message"}` | Private message; needs the ProcessEvent derivation (see below). |
| `POST /api/give` | `{"player","item","count"?}` | `count` defaults to 1. |
| `POST /api/ban` | `{"player","reason"?}` | Persistent; offline players by SteamID64. |
| `POST /api/unban` | `{"player"}` | |
| `POST /api/teleport` | `{"from","to"}` | Teleport `from` onto `to`'s position. |
| `POST /api/tpto` | `{"player","x","y","z"}` | Absolute world coordinates (doubles). |
| `POST /api/heal` | `{"player","amount"?}` | Full heal by default; `amount` heals partially, clamped to max. |
| `POST /api/feed` | `{"player"}` | Sustenance → max. |
| `POST /api/hydrate` | `{"player"}` | Hydration → max. |
| `POST /api/godmode` | `{"player","enabled"?}` | Toggles `bCanDie`; default `true`. |

```bash
curl -H "Authorization: Bearer $TOKEN" -X POST \
  -d '{"from":"ApachaiZ","to":"Seiylvana"}' http://127.0.0.1:8080/api/teleport
# → {"status":"ok","message":"teleported ApachaiZ to Seiylvana"}
```

## 🎮 In-game chat commands

For players the game considers owner or admin (`PrivilegeFunctionLibrary`):

```
/kick <player> [reason]
/ban <player> [reason]
/unban <player or SteamID64>
/give <player> <item> [count]
```

> ⚠️ The admin check resolves lazily on first use: game-feature classes register
> after the mod boots, so a one-shot lookup can miss them. This is handled in the
> code — the lookup retries on the first command instead of failing the whole run.

## 💬 RCON

Source RCON on the configured port (off by default). Commands:

```
help                      status
players [json]            player <name>
playercount
kick <player> [reason]    ban <player> [reason]     unban <player|id>
broadcast <message>       tell <player> <message>
give <player> <item> [count]
tp <player> <target>      tpto <player> <x> <y> <z>
heal <player>             feed <player>             hydrate <player>
godmode <player> <on|off>
```

## 🏗️ How it works

The mod is a shared object injected with `LD_PRELOAD`. Its constructor spawns an init
thread that waits for the game engine to come up, then attaches readers and hooks.
**Nothing is read or called before the engine validates.**

### 🔍 Discovery (why patches don't crash it)

UE globals are located through SDK offsets **and** fallback scans, each validated:

1. `GNames` (the FName pool) — SDK offset first; on failure, every writable segment is
   scanned for a pool whose first entry decodes to `None`. Name strings resolve from
   here.
2. `GObjects` (the `TUObjectArray`) — SDK offset validated against the chunk structure;
   on failure, a writable-segment scan. Objects are walked by chunk.
3. `GWorld` — SDK offset validated (`World` class + persistent level), else a sweep of
   `GObjects` for the live `World`.

Class names, function lookups and every field read go through validated pointers with
readability checks — a moved field degrades to missing data, not a crash.

Current offsets (build 1.0.1 / CL 248854, image base `0x200000`):

| Global | Relative | Absolute |
|---|---|---|
| `GNames` | `0x0DBDAFF8` | `0xDDDAFF8` |
| `GObjects` | `0x0DC96510` | `0xDE96510` |
| `GWorld` | `0x0DD525F8` | `0xDF525F8` |

### 🧵 Game thread marshalling

Native UE functions must run on the **game thread**. The mod never calls them from its
HTTP thread:

- A pump is installed on a native server RPC (`PlayerController.ServerUpdateCamera`);
  every time a player triggers it, work queued with `GameThread::RunSync` is drained on
  the game thread. That is how teleport, heal, give and kicks execute natively.
- `UObject::ProcessEvent` is not exported on Linux Shipping builds, so the mod derives
  its address at runtime (stack walk + shared vtable slot); until derived,
  ProcessEvent-backed features (`tell`, `broadcast`) stay off with a clear log line.

### 🧩 Native calls and the object pool

`FindFunction(className, functionName)` sweeps the live object pool and matches by
name — which is why game updates that **rename** UFunctions matter more than ones that
move offsets. On 1.0.1, `AActor::TeleportTo` disappeared from the pool; the mod falls
back to `K2_TeleportTo` (the BlueprintCallable wrapper) with the matching return offset.

## 🔧 Adapting to a new game build

When the game patches, expect one or more of these — in order of likelihood:

1. **Globals moved** (GNames/GObjects/GWorld). The segment scan finds the new values;
   run the server once, read the log (`GNames at … (segment scan)`), and update
   `Offsets::G*` in `src/engine/dom_engine.h`. The previous values are kept in the
   comment.
2. **Struct fields moved** (class member offsets in `dom_engine.h`). Symptom: plausible
   but wrong values, or empty fields in `/api/players`. Re-dump with the SDK
   (RSDWSDK/Dumpspace) or bisect with a read-only `/proc/<pid>/mem` probe.
3. **UFunctions renamed/removed** (e.g. `TeleportTo` → `K2_TeleportTo`). Symptom:
   `… not found` log lines and a fallback path. Fix: resolve the new name in the pool.
4. **Late-registered classes** (game features). Symptom: a lookup works on some runs
   and not others. Fix: resolve lazily on first use, never once at boot.
5. **Engine upgrade** (UE minor/major). Everything can shift; re-dump the SDK.

The safest loop: deploy → watch the startup log (every discovery step is logged) →
spot-check `/api/health` + `/api/players` + one action endpoint → only then trust it.

## 🔒 Security

- 🔐 Bind `127.0.0.1` and set a bearer token in production; leave `IPWhitelist` for
  multi-host setups. The API is unauthenticated otherwise, by design.
- 📵 RCON stays off unless you need it; its password is generated once.
- 🚦 Failed RCON logins are rate-limited and auto-blocked per address.

## 🩺 Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `/api/health` answers `engineReady:false` forever | The launcher wrapper (also injected) grabbed the REST port first | Fixed: listeners bind only in the process where the engine initialized; upgrade the mod |
| Clients (reqwest/hyper, bots) get `401` with the right token; `curl` works | Header names arrive lowercase (`authorization`); case-sensitive lookup | Fixed: names lowercased at parse (RFC 9110); upgrade the mod |
| `chat commands unavailable` / lookups fail on some runs | Game-feature classes register after boot | Fixed: lazy re-resolution on first use |
| Heal/teleport "succeeds" but the player sees nothing | Direct memory writes bypass UE replication (no dirty mark) | Fixed: native functions on the game thread (`IncreaseHealth`, `K2_TeleportTo`) |
| Everything answers `503` | Engine discovery failed | Check the startup log — a game update probably moved the globals; see [🔧 Adapting](#-adapting-to-a-new-game-build) |
| RCON won't start | Empty `Password` under `[RCON]` | Set a password; empty disables RCON by design |

## 🤝 Contributing

PRs welcome — especially offset updates for new game builds and endpoint tests.
Keep the discovery layer validated: every new field read must go through
`Mem::Readable`/`Mem::Read` checks, and native calls must be marshalled through
`GameThread::RunSync`.

Upstream: [dkoz/RSDWServerAPI](https://github.com/dkoz/RSDWServerAPI) ·
Offsets source: RSDWSDK (`Dumpspace/OffsetsInfo.json`, `CppSDK`).

## 📄 License

[MIT](LICENSE) — © 2026 Kozejin and contributors.
