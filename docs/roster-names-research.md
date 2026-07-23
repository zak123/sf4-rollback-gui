# Roster names research: ultras, costumes, colors

Research for replacing the LobbyClient's raw `InputScalar` byte fields
(Color / Costume / Ultra Combo) with named dropdowns. Findings below are
grounded in the game's own files on disk and cross-checked against the
Street Fighter Wiki; the two open encoding questions have a 3-minute
in-game verification path (§5).

**Status: §6 is implemented** (Roster tables + `DrawPickCombos` in the
app) on the expected §4 encodings. Until a live match confirms them,
treat name→byte mappings as provisional; the sidecar also carries a
native chara-select byte logger (`LogVsCharaSelectChanges`) as an
alternate verification path.

**UPDATE (2026-07-22): DLC costumes crash on battle load through the
netplay path.** First playtest with a non-default costume (Sakura,
costume 4 = the Vacation DLC costume) hard-crashed both machines during
load, after GGPO had synced. The netplay path bypasses the store's
entitlement check that the native character select performs, and a DLC
costume brought up that way faults the loader (the game's own SEH
swallows it- no WER, no dump, no log). The pick UI was therefore pulled
back to bounded numeric inputs capped to the base game: costume
`0..costumeCount-4` (excludes the always-last-three DLC slots
Vacation/Wild/Horror) and color `0..9` (drops DLC-only 13-22 and
stylized 11-12). Base-game alternates (Alt 1..3) are allowed but remain
unconfirmed through netplay- if they also crash, restrict further to
Default only.

## 1. How the bytes reach the game

The app fills `Dimps::GameEvents::VsMode::ConfirmedCharaConditions`
(charaID / costume / color / ultraCombo / ... / unc_edition) and the
sidecar memcpy's it verbatim into the game's native VsMode structure in
`fUserApp::_OnVsPreBattleTasksRegistered`. The game consumes it exactly
as if its own character select had written it — so the encodings are
the game's own, and the game's char-select *validation* is bypassed
(out-of-range bytes are never clamped for us; the app UI must only
offer valid values).

Costumes and colors are cosmetic only (identical hitboxes/skeletons —
they're tournament-legal), so a bad pick risks a client-side load
problem, not a desync.

## 2. Ground truth from the installed game files

Scanned the Steam install (`resource\battle\chara`, `dlc\*`,
`patch_*`). Costume model sets are `<CODE>_<NN>.obj.emo`, colors are
`<CODE>_<NN>_<MM>.col.emb`. Results, fully uniform across the cast:

- **44 characters, each with 6 or 7 costumes** (1-based file numbers).
  - **7 costumes** (25 chars — the SF4-console-original cast):
    RYU KEN CNL HND BLK ZGF GUL DSM BSN BLR SGT VEG AGL CHB RIC JHA
    BOS GKI GKN CMY FLN SKR ROS GEN DAN
  - **6 costumes** (19 chars — everyone added in Super/AE/Ultra):
    HWK DJY GUY CDY IBK MKT DDL ADN HKN JRI YUN YAN RYX GKX RLN ELN
    PSN HUG DCP
- **Every costume has color files 01–10.** No costume anywhere has
  11 or 12 as files.
- **The last three costume slots of every character** (and only those;
  132 = 44×3 exactly) additionally have color files **13–22**. These
  are the USF4-2014 DLC costumes.
- Every character has exactly `<CODE>.uc1.ema` + `<CODE>.uc2.ema`
  (two ultras each; stance/range variants share a slot).
- All costume/color files ship in the depot regardless of DLC
  purchase (entitlement is enforced by the char select we bypass), so
  both netplay sides always have the assets. One live sanity match
  with a DLC costume is still worth doing.

## 3. Names and how they match up

### Colors — no wiki needed

Per the SF Wiki costume page (matches the files perfectly): every
costume has **colors 1–10, plus colors 11 and 12 which are "stylized"
variants of color 1** (procedurally derived — that's why no 11/12
files exist), and the three USF4 costumes each add **colors 13–22**.

So color labels are just numbers plus an annotation, driven by two
per-costume facts we already have (always 12; 22 when the costume is
one of the last three slots):

    1..10, 11 (stylized), 12 (stylized) [, 13..22 on Vacation/Wild/Horror]

### Costumes — wave labels, not flavor names

Capcom never named individual costumes; they shipped in waves ("Ryu
Alternate 3", pack DLC). The SF Wiki tags each character's slots with
the wave, and slot order is identical for the whole cast:

| File slot | 7-costume chars | 6-costume chars |
|-----------|-----------------|-----------------|
| 01 | Default | Default |
| 02 | Alt 1 | Alt 1 |
| 03 | Alt 2 | Alt 2 |
| 04 | Alt 3 | **Vacation** |
| 05 | **Vacation** | **Wild** |
| 06 | **Wild** | **Horror** |
| 07 | **Horror** | — |

Caution: slot order is **not** release order (Horror released Oct 2014,
Wild Dec 2014, but Wild sits before Horror in every character's slots —
both the wiki tags and the 22-color file pattern confirm the table
above). The last-three-slots rule means the label table derives from
one number per character: the costume count.

### Ultras — per-character U1/U2 names

From the SF Wiki USF4 move lists (A–G and H–Z pages; order there is
consistently Ultra 1 then Ultra 2, spot-checked against a dozen
well-known characters). Indexed by our roster ID:

| ID | Character | Ultra 1 | Ultra 2 |
|----|-----------|---------|---------|
| 0 | Ryu | Metsu Hadoken | Metsu Shoryuken |
| 1 | Ken | Shinryuken | Guren Senpukyaku |
| 2 | Chun-Li | Hosenka | Kikosho |
| 3 | E. Honda | Ultimate Killer Head Ram | Orochi Breaker |
| 4 | Blanka | Lightning Cannonball | Shout of Earth |
| 5 | Zangief | Ultimate Atomic Buster | Siberian Blizzard |
| 6 | Guile | Flash Explosion | Sonic Hurricane |
| 7 | Dhalsim | Yoga Catastrophe | Yoga Shangri-La |
| 8 | M. Bison | Nightmare Booster | Psycho Punisher |
| 9 | Balrog | Violent Buffalo | Dirty Bull |
| 10 | Sagat | Tiger Destruction | Tiger Cannon |
| 11 | Vega | Bloody High Claw | Splendid Claw |
| 12 | C. Viper | Burst Time | Burning Dance |
| 13 | Rufus | Space Opera Symphony | Big Bang Typhoon |
| 14 | El Fuerte | El Fuerte Giga Buster | El Fuerte Ultra Spark |
| 15 | Abel | Soulless | Breathless |
| 16 | Seth | Tanden Stream | Tanden Typhoon |
| 17 | Gouki | Wrath of the Raging Demon | Demon Armageddon |
| 18 | Gouken | Shin Shoryuken | Denjin Hadoken |
| 19 | T. Hawk | Raging Typhoon | Raging Slash |
| 20 | Cammy | Gyro Drive Smasher | CQC |
| 21 | Fei Long | Rekkashingeki | Gekirinken |
| 22 | Dee Jay | Sobat Festival | Climax Beat |
| 23 | Sakura | Haru Ranman | Shinku Hadoken |
| 24 | Rose | Illusion Spark | Soul Satellite |
| 25 | Gen | Zetsuei / Ryukoha | Shitenketsu / Teiga |
| 26 | Dan | Shisso Buraiken | Haoh Gadoken |
| 27 | Guy | Bushin Goraisenpujin | Bushin Muso Renge |
| 28 | Cody | Final Destruction | Last Dread Dust |
| 29 | Ibuki | Yoroitoshi | Hashinsho |
| 30 | Makoto | Seichusen Godanzuki | Abare Tosanami |
| 31 | Dudley | Rolling Thunder | Corkscrew Cross |
| 32 | Adon | Jaguar Revolver | Jaguar Avalanche |
| 33 | Hakan | Oil Coaster | Oil Combination Hold |
| 34 | Juri | Feng Shui Engine | Kaisen Dankairaku |
| 35 | Yun | You Hou | Sorai Rengeki |
| 36 | Yang | Raishin Mahhaken | Tenshin Senkyutai |
| 37 | Evil Ryu | Metsu Hadoken | Messatsu-Goshoryu |
| 38 | Oni | Meido Gohado | Tenchi Sokaigen |
| 39 | Rolento | Patriot Sweeper | Take No Prisoners |
| 40 | Elena | Brave Dance | Healing |
| 41 | Poison | Love Storm | Poison Kiss |
| 42 | Hugo | Gigas Breaker | Megaton Press |
| 43 | Decapre | Psycho Stream | DCM |

Gen's ultras are stance pairs (Mantis / Crane); Oni's U1/U2 have
air/range variants (Messatsu-Gozanku, Messatsu-Gotenha). One slot each
regardless.

## 4. Expected byte encodings (to verify)

All evidence says the struct bytes are **0-based** versions of the
1-based file/UI numbers — every playtest ran costume=0/color=0 and
rendered as Default costume, color 1:

- `costume`: 0-based slot → file `_NN` = byte+1. Valid 0..5 or 0..6.
- `color`: 0-based → in-game color byte+1. Valid 0..11 everywhere
  (incl. the two stylized), 0..21 on the last three costumes. The one
  wrinkle to confirm: whether stylized 11/12 sit at bytes 10/11 and
  wild colors at 12..21 (direct mapping, expected), or the game skips
  bytes 10/11.
- `ultraCombo`: char-select menu order is U1 / U2 / Ultra Combo
  Double, so expect **0 = U1, 1 = U2, 2 = W (double)**. Not publicly
  documented anywhere; must verify.

## 5. Verification (3 minutes, offline, no netplay)

The overlay's **VS chara select** debug window already prints the live
bytes from the game's own memory (`Ultra combo: %d`, `Color`,
`Costume` via `VsCharaSelect::PlayerConditions`). So: launch the game,
enter local Versus, and at the native character select pick each ultra
option (U1, U2, W) and a few colors (10, 11-stylized, a wild 13+ on a
2014 costume) while watching the overlay window. That pins every
encoding, including the 11/12 wrinkle.

## 6. Proposed implementation

Extend `src/client/sf4e__Roster.hxx` (same extraction discipline as the
existing tables — game-ID order, never reorder):

```cpp
// Per roster ID. costumeCount is from the install file scan;
// the last three costumes are always Vacation/Wild/Horror (22 colors).
struct CharaExtra {
    const char* ultra1;
    const char* ultra2;
    uint8_t costumeCount;   // 6 or 7
};
static const CharaExtra charaExtra[NUM_CHARACTERS] = { ... };
```

UI (in the lobby panel's pick block):

- **Ultra**: Combo with 3 items: `U1: <name>` / `U2: <name>` /
  `W: both (reduced damage)`.
- **Costume**: Combo sized by `costumeCount`, labels
  `Default, Alt 1..N, Vacation, Wild, Horror`.
- **Color**: Combo sized by costume (12 or 22), labels `1..10,
  11 (stylized), 12 (stylized), 13..22`. Clamp when the costume
  changes so a wild color doesn't survive a switch to an old costume.

Persisted config keys (`ultra`, `costume`, `color`) keep their raw
byte meaning — no migration needed, just clamping on load.

## 7. Sources

- Install file scan (this machine, 2026-07-21): counts in §2.
- [List of moves in Ultra Street Fighter IV A-G](https://streetfighter.fandom.com/wiki/List_of_moves_in_Ultra_Street_Fighter_IV_A-G),
  [H-Z](https://streetfighter.fandom.com/wiki/List_of_moves_in_Ultra_Street_Fighter_IV_H-Z) — ultra names/order.
- [Alternate Costumes/Street Fighter IV series](https://streetfighter.fandom.com/wiki/Alternate_Costumes/Street_Fighter_IV_series) — costume waves per slot, 10+2 color rule, 20-color packs.
- [EventHubs: Summer Vacation costumes](https://www.eventhubs.com/news/2014/jul/28/images-all-new-ultra-street-fighter-4-costumes-revealed-summer-vacation-dlc-outfits/) — 2014 pack naming/dates.
