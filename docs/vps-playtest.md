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
3. Create the config files the bundle's launch wrapper reads (in
   `C:\sf4e-lobbyd`):

   ```powershell
   Set-Content C:\sf4e-lobbyd\identity.txt "<public-ip>:23450" -NoNewline -Encoding ascii
   ```

   - `identity.txt` is the address testers connect to; it's the routing
     name baked into connection and lobby IDs, and it survives updates.
   - The bundled `sidecar-hash.txt` makes the wrapper pin the exact
     game build testers must run, so mismatched builds (which would
     desync) are rejected up front. Every client bundle from the same
     CI run carries the matching `Sidecar.dll`. The pin only applies to
     game connections- the lobby app carries no sidecar and is always
     admitted. Delete the file to accept any game build- fine for small
     trusted groups, not for open tests.
   - While the repo is **private**, also give the self-updater a token:
     create a fine-grained PAT (this repo only, Contents: read-only) and
     `Set-Content C:\sf4e-lobbyd\github-token.txt "<PAT>" -NoNewline`.
     Delete the file once the repo is public.
   - Optionally, have successful updates announced to a Discord channel:
     `Set-Content C:\sf4e-lobbyd\discord-webhook.txt "<webhook url>" -NoNewline`.
     The post names the new version and links the release so testers
     know to grab the matching client. Keep webhook URLs out of the
     repo- anyone holding one can post to the channel.
4. Register the server and the self-updater as scheduled tasks (admin
   prompt), and start the server:

   ```
   schtasks /create /tn sf4e-lobbyd /sc onstart /ru SYSTEM /tr "C:\sf4e-lobbyd\run-lobbyd.bat"
   schtasks /create /tn sf4e-updater /sc minute /mo 10 /ru SYSTEM /tr "powershell -ExecutionPolicy Bypass -File C:\sf4e-lobbyd\vps-update.ps1"
   schtasks /run /tn sf4e-lobbyd
   ```

   The updater checks the repo's **latest GitHub release** every 10
   minutes; when a new one appears it downloads the server bundle,
   stops the server, swaps the files (including the hash pin, the
   wrapper, and itself), and restarts.

## Cutting a release

1. Build the ref you want released: `git push origin main:build` (or
   the Actions "Run workflow" button). CI builds it, runs the smoke
   test, and uploads the two bundles as run artifacts.
2. Download both artifacts from the run and create a GitHub release,
   attaching both zips. **Keep the exact filenames**
   (`sf4-rollback-gui-server.zip`, `sf4-rollback-gui-client.zip`)- the
   VPS updater matches the server asset by name, and both zips must
   come from the **same run** so the client matches the server's hash
   pin.
3. Within ~10 minutes the VPS self-updates to the release. Testers grab
   the client zip from the same release- the fresh hash pin locks older
   builds out (that's the point).

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
