# Code signing and Windows warnings

Unsigned, low-prevalence downloads trip two separate Windows systems:

- **SmartScreen** blocks first runs of downloaded executables with no
  reputation ("Windows protected your PC").
- **Defender's ML heuristics** flag the *behavior*: Launcher.exe spawns
  SSFIV.exe suspended and injects Sidecar.dll, which is what malware
  does too. The sibling fork (Confetti3's SF4-Netplay-Launcher, same
  Detours technique) gets detected as `Wacapew.A!ml`.

Fixes, in order of impact:

## 1. SignPath Foundation (free code signing for OSS) — IN PROGRESS

SignPath signs open-source releases at no cost, and has already
approved Confetti3's SF4 launcher — the same kind of binary. Their
criteria: public repo, OSS license, active maintenance, signing done
in CI. We meet all of them.

**Application (project owner does this once):**

1. Go to <https://signpath.org/apply> and apply with:
   - Project: `sf4-rollback-gui`
   - Repository: `https://github.com/zak123/sf4-rollback-gui`
   - License: MIT
   - Description: "Rollback-netcode online play for Ultra Street
     Fighter IV, with a lobby/matchmaking app and dedicated server.
     Fork of adanducci's sf4e."
   - CI: GitHub Actions (`.github/workflows/build.yml`)
   - Policy file: `.signpath/signpath.json` (in this repo)
2. Once approved: install the SignPath GitHub App on the repo, create
   the project with slug `sf4-rollback-gui`, enable a signing policy
   named `release`.
3. Add four repo secrets: `SIGNPATH_API_TOKEN` (submitter role),
   `SIGNPATH_ORGANIZATION_ID`, `SIGNPATH_PROJECT_SLUG`,
   `SIGNPATH_SIGNING_POLICY_SLUG` (`release`).
4. Then wire the signing step into CI (below).

**CI integration — ORDER MATTERS.** The workflow pins
`sidecar-hash.txt` to Sidecar.dll's exact bytes, and signing changes
the bytes. The step order must be:

    build -> sign (Launcher.exe, Sidecar.dll, LobbyClient.exe,
    Lobbyd.exe) -> compute sidecar hash -> package zips

Signing after the hash step would make the server reject every signed
client. Use `signpath/github-action-submit-signing-request` with the
secrets above.

**Expectations:** signing removes "Unknown publisher" immediately;
SmartScreen reputation still ramps over the first releases. Defender
can still occasionally flag the injection pattern — keep filing false
positives (below); they stick better for signed binaries.

## 2. Defender false-positive submissions (per release, ~5 minutes)

After cutting a release, submit the binaries to Microsoft:

1. <https://www.microsoft.com/en-us/wdsi/filesubmission>
2. "Submit a file" as **Software developer**, sign in with any
   Microsoft account.
3. Upload `sf4-rollback-gui-client.zip` from the release.
4. Select "Incorrectly detected as malware", and name the detection if
   Defender showed one (Confetti3's build trips `Wacapew.A!ml`).
5. Link the release page and note it is an open-source game mod built
   in public by GitHub Actions.

Microsoft usually clears these within 1–3 days.

## 3. Already done

- Version resources (publisher, description, version) are stamped on
  all four binaries via `src/version.rc.in` — metadata-less
  executables score worse with heuristics. Bump the `project(VERSION)`
  in CMakeLists.txt alongside releases.
- README tells playtesters what the SmartScreen wall looks like and
  how to get past it.

## Not worth it (for now)

- **OV certificates** (~$70–400/yr): hardware-token/cloud-HSM
  requirements since 2023 complicate CI signing; SignPath is free.
- **EV certificates**: expensive, generally need a registered
  business, and no longer guarantee instant SmartScreen reputation.
- **Azure Trusted Signing** (~$10/mo): the strongest paid option
  (Microsoft-validated identity, fast reputation) — revisit if the
  SignPath application falls through.
