# 🐉 RSDWServerAPI

[![Licence : MIT](https://img.shields.io/badge/licence-MIT-green.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-0.1.6-blue.svg)](#-compatibilité)
[![Jeu](https://img.shields.io/badge/jeu-RuneScape%3A%20Dragonwilds%201.0.1-orange.svg)](#-compatibilité)
[![Moteur](https://img.shields.io/badge/moteur-UE%205.6.1-purple.svg)](#-compatibilité)

> 🇬🇧 [English version](README.md)

API d'administration pour les serveurs dédiés **RuneScape: Dragonwilds** sous Linux.

Elle injecte une API REST, un écouteur Source RCON, des commandes admin en jeu et un
relais Discord optionnel dans un serveur en cours d'exécution — pour voir qui est
connecté, lire leurs statistiques en direct, les téléporter, les soigner, les expulser
ou les bannir à distance : depuis un panneau, un script, un bot Discord ou tout ce qui
parle HTTP.

> 💡 Ce fork porte les correctifs durcis en production et les offsets du build 1.0.1
> (branche `fix/engine-ready-listeners-and-header-case`, voir la
> [PR #3](https://github.com/dkoz/RSDWServerAPI/pull/3) en amont).

---

## 📦 Compatibilité

Vérifié en production sur un serveur dédié Linux (Ubuntu, systemd + `LD_PRELOAD`).

| Version du jeu | Build (CL) | Build Steam | Dépôt Linux | État |
|---|---|---|---|---|
| 1.0.0.x | 247725 (`++dominion+hotfix`) | — | — | ✅ vérifié (offsets SDK) |
| **1.0.1** « Eye on Ashenfall » | **248854** (`++dominion+staging`) | **25811348** | **3501791** | ✅ vérifié (offsets mis à jour) |

- Moteur : **UE 5.6.1** — inchangé entre les deux builds ci-dessus.
- Plateforme : serveur dédié Linux (`RSDragonwildsServer-Linux-Shipping`, base d'image
  `0x200000`, binaire strippé : pas de `.symtab`, `UObject::ProcessEvent` non exporté).

## ✨ Fonctionnalités

- 📡 **Données joueurs en direct** — noms, GUID de personnage, plateforme, ping,
  position, rotation, vélocité, santé, faim, soif, chaque compétence avec son XP,
  chaque attribut flottant.
- 🚪 **Expulsion à distance** via la fonction de kick du jeu, persistante jusqu'au
  redémarrage.
- 📢 **Diffusion** et 💬 **message privé** via le chat du jeu lui-même.
- 🎁 **Don d'objets** via la fonction de don du jeu.
- 🔨 **Bannissements** qui survivent aux redémarrages, joueurs hors-ligne bannissables
  par SteamID64.
- 🌀 **Téléportation** — joueur vers joueur, ou vers des coordonnées — via la native
  `K2_TeleportTo` du jeu sur le game thread (acquittée par le moteur, répliquée).
- 💚 **Actions d'admin** — soin (native `IncreaseHealth`), nourrir, hydrater, god mode.
- 🎮 **Commandes admin en jeu** — `/kick`, `/ban`, `/unban`, `/give`, contrôlées par la
  liste owner/admin du jeu.
- 📣 **Relais Discord** — chat du jeu et morts des joueurs postés sur des webhooks.
- 🔌 **Deux écouteurs indépendants** — REST et Source RCON, activables séparément.
- 🔐 **Contrôle d'accès sur les deux** — bearer token ou mot de passe RCON, whitelist
  d'IP, limitation de débit par adresse et blocage automatique après échecs répétés.
- 🛡️ **Offsets validés à l'exécution** — chaque étape de découverte est vérifiée et
  contre-vérifiée avant toute lecture ; après un patch du jeu, une incohérence se
  dégrade en `503` + une ligne de log plutôt que de servir n'importe quoi.

## 🚀 Démarrage rapide

### Compilation

Docker est la seule exigence. Le conteneur de build est Debian bookworm, aligné sur
l'image du serveur dédié : le résultat ne demande jamais une glibc plus récente que
celle de l'hôte. `libstdc++` et `libgcc` sont liées statiquement.

```bash
./build.sh
```

Sortie : `dist/librsdwapi.so`.

Un simple `make` fonctionne aussi avec une toolchain locale ; sur les hôtes sans
`libstdc++` statique (Fedora, …), retirez les drapeaux statiques :

```bash
make LDFLAGS="-shared -pthread -ldl"
```

### Installation

Copiez `dist/librsdwapi.so` à la racine du serveur et préfixez la commande de
démarrage :

```bash
LD_PRELOAD=./librsdwapi.so ./RSDragonwilds/Binaries/Linux/RSDragonwildsServer-Linux-Shipping -log -port=7777
```

### systemd (montage de production)

Le drop-in ci-dessous est celui qui tourne en production : `LD_PRELOAD` est appliqué à
tout le service (le script lanceur *et* le binaire du jeu sont injectés — sans danger,
voir [🏗️ Fonctionnement](#-fonctionnement)), et le répertoire de données est fixé avec
`RSDWAPI_DIR` :

```ini
# /etc/systemd/system/dragonwilds.service.d/rsdwapi.conf
[Service]
Environment=RSDWAPI_DIR=/home/steam/dragonwilds_linux_server/rsdwapi
Environment=LD_PRELOAD=/home/steam/dragonwilds_linux_server/librsdwapi.so
```

Sur Pterodactyl ou Pelican, placez le `.so` à la racine du serveur et réglez le
démarrage ainsi :

```
LD_PRELOAD=/home/container/librsdwapi.so ./RSDragonwilds/Binaries/Linux/RSDragonwildsServer-Linux-Shipping -log -port={{SERVER_PORT}} -ini:Game:[/Script/Engine.GameSession]:MaxPlayers={{MAX_PLAYERS}}
```

Allouez le port API (et le port RCON si vous l'activez) dans le panneau.

## ⚙️ Configuration

Au premier démarrage, le mod écrit `rsdwapi/settings.ini` à côté du binaire du serveur
(`RSDragonwilds/Binaries/Linux/rsdwapi/` sur une installation standard). Utilisez
`RSDWAPI_DIR` pour le placer ailleurs. Le fichier est relu au démarrage du serveur.

Un bearer token et un mot de passe RCON sont générés à ce premier démarrage et ne sont
jamais régénérés ensuite — une valeur vide reste vide.

**L'API REST est activée par défaut. RCON est désactivé par défaut.**

```ini
[General]
EnableLogging=true        # étapes de découverte + chaque requête
VerboseLogging=true       # détail des scans ; laisser sauf si trop verbeux

[API]
Enabled=true
BindAddress=0.0.0.0       # 🔒 préférer 127.0.0.1 + un token en production
Port=8080
BearerToken=              # vide = aucune authentification (déconseillé)
IPWhitelist=              # IPs / CIDR séparés par des virgules ; vide = tout le monde

[RCON]
Enabled=false             # Source RCON, désactivé par défaut
BindAddress=0.0.0.0
Port=27020
Password=                 # vide = RCON ne démarre pas, par conception
IPWhitelist=
CommandsPerMinute=60
CommandBurst=15
MaxFailedAuth=5
FailWindowSeconds=60
BanSeconds=300
MaxConnections=16

[Discord]
Enabled=false
WebhookUrl=               # relais du chat
DeathWebhookUrl=          # morts ; retombe sur WebhookUrl si vide
Username=Dragonwilds
```

## 🌐 API REST

Tous les endpoints vivent sous `/api/`. Quand un bearer token est configuré, envoyez
`Authorization: Bearer <token>` (les noms d'en-têtes sont insensibles à la casse —
reqwest/hyper les mettent en minuscules et c'est géré).

- `200` avec `{"status":"ok","message":"…"}` en cas de succès.
- `401` token manquant/incorrect · `404` route inconnue · `503` moteur pas prêt
  (`{"error":"engine not initialised"}`).

### Lectures

| Route | Description |
|---|---|
| `GET /api/hello` | Ping de vie. |
| `GET /api/health` | `{"status","version","uptimeSeconds","engineReady"}`. |
| `GET /api/players` | Joueurs connectés, détail complet. Filtres de requête : `name`, `netId`, `characterName`, `characterGuid`. |
| `GET /api/items` | `{"items":[{"name","maxStackSize"}]}` — tous les objets `ItemData`. |
| `GET /api/bans` | `{"bans":[{"netId","name","reason","bannedAt"}]}` — bannissements persistants. |
| `GET /api/status` | Identité du serveur : `serverName`, `worldName`, `ownerId`, `serverGuid`, `isDedicatedServer`, `crossplayEnabled`, état du monde. |

Entrée de `/api/players` (tous les champs sont optionnels — toutes les plateformes
n'exposent pas tout) :

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

Les identifiants de joueur sont comparés à `playerId`, `uniqueNetId`, `characterGuid`,
`characterName` et `name` — la même chaîne marche partout.

| Route | Corps | Notes |
|---|---|---|
| `POST /api/kick` | `{"player","reason"?}` | Kick natif du jeu, ré-appliqué tant que le serveur tourne. |
| `POST /api/broadcast` | `{"message","sender"?}` | Chat natif du jeu, à tout le monde. |
| `POST /api/tell` | `{"player","message"}` | Message privé ; nécessite la dérivation de ProcessEvent (voir plus bas). |
| `POST /api/give` | `{"player","item","count"?}` | `count` vaut 1 par défaut. |
| `POST /api/ban` | `{"player","reason"?}` | Persistant ; hors-ligne par SteamID64. |
| `POST /api/unban` | `{"player"}` | |
| `POST /api/teleport` | `{"from","to"}` | Téléporte `from` sur la position de `to`. |
| `POST /api/tpto` | `{"player","x","y","z"}` | Coordonnées monde absolues (doubles). |
| `POST /api/heal` | `{"player","amount"?}` | Soin complet par défaut ; `amount` soigne partiellement, borné au max. |
| `POST /api/feed` | `{"player"}` | Faim → maximum. |
| `POST /api/hydrate` | `{"player"}` | Soif → maximum. |
| `POST /api/godmode` | `{"player","enabled"?}` | Bascule `bCanDie` ; `true` par défaut. |

```bash
curl -H "Authorization: Bearer $TOKEN" -X POST \
  -d '{"from":"ApachaiZ","to":"Seiylvana"}' http://127.0.0.1:8080/api/teleport
# → {"status":"ok","message":"teleported ApachaiZ to Seiylvana"}
```

## 🎮 Commandes chat en jeu

Pour les joueurs que le jeu considère owner ou admin (`PrivilegeFunctionLibrary`) :

```
/kick <joueur> [raison]
/ban <joueur> [raison]
/unban <joueur ou SteamID64>
/give <joueur> <objet> [quantité]
```

> ⚠️ La vérification admin se résout paresseusement à la première utilisation : les
> classes de game features s'enregistrent après le démarrage du mod, un lookup unique
> peut donc les manquer. C'est géré dans le code — le lookup retente à la première
> commande au lieu de condamner toute la session.

## 💬 RCON

RCON Source sur le port configuré (désactivé par défaut). Commandes :

```
help                      status
players [json]            player <nom>
playercount
kick <joueur> [raison]    ban <joueur> [raison]     unban <joueur|id>
broadcast <message>       tell <joueur> <message>
give <joueur> <objet> [quantité]
tp <joueur> <cible>       tpto <joueur> <x> <y> <z>
heal <joueur>             feed <joueur>             hydrate <joueur>
godmode <joueur> <on|off>
```

## 🏗️ Fonctionnement

Le mod est une bibliothèque partagée injectée via `LD_PRELOAD`. Son constructeur lance
un thread d'initialisation qui attend que le moteur du jeu démarre, puis attache les
lecteurs et les hooks. **Rien n'est lu ni appelé avant que le moteur soit validé.**

### 🔍 Découverte (pourquoi les patches ne le font pas crasher)

Les globales UE sont localisées par offsets SDK **et** par scans de secours, chacun
validé :

1. `GNames` (le pool de FName) — offset SDK d'abord ; en cas d'échec, chaque segment
   inscriptible est scanné pour un pool dont la première entrée décode en `None`. Les
   noms se résolvent depuis ce pool.
2. `GObjects` (le `TUObjectArray`) — offset SDK validé contre la structure de chunks ;
   sinon scan des segments inscriptibles. Les objets sont parcourus par chunk.
3. `GWorld` — offset SDK validé (classe `World` + niveau persistant), sinon balayage de
   `GObjects` pour trouver le `World` vivant.

Les noms de classes, les recherches de fonctions et chaque lecture de champ passent par
des pointeurs validés avec des contrôles de lisibilité — un champ déplacé se dégrade en
donnée manquante, pas en crash.

Offsets actuels (build 1.0.1 / CL 248854, base d'image `0x200000`) :

| Globale | Relatif | Absolu |
|---|---|---|
| `GNames` | `0x0DBDAFF8` | `0xDDDAFF8` |
| `GObjects` | `0x0DC96510` | `0xDE96510` |
| `GWorld` | `0x0DD525F8` | `0xDF525F8` |

### 🧵 Marshalling vers le game thread

Les fonctions natives UE doivent s'exécuter sur le **game thread**. Le mod ne les
appelle jamais depuis son thread HTTP :

- Un pump est installé sur une RPC serveur native (`PlayerController.ServerUpdateCamera`) ;
  chaque fois qu'un joueur la déclenche, le travail en file (`GameThread::RunSync`) est
  drainé sur le game thread. C'est ainsi que téléport, soin, dons et kicks s'exécutent
  nativement.
- `UObject::ProcessEvent` n'est pas exporté sur les builds Linux Shipping : le mod
  dérive son adresse à l'exécution (marche de pile + slot de vtable partagé) ; tant que
  la dérivation n'a pas eu lieu, les fonctionnalités adossées à ProcessEvent (`tell`,
  `broadcast`) restent désactivées avec un log explicite.

### 🧩 Appels natifs et pool d'objets

`FindFunction(className, functionName)` balaie le pool d'objets vivant et compare par
nom — c'est pourquoi les mises à jour qui **renomment** des UFunctions comptent plus
que celles qui déplacent des offsets. Sur 1.0.1, `AActor::TeleportTo` a disparu du
pool ; le mod retombe sur `K2_TeleportTo` (le wrapper BlueprintCallable) avec l'offset
de retour correspondant.

## 🔧 Adapter à un nouveau build du jeu

Quand le jeu patche, attendez-vous à un ou plusieurs de ces cas — par ordre de
probabilité :

1. **Globales déplacées** (GNames/GObjects/GWorld). Le scan de segments trouve les
   nouvelles valeurs ; lancez le serveur une fois, lisez le log
   (`GNames at … (segment scan)`) et mettez à jour `Offsets::G*` dans
   `src/engine/dom_engine.h`. Les anciennes valeurs restent en commentaire.
2. **Champs de structures déplacés** (offsets de membres de classes dans
   `dom_engine.h`). Symptôme : valeurs plausibles mais fausses, ou champs vides dans
   `/api/players`. Re-dumpez avec le SDK (RSDWSDK/Dumpspace) ou bissectez avec une
   sonde en lecture seule sur `/proc/<pid>/mem`.
3. **UFunctions renommées/supprimées** (ex. `TeleportTo` → `K2_TeleportTo`). Symptôme :
   lignes `… not found` dans les logs + passage par un chemin de repli. Correctif :
   résoudre le nouveau nom dans le pool.
4. **Classes enregistrées tardivement** (game features). Symptôme : un lookup marche
   sur certaines runs et pas d'autres. Correctif : résolution paresseuse à la première
   utilisation, jamais une seule fois au boot.
5. **Mise à niveau du moteur** (UE mineure/majeure). Tout peut bouger ; re-dumpez le SDK.

La boucle la plus sûre : déployer → surveiller le log de démarrage (chaque étape de
découverte est loggée) → vérifier `/api/health` + `/api/players` + un endpoint
d'action → seulement ensuite faire confiance.

## 🔒 Sécurité

- 🔐 Liez `127.0.0.1` et configurez un bearer token en production ; gardez
  `IPWhitelist` pour les montages multi-hôtes. L'API est sinon sans authentification,
  par conception.
- 📵 RCON reste désactivé sauf besoin ; son mot de passe est généré une fois.
- 🚦 Les échecs de login RCON sont limités en débit et auto-bloqués par adresse.

## 🩺 Dépannage

| Symptôme | Cause | Correctif |
|---|---|---|
| `/api/health` répond `engineReady:false` pour toujours | Le script lanceur (aussi injecté) a pris le port REST en premier | Corrigé : les listeners ne se lient que dans le processus où le moteur s'est initialisé ; mettez le mod à jour |
| Les clients (reqwest/hyper, bots) reçoivent `401` avec le bon token ; `curl` marche | Les noms d'en-têtes arrivent en minuscules (`authorization`) ; lookup sensible à la casse | Corrigé : noms mis en minuscules au parse (RFC 9110) ; mettez le mod à jour |
| `chat commands unavailable` / lookups ratés sur certaines runs | Les classes de game features s'enregistrent après le boot | Corrigé : re-résolution paresseuse à la première utilisation |
| Le soin/téléport « réussit » mais le joueur ne voit rien | Les écritures mémoire directes contournent la réplication UE (pas de dirty mark) | Corrigé : fonctions natives sur le game thread (`IncreaseHealth`, `K2_TeleportTo`) |
| Tout répond `503` | La découverte du moteur a échoué | Regardez le log de démarrage — une mise à jour a probablement déplacé les globales ; voir [🔧 Adapter](#-adapter-à-un-nouveau-build-du-jeu) |
| RCON ne démarre pas | `Password` vide sous `[RCON]` | Mettez un mot de passe ; vide = RCON désactivé par conception |

## 🤝 Contribuer

PR bienvenues — en particulier les mises à jour d'offsets pour les nouveaux builds et
les tests d'endpoints. Gardez la couche de découverte validée : chaque nouvelle lecture
de champ doit passer par `Mem::Readable`/`Mem::Read`, et les appels natifs doivent être
marshallés via `GameThread::RunSync`.

En amont : [dkoz/RSDWServerAPI](https://github.com/dkoz/RSDWServerAPI) ·
Source des offsets : RSDWSDK (`Dumpspace/OffsetsInfo.json`, `CppSDK`).

## 📄 Licence

[MIT](LICENSE) — © 2026 Kozejin et contributeurs.
