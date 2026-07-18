# SF4 Rollback GUI

**Modern online play for _Ultra Street Fighter IV_ on Steam: rollback
netcode, lobbies, and chat. No IP addresses, no port typing, no 2014
menus.**

![The lobby app: lobby browser, create-a-lobby form, and lounge chat](images/lobby-client.png)

## What is this?

USF4 shipped with delay-based netcode: every bit of network lag between
you and your opponent turns into input lag under your fingers. Modern
fighting games use **rollback** netcode instead, which keeps your inputs
instant and hides network hiccups by predicting and correcting the
opponent's actions. USF4 never got it — this project gives it one.

It builds on two pieces:

* **[sf4e](https://codeberg.org/adanducci/sf4e)** by **adanducci** does
  the genuinely hard part: it injects [GGPO](https://github.com/pond3r/ggpo)
  rollback into the Steam release of USF4 at runtime, by reverse
  engineering the game deeply enough to save and restore its entire
  battle state every frame. No game files are modified on disk.
* **This fork** wraps that in the experience you'd actually want, in the
  spirit of Fightcade: a desktop app with a lounge, chat, and a lobby
  browser, plus dedicated lobby servers. Playing someone is: join their
  lobby, pick your character, hit Ready — **the game launches itself
  straight into the match**.

When a match ends you're back at the game's menu, both players still
seated in the lobby; ready up again for infinite runbacks. Close the
game when you're done and the app puts you back in the lobby chat.

## Status

**Early playtest.** The lobby experience (chat, browsing, creating and
joining lobbies) and the launch-into-match flow are working; matches
that can't connect fail fast with a clear message instead of hanging.
What this playtest is for: exercising real internet matches between
real connections, and finding out how often peer-to-peer connections
succeed without any router setup.

Not here yet: rankings or match history (rematch freely — nothing is
tracked), spectating, automatic score detection, and a connection relay
for stubborn NATs.

## Join the playtest

You need:

* **Ultra Street Fighter IV on Steam** ([store page](https://store.steampowered.com/app/45760/)),
  installed. You must own the game — this project contains no game files.
* Windows 10 or later.
* A controller or stick (keyboard works too).

Steps:

1. Download the **client bundle** (`sf4-rollback-gui-client.zip`) from
   the [latest release](https://github.com/zak123/sf4-rollback-gui/releases/latest),
   and extract **all** of it into one folder — the DLLs must stay next
   to the exes.
2. Run `LobbyClient.exe`. Pick a name, enter the playtest server
   address you were given (currently `44.226.168.62:23450` — this can
   change between playtests), and connect.
3. Say hi in the lounge. Create a lobby or join one from the browser.
4. Pick your character (the lobby creator also picks the stage) and hit
   **Ready**. When both players are ready, USF4 launches on its own.
5. At the title screen, **press a button on the controller you want to
   play on** — that binds it. The match then starts by itself.
6. Rematch: after the match, both players ready up again in the window
   shown in-game. Done playing: close the game and you're back in the
   lobby app.

### If a match won't connect

Lobby chat always works, but the match itself is a direct
peer-to-peer connection between the two players. On most home routers
it connects on its own. If your matches keep failing after ~45 seconds
with a "could not reach the opponent" message, forward **UDP port
23457** on your router to your PC and try again. Two players on the
same home network usually can't play each other — grab someone outside
the house.

### Reporting problems

Tell us what happened, and attach:

* Logs from `%APPDATA%\sf4e\logs` (both `sf4e.log` and
  `LobbyClient.log`),
* a video clip if you can — it's the single most useful thing for
  netplay bugs.

## For developers

* [docs/building.md](docs/building.md) — building from source (upstream
  sf4e's instructions, plus this fork's targets).
* [docs/product-design.md](docs/product-design.md) — architecture,
  protocol, and roadmap for the lobby product.
* [docs/vps-playtest.md](docs/vps-playtest.md) — hosting your own lobby
  server.
* `scripts/local-test.bat` — one-click local server + two apps for
  development.

Contributions that touch the rollback core itself are usually better
aimed at [upstream sf4e](https://codeberg.org/adanducci/sf4e) — this
fork tries to stay a thin product layer over it.

## Credits and license

* **[adanducci](https://codeberg.org/adanducci)** — sf4e: the reverse
  engineering and rollback implementation this project exists on top of.
* [GGPO](https://github.com/pond3r/ggpo) (GroundStorm Studios),
  [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets)
  (Valve), [Dear ImGui](https://github.com/ocornut/imgui),
  [spdlog](https://github.com/gabime/spdlog),
  [nlohmann/json](https://github.com/nlohmann/json),
  [Detours](https://github.com/microsoft/Detours) (Microsoft),
  [ValveFileVDF](https://github.com/TinyTinni/ValveFileVDF).

This project is licensed under the MIT License — see
[LICENSE](LICENSE). Street Fighter, Street Fighter IV, and Ultra Street
Fighter IV are © CAPCOM. This project is not affiliated with or
endorsed by Capcom, and requires a legitimately owned copy of the game.
