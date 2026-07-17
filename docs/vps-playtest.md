# Running a public playtest server

How to put `Lobbyd` on a VPS so playtesters anywhere can chat, join
lobbies, and play. Everything except the match itself flows through the
server, so it works for any tester behind any NAT; the match itself is
peer-to-peer GGPO UDP (see "Match connectivity" below for what that
means in practice).

## What you need

- A **Windows** VPS. `Lobbyd` is tiny- 1 vCPU / 2 GB RAM is plenty for
  dozens of testers. Any provider offering Windows Server 2019/2022
  works (Azure, Vultr, OVH, Contabo, ...). A Linux port of Lobbyd is
  future work; today the server is Windows-only.
- The **server bundle** from CI (`sf4-rollback-gui-server.zip` from the
  latest Actions run): `Lobbyd.exe` plus its runtime DLLs and
  `sidecar-hash.txt`.
- One inbound **UDP** port (default 23450).

## Setup

1. Copy the server bundle to the VPS, ex. `C:\sf4e-lobbyd\`.
2. Open the port in Windows Firewall (admin prompt):

   ```
   netsh advfirewall firewall add rule name="sf4e-lobbyd" dir=in action=allow protocol=UDP localport=23450
   ```

   Also allow UDP 23450 in the provider's own firewall/security-group
   settings if it has one (Azure NSG, Vultr firewall, etc.).
3. Run it:

   ```
   Lobbyd.exe --port 23450 --no-default-lobby --identity <public-ip>:23450 --sidecar-hash <contents of sidecar-hash.txt>
   ```

   - `--identity` should be the address testers connect to; it's the
     routing name baked into connection and lobby IDs.
   - `--sidecar-hash` pins the exact build testers must run, so
     mismatched builds (which would desync) are rejected up front. Every
     client bundle from the same CI run carries the matching
     `Sidecar.dll`. **Leave it off while iterating quickly** and any
     build can connect- fine for small trusted groups, not for open
     tests.
   - `--max-peers` caps admissions (default 64).
4. Keep it running across reboots with a scheduled task (admin prompt):

   ```
   schtasks /create /tn sf4e-lobbyd /sc onstart /ru SYSTEM /tr "C:\sf4e-lobbyd\Lobbyd.exe --port 23450 --no-default-lobby --identity <public-ip>:23450 --sidecar-hash <hash>"
   ```

   (Or use [NSSM](https://nssm.cc/) to run it as a real service with
   restart-on-crash.)

## What playtesters do

1. Own USF4 on Steam, installed.
2. Unzip the **client bundle** (`sf4-rollback-gui-client.zip`) anywhere-
   all files must stay together in one folder.
3. Run `LobbyClient.exe`, enter a name and `<public-ip>:23450`, connect.
4. Chat, create/join a lobby, ready up. The game launches itself; press
   a button at the title screen to bind the controller, and the match
   starts on its own. When done, close the game- the app puts you back
   in the lobby.

## Match connectivity (read before blaming the code)

Lobby traffic all flows through the VPS and always works. The **match**
is direct peer-to-peer UDP between the two players. Both games fire
packets at each other's address simultaneously, which punches through
typical home routers with no setup. When it doesn't (strict/symmetric
NAT, CGNAT), the game gives up with a clear message after 45 seconds
instead of hanging, and both players are returned to their apps.

If a pair consistently can't connect:

- Each player forwards their GGPO UDP port (default **23457**) on their
  router to their PC, or
- One player with an open/forwarded port is usually enough in practice.

Two testers behind the *same* router likely can't play each other
(NAT hairpin); they should test against outside players.

Please collect: how often matches connect without any port forwarding,
and from what ISP/router setups the failures come. That data decides
how urgently the relay fallback (M5) is needed.

## Updating a build

1. New CI run produces new client+server bundles and a new sidecar hash.
2. Update the server's `--sidecar-hash` (edit the scheduled task) and
   restart `Lobbyd`.
3. Send testers the new client zip. Old builds are cleanly rejected
   with a version-mismatch message.
