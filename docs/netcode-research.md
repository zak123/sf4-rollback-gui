# Netcode research: sync integrity and disconnect robustness

Researched 2026-07-19 against our tree, upstream sf4e, the vendored GGPO
fork (stock 2019 GGPO + build packaging), and Confetti3's fork. Nothing
here is implemented yet; see the priority list at the end.

## How sync works today

GGPO exchanges **inputs only** (two DWORDs per side per frame,
prediction window 8 frames). Everything else must be *derived
identically* on both machines — the determinism contract. Rollback
savestates piggyback the game's training-mode memento system plus
sf4e's `AdditionalMemento`s (task cores, HUD tasks, sprite actions,
battle-flow statics, sound-player shadow state; pool = prediction+2
slots). Desync *detection* is a 1 Hz exchange of a small two-character
snapshot (positions, meters) over the session channel, compared by
memcmp.

Cross-build divergence is already solved by the sidecar hash gate, and
sound-state divergence is mitigated (`bUsePureSounds` defaults on).

## Where desyncs come from, ranked

1. **Uncaptured state** — any value the simulation reads that isn't in
   a savestate silently diverges after a rollback. One instance is
   already known upstream (issue #3: motion blur isn't saved/loaded);
   the open question is what else. This is the class the tooling below
   is designed to hunt.
2. **Frame-pacing interference** — upstream issue #13: the game's
   "Smooth" frame-rate option interferes with the simulation rate.
   Confetti3 confirms it in the field; their guidance is simply "turn
   Smooth off." VSync settings can also affect rollback feel.
3. **Sound state** — mitigated, but the shadow-manager approach means
   new sound paths could still drift; low priority.

## Tooling gaps found

- **GGPO's sync-test mode is unusable as wired.** GGPO ships
  `ggpo_start_synctest`, which replays every frame through a
  rollback+resim *locally* and flags divergence — the standard tool for
  finding uncaptured state, needing one machine and one game instance
  (no Steam double-launch problem, no opponent). It's inert here
  because our save callback reports `*checksum = 0` and the log
  callback is a no-op, so it has nothing to compare.
- **Detection is coarse.** The 1 Hz two-character snapshot misses
  projectiles, tasks, HUD, and RNG state, and only fires once per
  second — a desync can go unnoticed until it's visually obvious.
- **Detection response is harmful.** On mismatch the client pops a
  *blocking* `MessageBoxA` on the simulation thread. The local sim
  freezes; the opponent's side then sees a stalled peer and times out —
  one detected desync manufactures a second failure mode.

## Disconnect-bug audit

Fixed already: aggressive GGPO timeouts (now 3000/1500 ms), no
interrupted-connection pause (now sim-holds until resumed), pre-battle
and handshake hangs (watchdogs), "will likely crash" aborts (now clean
native leave), snapshot leak across games (upstream #9).

Still open:

- **TIMESYNC stalls in one gulp.** When GGPO reports us ahead, the
  handler does `Sleep(frames_ahead * 16ms)` inside the event callback —
  an abrupt freeze up to ~130 ms. Felt as a hitch locally, and a long
  enough stall reads as an unresponsive peer remotely. Standard
  practice is spreading the correction across many frames (~1 ms extra
  sleep per frame until paid off).
- **Savestate pool exhaustion is still a fatal message box** ("attach a
  debugger"). Should be a clean abort like the other failure paths.
- **The desync message box** (above) — same treatment.

## Priorities

| # | Work | Why | Needs live game? |
|---|------|-----|------------------|
| P1 | **Wire sync-test mode**: real state checksum (ex. Fletcher32 over the memento buffers at save time) + a `--synctest` launcher flag mapping to `ggpo_start_synctest` | The systematic uncaptured-state finder; runs solo on one machine in any offline mode | Build: no. Run: yes — but solo, no opponent needed |
| P2 | Replace the desync and pool-exhaustion message boxes with the clean-abort path | Stops one failure from manufacturing a second; unfreezes the sim thread | No |
| P3 | Reuse the P1 checksum in the periodic session exchange (full-state checksum at 1 Hz instead of the two-character sample) | Detection catches *any* divergence, not just character state | Validation only |
| P4 | Smooth out TIMESYNC pacing | Removes hitches; avoids stall-reads-as-hang | Validation only |
| P5 | Detect the "Smooth" frame-rate option in-game and warn in the overlay | Converts a known footgun (#13) into a visible warning | Yes — finding the option flag is live RE |
| P6 | Capture motion blur state (#3) | Closes the one *known* uncaptured value | Yes — live RE |

P1 is the force multiplier: once sync-test runs clean through training
mode and a few matches' worth of inputs, every remaining desync report
from the playtest points at *netcode*, not state capture — and P3 then
catches real-world divergence within a second with an exact frame
number.

## What requires the game executable

The binary itself is on disk and statically analyzable (that's how the
roster tables were extracted). What static work can't do is *observe
the simulation*: verifying determinism fixes, hunting the #13 pacing
interaction, and finding memory locations for P5/P6 all need the game
running — P1 solo-runnable by one person, the rest eventually validated
by real matches. Practically: code and tooling land without the game;
proof needs a controller in someone's hands.
