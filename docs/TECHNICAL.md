# WASD technical notes

How WASD reads each value from Dark Souls III, how every number was
verified, and how to build and test it. For what WASD is and how to
install it, see the [README](../README.md).

## Where it started: Milestone 1 proof of concept

Read-only console tool proving the core technical loop of the original brief:
reliably read a live value out of a running Dark Souls III (Seamless
Co-op) process, with real latency/rate numbers, no write access ever.

Long-term goal is a live coaching overlay. This is not that — it's the
foundation the brief asked to prove first: a real, verified,
restart-surviving read of one value (player HP).

## Hard safety constraints (do not relax)

- **Read-only, always.** The only access rights ever requested from
  `OpenProcess` are `PROCESS_VM_READ | PROCESS_QUERY_INFORMATION`.
  `PROCESS_VM_WRITE` and `WriteProcessMemory` do not appear anywhere in
  `src/main.cpp` — grep it yourself, it's one file.
- **Seamless Co-op only.** Vanilla online DS3 runs under the exact same
  `DarkSoulsIII.exe` process name, so before opening any handle the tool
  requires that a Seamless Co-op module (`ds3sc.dll`) already be loaded
  in the target process, and refuses to attach otherwise. This is a
  best-effort guard, not a guarantee — never rely on it alone. If
  Seamless Co-op ever renames its DLL, run `probe --list-modules` to see
  what's actually loaded and update `kSeamlessCoopModuleMarker` in
  `src/main.cpp`.

## Licence and credits

WASD is free software under the **GNU GPL, version 3 or later**
(`LICENSE`). Facts about the game documented by other projects (memory
layouts, byte patterns, file formats, flag ids, formulas) are credited,
with their licences, in `data/THIRD_PARTY_NOTICES.md`. Dark Souls III
and its data belong to FromSoftware and Bandai Namco; WASD ships none of
the game's data.

## Building

Requires MSVC (Visual Studio's C++ tools). From a normal PowerShell
prompt:

```powershell
.\build.ps1
```

`build\` isn't in git (2026-10-07): build after cloning or pulling.
Produces two exes from one compile of `src\main.cpp`, version **0.1.0**:

- **`build\WASD.exe`**, the app: double-click it. A window with its own taskbar button
  (status, map data, live page, overlay on/off, spoiler tier, results page, data folder,
  Quit) plus the overlay and the live page. No console; it logs to
  `logs\wasd-YYYYMMDD.log` in your data folder (7 days kept). One copy at a time: starting
  it again brings the running window to the front. Closing the window quits.
- **`build\wasd-cli.exe`**, the command-line tool (every command below; `--version`).

`.\build.ps1 -Release` builds it optimised, for the installer. Both link
the C++ runtime statically, so no Visual C++ Redistributable is needed.
The exe carries its version info, icon and an `asInvoker` manifest from
`src/app.rc`; the version and publisher (AmishGoose) are defined once in
`src/version.h`. The script locates `vcvars64.bat`
under the VS2026 Community install path baked into `build.ps1` — update
that path if your VS install differs.

## Installer

`installer\wasd.iss` (Inno Setup 6: `winget install JRSoftware.InnoSetup`)
builds `dist\WASD-<version>-setup.exe` from a release build:

```powershell
.\build.ps1 -Release; .\tools\fetch_python.ps1
& "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" installer\wasd.iss
```

It installs per user (no admin) into `%LOCALAPPDATA%\Programs\WASD`: `bin\`
(both exes), `data\`, `templates\`, the bundled `python\` and `tools\`, and a
`README.txt`. A Start menu shortcut "WASD" carries the app's taskbar identity
(`AmishGoose.WASD`), so a pinned shortcut and the running window share one
button; a desktop shortcut is optional. Settings -> Apps lists it as
"WASD: World Awareness & State Display" with version, publisher, icon and
size. Installing a newer version upgrades in place. Setup and uninstall ask
for WASD to be closed first. Uninstall asks whether to delete your data
(`%LOCALAPPDATA%\WASD`, default No); a silent uninstall keeps it unless
`/REMOVEUSERDATA=1`. The name, version and publisher are read from the
built `WASD.exe`. `/DTestBuild` makes a separate test identity for the
tests.

## Making a release

```powershell
.\release.ps1              # -> dist\WASD-<version>-setup.exe and its .sha256
.\release.ps1 -CheckOnly   # just: is the tree clean, is the version untagged?
```

It stops unless every change is committed (so the setup file matches a
commit) and `v<version>` isn't tagged yet (one file per version; the
version is `WASD_VERSION_STRING` in `src\version.h`). Then it builds
optimised into `build\release` (leaving the dev exes in `build\` alone),
fetches the bundled Python, runs every test suite -- the default run,
`-Only pkg` with the exe checks and the full installer cycle on the
release binaries, and `-Only realdata` -- builds the installer, writes
and re-checks its SHA-256 file, and prints the `git tag` / `git push`
commands. It never tags or pushes by itself: tag once the setup has been
checked on a second PC. A release takes a few minutes, mostly
tests.

## Where WASD keeps its files

Two places (`AppPaths` in `src/main.cpp`):

- **Program files** (read-only): `data\*.tsv` and `templates\`, found in
  the folder above the exe (`build\` here, `bin\` in an install).
- **Your data** (read-write): `settings.ini`, `sessions\`, `reports\`,
  `scratch\` (scan and pointer-search results), and later `logs\`.
  - **In this repo** (a git checkout with `src\main.cpp`): the repo
    itself, so everything stays where it always was. Generated map data
    stays in `data\generated\`, where `tools\extract_treasures.py`
    writes it.
  - **Installed:** `%LOCALAPPDATA%\WASD\`, which an upgrade or uninstall of
    the program never touches; map data goes in `generated\` there.
  - `WASD_USER_DIR` overrides both (the tests use it).

**Map data** (`treasures.tsv`, `enemies.tsv`: item positions and boss
placements; `item_names.tsv`, `bonfire_names.tsv`, `place_names.tsv`: the
game's own English names, from its text and regulation files;
`regions.tsv`: each play region labelled with its area and nearest
bonfire; `boss_names.tsv`: each boss's name as on its health bar; all
read from your own DS3 install, never shipped): `wasd-cli.exe extract
[--game <folder>]` runs `tools\extract_treasures.py` with the system
Python here, or the bundled Python (`python\`) in an install, and
`overlay` does it by itself before starting when a table is missing or
older than the game's `Data5.bdt`, `Data1.bdt` or `Data0.bdt`. Names
appear once it's done, without a restart. It finds the game folder from
`--game`, the running game (only with Seamless Co-op loaded), `game_dir`
in `settings.ini` (saved after the first find), or Steam's library list.
`tools\fetch_python.ps1` fetches and trims the bundled Python (pinned
3.14.8, SHA-256 checked).

`wasd-cli.exe paths` prints what this copy uses; the overlay logs
"Your data: ..." at start. Folders are created on first write.

## Tests

No game needed. From a PowerShell prompt:

```powershell
.\test.ps1                 # every suite; exits non-zero if any fails
.\test.ps1 -Only js        # one suite: cpp, js, py, pkg or realdata
.\test.ps1 -Filter route   # C++ tests whose name contains "route"
```

| Suite | Where | Covers | Runs on |
|---|---|---|---|
| **C++** | `tests/cpp` | Formulas (soul cost, attunement, CalcCorrectGraph curves, poise, stamina, roll tiers), session bookkeeping (deaths, souls lost / recovered / gained, time per area), area lookup, JSON and the perf CSV, overlay wrapping, clock directions, Untended Graves vs. normal world, the route `needs` rules and "later" bosses. **Data lint:** every `data/*.tsv` through the real loaders, and the tables checked against each other (route and missable flags name real bosses, area names are areas of `region_areas.tsv`, key items exist under the same name in `item_names.tsv`, every boss in exactly one route step). | Windows + MSVC |
| **JS** | `tests/js` | The live page's `derive()` (the spoiler rules for bosses and items at each tier, the boss chart, "later" bosses, tiles, status) and its text parsers; the results page's `summarise()` (trail runs, warp attribution, time and deaths per area) and the perf verdict. The functions are cut out of the templates by `tests/js/extract.mjs`, so the tests run the pages' real code. | Node 18+ |
| **Packaging** | `tests/pkg` | Rebuilds the exe, then checks what Windows and the installer read from it: the version resource matches `src/version.h` (numbers, product name, publisher), the manifest is `asInvoker` and the only one, the icon is embedded at every size, the exe is x64 and imports only Windows' own DLLs (no Visual C++ runtime), and `--version` prints the version. The install/uninstall cycle (`-Only pkg`) adds a real install, upgrade and uninstall. | Windows |
| **Installer** | `tests/pkg/cycle_installer.py` | `.\test.ps1 -Only pkg` (not in the default run): compiles test builds of the installer, then installs into a folder with a space and an accent, checks every file, the uninstall entry's fields, the Start menu shortcut's target and identity, and the installed exes; upgrades to a "0.1.1" test build (same folder, one entry, data kept); checks uninstall is refused while the app runs; uninstalls (everything gone, data kept); and `/REMOVEUSERDATA=1`. All under a test identity, so a real install is never touched. | Windows + Inno Setup 6 |
| **Release** | `tests/pkg/test_release.py`, `cycle_release.py` | `release.ps1`'s pre-flight checks on a throwaway git repo (default run): clean and untagged passes; uncommitted, untracked, already tagged or no version stops it. And with `-Only pkg`, a full `release.ps1 -DryRun -SkipTests` into a temp folder: the test-identity installer and a matching `.sha256`, optimised exes in `build\release`, the dev exes in `build\` untouched. | Windows + git (+ Inno Setup) |
| **Real data** | `tests/realdata` | `.\test.ps1 -Only realdata` (not in the default run): the bundled and the dev Python run the extractor on your real DS3 install; the output must be identical. Skipped without the game. | Windows + the game |
| **Python** | `tests/py` | `tools/ds3_archive.py` (name hash, RSA header decryption with a toy key, AES against the FIPS-197 vector, DCX, reading a synthetic archive), `tools/extract_treasures.py` (a synthetic MSB: treasure → part → collision → play region, the m40 Untended split, the nearest-region fallback, enemies), `tools/find_patterns.py` (references, unique patterns and their resolution on a synthetic PE image) and what goes into a release (`data/` holds only our own files, the licence, the installer's list). | Python 3.8+ (AES test: Windows) |

How the C++ tests reach the code: `tests/cpp/run_tests.cpp` includes
`src/main.cpp` with `DS3_NO_MAIN` defined (which drops its `wmain`) and
builds `build\wasd-tests.exe`, so tests call the tool's own functions.
Logic that reads the game is tested through its pure core where there is
one (`BossIsLaterIn`, `PerfCsvToJson`). Anything that needs the running
game still needs a live check.

In a cloud session (no MSVC), the JS and Python suites run as
`node --test "tests/js/*.test.mjs"` and
`python -I -m unittest discover -s tests/py`.

Found by the tests when they were written (2026-10-07): `bosses.tsv`
had Lothric, Younger Prince in Lothric Castle, but his arena is play
region 341010, which is Grand Archives, so the Bosses panel would have
missed him there. And `ds3_archive.py` left the `.bhd` file open.

## Commands

All commands attach read-only and print timestamped, latency-tracked
output. None of them ever write to the game.

| Command | What it does |
|---|---|
| `extract [--game <folder>]` | Reads item and enemy positions from your DS3 install into the map data (see "Where WASD keeps its files"). Exit codes: 0 done, 2 game folder not found, 3 no map archives there, 4 an archive couldn't be read, 5 no Python. |
| `paths` | Prints where this copy reads its program files and writes your data (see "Where WASD keeps its files"), and whether it runs as a dev checkout or installed. UTF-8 output. |
| `probe [--list-modules]` | Milestone-1 self-test: find the process, verify the Seamless Co-op marker, open a read-only handle, do 5 timed reads of the module's own PE header (`'MZ'`). Proves the mechanics without depending on any gameplay offset. Runs only when named (it was the default until 2026-10-07). |
| `hp [iterations]` | Re-derives the HP pointer chain fresh every run and polls it at 10Hz, printing HP + read latency each time (default 100 reads, ~10s). |
| `overlay` | **The main way to play with the guide**, and what runs when no command is given, e.g. when `wasd-cli.exe` is double-clicked. If a double-clicked run stops with an error, the window stays open until Enter (never when run from a terminal or script). A click-through HUD panel over the game window's top-right (game windowed or borderless) with only the critical hints: the spoiler tier with the nearest item, bosses done / total here, and boss weaknesses at the tier. It also serves the **live page**, a dashboard for a second monitor at http://localhost:8765 (this PC only), and opens it in your browser. It waits for the game and re-attaches after restarts. **F10** hides the overlay and pauses polling, **F11** shows perf figures, **F9** cycles the spoiler tier (also settable on the page). Ctrl+C exits. See "Live page" below. |
| `flag <id> [...] [--trace]` / `flag --bosses` | Reads DS3 event flags: on/off records of item pickups, boss kills, bonfires lit, quest steps. `--bosses` checks the 25 boss-defeated flags. See "Event flags" below. |
| `nearby [N]` / `nearby --watch` | The N closest unfound world pickups with distance, clock direction and height, from map-file positions (run `tools\extract_treasures.py` once first). `--watch` logs how far you stood from each item you pick up. The companion window shows the closest as "Nearest". |
| `missables [--full]` | Missable-content warnings (NPC questline steps, missable items, ending requirements) from `data/missables.tsv`: pending / missed / done at the spoiler tier, with marks made on the live page. Also a page card and an overlay alert in the entry's area. |
| `route [--full]` | Route hints: the game's steps (main path, optional areas, DLC) from `data/route.tsv`, open / done / locked from boss flags, and the suggested next step, at the spoiler tier. Also on the live page and as an overlay line. |
| `resist [--full]` | Bosses' damage absorptions and status resistances, live from `NpcParam`, with each boss's row found through the map files. Spoiler tier Full only (or `--full`), defeated bosses included. The overlay's boss line shows the nearest boss still to fight's weaknesses, also at Full only. |
| `upgrades` | Each equipped weapon's next upgrade: the materials it needs against what you hold (e.g. "Long Sword +0 -> +1: Titanite Shard 1/2"). The companion window shows the active right-hand weapon's. |
| `keys` | Key items (keys, quest items, spell tomes and scrolls): held, obtained (used up) or not yet, with what each opens and, for world pickups, the map section. The companion window shows "X / 35 key items (Y held)". |
| `progress` | Bosses grouped by area (defeated or not, required or optional, with notes), base-game totals, and Estus / Undead Bone Shards found out of the total. The companion window shows the current area's bosses and the shard counts; the overlay shows "Bosses X / Y defeated here". |
| `items [--map NN]` | The current (or given) map section's one-time world pickups, found or not, with item names. The companion window and overlay show the count ("Items 2 / 33"). See "Unfound items" below. |
| `paramsearch <table> <int32 value> [hex row bytes]` | Finds every row and offset of a live game table holding a number, e.g. which `ItemLotParam` row sets a given pickup flag. |
| `flagwatch [--learn n] [--iter n]` | Prints every event flag that flips (with its id) while you play, after learning and suppressing flags that toggle on their own. For working out which flag belongs to which pickup, door or bonfire. |
| `report [--no-open]` | Builds `reports/results.html` from every saved session and opens it in the browser: headline totals, time and deaths per area, level over the playthrough, a death map per area, the guide's own performance per session (Milestone 5), and a sessions table. Self-contained (no network). See "Session recording and the results page" below. |
| `window` | Retired: the Milestone 3 companion window was replaced by the live page. Runs `overlay`. |
| `stats --live` | **Milestone 2.** Same as `stats`, but runs until closed. **F10** pauses/resumes even with the game focused: zero memory reads while paused, and the key is consumed so the game never sees it. Ctrl+C stops with a summary, and it exits by itself if the game closes. The status line prints only when a value changes. A `perf:` line every 5s shows measured poll rate, read latency (avg/max), and the tool's own CPU and memory. See "Milestone 2 — live mode" below. |
| `stats [iterations]` | **The main event.** Polls HP/FP/Stamina/current-equip-load/Level/Souls at 10Hz, prints Item Discovery once at startup. Attributes and base-max are read once at startup, then automatically re-resolved the moment Level changes mid-run; all 14 equipped-item slots are checked every tick and any change is resolved to full item identity on the spot. Also prints Max Equip Load and Total Poise once at startup **and every time an equip slot changes** (armor pieces AND rings both trigger a recompute), both *computed* live from Vitality/EquipParamProtector/SpEffectParam — see "Armor Poise" below (ring-bonus formula still unverified, no poise ring tested yet). Also prints Absorption % for all 8 damage types, Bleed/Poison/Frost/Curse (+Toxic) resistance totals, Attunement Slot count, equipped spells per slot, and the active right/left hand + bottom (quick item) slot (each with its own per-tick change detection), all live-verified exact matches — see "Armor Defense/Absorption", "Status Resistances", "Attunement Slots", "Equipped Spells", and "Active Right/Left Hand and Bottom Slot" below. Top slot (active spell) is a known open gap. Prints a *computed* souls-required-for-next-level figure. Also prints computed Attack Rating (Physical + any active elemental type, correctly reflecting live two-handing state) for each of the 6 weapon slots — live-verified exact match for Physical, Fire, Spell Buff, and two-handing detection, see "Weapon Attack Rating" and "Two-handing state" below. This is what to run after a game restart to confirm the chain still holds. |
| `scan <int32 value>` / `rescan <int32 value>` | Read-only value scan/narrow (a hand-rolled, read-only equivalent of Cheat Engine's "first scan"/"next scan"), used to originally locate and verify the live HP address against the HUD. |
| `fscan <float value> [tolerance]` / `frescan <float value> [tolerance]` | Same as `scan`/`rescan`, but for float32 values within `tolerance` (default 0.05) rather than exact int32 equality — the HUD shows limited decimal precision, so an exact-match scan would miss real hits. For values like equip load / item discovery with no source lead in any reference project checked, this is the only way in. |
| `watch <hex address> [iterations] [--float]` | Repeatedly reads one raw address, for eyeballing a scan/rescan (or fscan/frescan) result against the status screen in real time. |
| `findptr <hex target> [hex max back-offset]` | Read-only backward pointer scan: what points at-or-near an address. Used during the original hunt for a static path to HP; mostly superseded by `wcm` now that a verified byte pattern exists. |
| `resolve <hex offset0> [offset1 ...]` | Walks a chain of module-relative offsets and prints the resulting int32 value repeatedly. `wcm`'s output prints a ready-to-paste `resolve` command. |
| `wcm [hex target address]` | Diagnostic: runs the WorldChrMan/XA byte-pattern scan and prints each hop. Pass a confirmed HP address (from `watch`) to have it compute the final struct-relative offset for you. |
| `dump <hex module offset> [byte count] [save path]` | Read-only hex dump of raw code/data bytes at `DarkSoulsIII.exe+offset` (default 256 bytes). Same read mechanism as the AOB pattern scanning. Pass a save path to also write the exact bytes to disk for offline disassembly. |
| `roots` | Finds every memory root by its byte pattern in the running game and shows how many times each pattern matches (must be exactly once) and what it resolves to. The first thing to run if a game update ever breaks reading. See "Memory roots — our own byte patterns". |
| `dumpmodule <save path>` | Read-only copy of the whole `DarkSoulsIII.exe` module (its loaded code and data) to a file, page by page; unreadable pages are left as zeros. For finding our own byte patterns offline with `tools/find_patterns.py`. The file is the game's own code: keep it private. |
| `peek <hex absolute address> [byte count] [save path]` | Same as `dump`, but for an absolute address (e.g. a `scan`/`rescan`/`findptr` hit) instead of a module offset. Investigative only, not restart-proof by itself. |
| `paramrow <table name> <decimal row id> [byte count] [save path]` | Hex-dumps any live param row by table name (e.g. `EquipParamWeapon`) + row id, the same way `peek`/`dump` do for raw addresses. General-purpose investigative tool built for the weapon-AR work, reusable for any future per-item-stat feature. |
| `equip` | Resolves every equipped slot (weapons, armor, rings) to its real `{uniqueId, giveId, quantity}` record, plus an English name if `generated/item_names.tsv` is present (made by `extract`). Disassembly-verified — see "Equipped item identity — solved" below. |
| `inventory` | Same chain as `equip`, but walks the *entire* inventory (every item ever acquired, not just what's equipped) using the container's own real capacity as the bound. See "Full inventory — also solved" below. |
| `ar [--verbose] [--row <giveId>] [--infusions <giveId>]` | Attack Rating for every weapon in the inventory (or any `EquipParamWeapon` row), printed the way the game's menu shows it — `base+bonus`, or `base-penalty` when a stat requirement is unmet — for both one- and two-handed, plus Spell Buff for catalysts and each weapon's decoded `AttackElementCorrectParam` row. `--infusions` computes all 16 infusions of one weapon (to compare against the blacksmith's preview); `--verbose` dumps every formula input. See "Weapon Attack Rating". |
| `memdiff <pgd\|chrins\|chrmods\|chrdata\|hex addr> [hex deref ...] [--len hex] [--learn n] [--iter n] [--summary]` | Live byte-diff of a structure (optionally reached through a pointer chain). Spends the first `--learn` ticks (default 30, ~3s) marking anything that changes on its own as noise, then prints only the int32s that change after that — i.e. what *your* in-game action changed. Found the active-spell index. `--summary` prints one line per changed float at the end instead (change count, first/last/min/max), for actions that change too much to read live; it found player position. |
| `inv [--id <decimal inventoryItemId>]` | The investigation tool that led to `equip`/`inventory`: hex-dumps `EquipInventoryData` and probes candidate array bases against a given inventory item ID. Kept for reference/further investigation (e.g. Attack Power). |

## Memory roots — our own byte patterns

WASD finds its way into the game's memory through seven roots: the
pointer cells `WorldChrMan`, `GameDataMan` ("BaseA"), `GameMan`,
`EventFlagMan`, `FieldArea` and the param master, and the struct offset
`XA`. Each is found by a byte pattern searched in the running game's
code, so nothing depends on one patch's addresses.

**How the patterns were found (2026-10-09):**

1. `wasd-cli.exe dumpmodule <file>` copied the loaded `DarkSoulsIII.exe`
   module to a private file, read-only (the game's own code: never in
   the repository).
2. `tools/find_patterns.py` listed every instruction that refers to each
   root (WorldChrMan alone has 1,903), grew a pattern from each one until
   it matched exactly once, and kept the best: at least 16 bytes, no
   jumps in the context (bytes past a jump belong to other code), the
   instruction's own displacement as a wildcard.
3. Every pattern is REX + opcode + ModRM with the displacement at +3, so
   a cell is `instruction + 7 + disp32` and `XA` is the disp32 itself.

**Checked:** on DS3 1.15.2 each pattern matched exactly once in the dump
and live (`roots`), and resolved to the same place as the copied pattern
it replaced; the param master resolves to `+0x479B8B0`, the cell that was
hard-coded for 1.15.2 before. Flags, param tables, stats, equip load and
absorption all read as before on the new patterns. `tests/cpp/test_patterns.cpp`
checks each pattern's shape and the scanner's arithmetic on synthetic
code; `tests/py/test_find_patterns.py` tests the finder.

## The verified HP pointer chain

```
DarkSoulsIII.exe + 0x477FDB8   (WorldChrMan pointer cell -- found by AOB scan, not hardcoded)
  -> deref -> +0x80
  -> deref -> +0x1F90          (the "XA" constant -- also found by AOB scan)
  -> deref -> +0x18            (this is the live stat struct base)
                                  -> +0xD8 = HP current    +0xDC = max    +0xE0 = base max
                                  -> +0xE4 = FP current     +0xE8 = max    +0xEC = base max
                                  -> +0xF0 = Stamina current +0xF4 = max   +0xF8 = base max
```

Implemented as `ResolvePlayerHp()` in `src/main.cpp`, used by `hp`,
`stats`, and `wcm`. All nine fields (three stats × current/max/base-max)
are wired up and verified live.

## Level, Souls, and required-souls-to-level

Level and Souls are save-profile data, not live combat state, so they
live behind a *different* global than HP/FP/Stamina:

```
DarkSoulsIII.exe + <BaseA offset>   (BaseA pointer cell -- found by AOB scan, not hardcoded)
  -> deref -> +0x10
  -> deref -> +0x44                 (CharacterStats struct base)
                                       +0x00 = Vigor          +0x18 = Faith
                                       +0x04 = Attunement     +0x1C = Luck
                                       +0x08 = Endurance      +0x20 = (unused)
                                       +0x0C = Strength       +0x24 = (unused)
                                       +0x10 = Dexterity      +0x28 = Vitality
                                       +0x14 = Intelligence   +0x2C = Level
                                                               +0x30 = Souls
```

Implemented as `ResolvePlayerProfile()`, same AOB technique as the
WorldChrMan chain (our own pattern; the chain shape is a fact from
veeenu/darksoulsiii-practice-tool).
The field layout was independently cross-checked against
AmySouls/DS3RuntimeScripting's own `Attributes` struct, which lands on
the exact same 0x2C (soulLevel) offset despite being written by a
different author for a different tool — then still verified live
(Level=32, Souls=13359→17319→20245 across three runs, all confirmed by
the user against the character status screen).

All nine base attributes (Vigor, Attunement, Endurance, Strength,
Dexterity, Intelligence, Faith, Luck, Vitality) live in the same struct
and were wired up the same way — confirmed both against the status
screen and by an independent arithmetic identity DS3 itself uses
(`SoulLevel = AttributesTotal - 89`): the read attributes summed to
121, and 121 - 89 = 32, matching the already-verified Level exactly.

**Equip slots** (weapons, armor, rings, arrows/bolts, covenant) all live
in one array on `EquipGameData` (`PlayerGameData base + 0x228`), at
`+0x24 + slot*4`. Cross-referenced against AmySouls/DS3RuntimeScripting's
`InventorySlot` enum, which lines up exactly with the weapon offsets
already verified live (see below):

```
EquipGameData + 0x24 + slot*4:
  slot 0-5   = L1, R1, L2, R2, L3, R3 weapons     (verified live)
  slot 6-9   = Primary/Secondary Arrow, Bolt      (not yet live-verified)
  slot 12-15 = Head, Chest, Hands, Legs           (not yet live-verified)
  slot 17-20 = Ring 1-4                           (not yet live-verified)
  slot 21    = Covenant                           (not yet live-verified)
```

Every one of these reads is a raw `inventoryItemId` — a small, per-item
reference number, **not** the item's identity. See "Equipped item
identity" below for what that number does and doesn't get you.

**Required souls for the next level is not read from memory at all.**
DS3 computes it on the fly from current level via a documented formula
rather than storing it anywhere. `RequiredSoulsForLevel()` reimplements
that formula (~2.5%/level for levels 2-12, a cubic for 13+), sourced
from the community [Fextralife wiki](https://darksouls3.wiki.fextralife.com/Level)
and cross-checked against that page's own published level→cost table
before use (level 33 → formula gives 6640, table says 6640, exact
match). This is the one value in `stats`'s output that's *computed*,
not *read* — since live-verified against the actual in-game level-up
screen too (2026-08-19, level 47→48: computed 13435, screen said
13435, exact match).

**How each piece was derived** (see chat history / commit log for the
full trail):

1. Used a hand-rolled read-only memory scanner (`scan`/`rescan`, modeled
   on Cheat Engine's value-scan workflow) to find and verify a live HP
   address by watching it change in real time against the in-game HUD.
2. That raw address is only valid for one game session — ASLR and heap
   allocation mean it moves on every restart. To get something that
   survives a restart, the chain needs to be expressed as offsets from
   the module's own base address plus stable struct-field offsets.
3. Cloned [veeenu/darksoulsiii-practice-tool](https://github.com/veeenu/darksoulsiii-practice-tool)
   (a real, maintained, open-source DS3 speedrunning tool) and found its
   actual `WorldChrMan` byte signature in
   `xtask/src/codegen/aob_scans.rs` — a genuine AOB (array-of-bytes)
   pattern scanned against the game's own code, not a fixed hex offset.
   This is *why* it should survive patches: the practice tool uses this
   same technique specifically because hardcoded offsets break on every
   game update and byte patterns mostly don't.
4. Implemented the same technique here (`ParsePattern` +
   `FindPatternInModule` + the RIP-relative math in `ResolvePlayerHp`),
   scanning the *live* process's mapped module memory (still 100%
   read-only) rather than a file on disk. (2026-10-09: the copied
   patterns are replaced by our own, found in the game's code; see
   "Memory roots — our own byte patterns" below.)
5. Found a second open-source project
   ([AmySouls/DS3RuntimeScripting](https://github.com/AmySouls/DS3RuntimeScripting))
   with a hardcoded HP field offset (`+0xD8` from a character-instance
   pointer). Initially tested this against our own scan-derived address
   and it **did not match** — a plain backward pointer-scan couldn't
   find any pointer at that exact offset. Rather than trust an
   unverified number, this was set aside.
6. Once the AOB-derived chain (`+0x80 -> +XA -> +0x18`) was live and
   producing a plausible struct address, the same `+0xD8` offset was
   retested against *that* correctly-navigated struct — and this time
   the value it read matched the live HUD exactly, confirmed
   interactively by the user (549 HP, then reconfirmed at 474 HP in a
   later run).
7. The same DS3RuntimeScripting source also listed FP and Stamina as
   sibling fields on the identical struct (`+0xE4`, `+0xF0`), in a
   clean current/max/base-max triplet pattern per stat — a good
   internal-consistency signal, but tested against the HUD anyway
   rather than assumed: both matched on the first try (FP=24,
   Stamina=94, confirmed by the user against a 634-HP live read).
8. Wired up and verified the remaining six fields in the same triplets
   (max + base-max for all three stats) against the character status
   screen: HP 634/715 (base max 550, suggesting an active ring/buff),
   FP 24/114, Stamina 94/94 — all confirmed correct, including the
   base-max/max HP mismatch being a real buff effect rather than a bug.

**Caveat, worth repeating:** `+0x80` and `+0x18` are structural hops
that appear un-versioned (constant across the version table) in the
practice tool's own source. `WorldChrMan`'s cell and the `XA` constant
are re-derived from code bytes on every single run of this tool, so
they should ride out most future game patches automatically. The
`+0xD8` HP field offset is the one number in this chain with no
self-verifying signature behind it — if `hp` or `wcm` ever prints a
value that doesn't match the HUD (most likely after a game update),
that's the first thing to re-derive: run `wcm <confirmed-HP-address>`
(found via a fresh `scan`/`rescan`/`watch` pass) to compute the new
offset.

## Current equip load and item discovery — a third independent anchor

Unlike everything above, neither of these had any source lead at all —
checked across DS3RuntimeScripting, veeenu/darksoulsiii-practice-tool,
gitDanilo/GitGud, and The Grand Archives' Cheat Engine table. Found
live instead, the same way HP originally was:

1. `fscan 18.6` (the status screen's reported equip load) against live
   memory, narrowed across three independent value changes
   (18.6 → 14.6 → 31.6, confirmed correct each time by decoding the
   raw float bits, not just eyeballing decimal digits) down to 3
   candidate addresses.
2. Came back to it later in the session; the same 3 addresses were
   still alive and still tracked a fresh value change (17.1 → 14.6)
   correctly, even after a long gap — the addresses themselves are
   session-stable.
3. `findptr` traced one of them back to a **static, module-embedded
   pointer cell** — `DarkSoulsIII.exe+0x47F3B98` — the same category of
   anchor as `WorldChrMan`/`BaseA` (a fixed offset into the module's
   own data section, re-derived fresh every run, not a hardcoded
   absolute address), just found by tracing a scan hit backward instead
   of an AOB byte-signature scan. Re-verified this chain resolves to
   the exact right value after yet another independent change,
   confirmed via `[BitConverter]::ToSingle` on the raw bits (`14.6`
   exactly, twice).

**The verified chain:**
```
DarkSoulsIII.exe + 0x47F3B98   (static pointer cell -- found via findptr, not AOB)
  -> deref -> +0x1E7C = current equip load (f32, live-verified)
  -> deref -> +0x1E84 = Item Discovery (int32, strong circumstantial match)
```

This lands in what looks like a menu/status-screen display cache
(surrounding bytes include what appear to be re-ordered attribute
values, not the live `CharacterStats` layout used elsewhere in this
file) rather than a live gameplay struct — plausible, since this is
exactly the kind of aggregate the status screen itself would need to
render in one place.

**Item Discovery** at `+0x1E84` matched the user's reported value (107)
exactly on first inspection — right next to a fully-verified real stat,
right data type (a plain int32, matching how DS3 always displays this
as a whole number). Strong evidence, but **not yet independently
live-swap-verified** the way equip load was (no discovery-affecting
gear was on hand this session to test an actual value change) — treat
as provisional until that test happens.

**Max equip load — solved, computed rather than read.** No plausible
~48.0 float ever turned up in memory near this struct, and it's a poor
fit for scan/rescan anyway (only changes on a Vitality level-up, not a
quick gear swap). Same answer as required-souls-for-level: don't scan
for it, compute it. Per the community-maintained
[Fextralife wiki](https://darksouls3.wiki.fextralife.com/Equipment_Load):

```
Base max equip load = 40.0 + (Vitality x 1.0)
```

capping at 139 when Vitality hits its own cap of 99 (`40+99=139`, an
internally consistent check against the wiki's own numbers). Verified
against this session's *own* live data, not just the wiki: this
character's Vitality was 8 exactly when max equip load was found live
at 48.0 — `40+8=48` exactly, computed by `ComputeBaseMaxEquipLoad()`
and confirmed by running `stats` and reading the result straight off,
no adjustment needed.

Two rings modify this **multiplicatively, not additively** (same wiki
page): Havel's Ring (+15/17/18/19% at +0/+1/+2/+3) and Ring of Favor
(+5/6/7/8% at +0/+1/+2/+3) — e.g. both unupgraded stack as
`1.15 × 1.05 = 1.2075`, not `1.20`. **Now computed, not just detected**
— see "Ring equip-load bonus" below, right after "Ring poise bonus"
(same SpEffectParam mechanism, same live-corroborated field-default
evidence, same still-open caveat: no load-boosting ring was on hand to
test the actual bonus value against). A name-based warning
(`EquipLoadRingBonusWarning`) is kept as a sanity-check fallback only —
it fires if Havel's/Favor is detected equipped by name but the computed
multiplier still came back at 1.0, which would mean the SpEffect chain
silently failed for that ring rather than that no bonus exists.

## Armor Poise — live param-table read + a live-discovered field transform

This is the passive equipment-Poise stat shown on the character status
screen. It's computed live
from `EquipParamProtector`, DS3's per-armor-piece data table, not
scanned or hardcoded per item.

**Reading any param table live, generically.** DS3 keeps every game
data table (weapons, armor, rings, goods, SpEffects, ...) behind one
shared navigation structure — `SoloParamRepository` — walked here the
same way veeenu/darksoulsiii-practice-tool's `Params` type does (its
`lib/libds3/src/params/mod.rs`, fully read and ported to C++, entirely
read-only pointer-chasing):

```
DarkSoulsIII.exe + 0x479B8B0     (ParamMaster cell on 1.15.2 -- found by
                                   our own byte pattern since
                                   2026-10-09; it used to be hard-coded
                                   for this one patch)
  -> {start, end}: array of ParamEntry* (one per table, ~200 tables)
  -> match by embedded name (UTF-16, direct-or-indirect string per
     ParamEntry's own length field) e.g. L"EquipParamProtector"
  -> deref +0x68, deref +0x68 again -> table object
     -> +0x0A = row count (u16)
     -> +0x40 = ParamEntryOffset[rowCount], {id:i64, offset:isize, unk:i64}
        -> linear-scan for a row's id, table base + offset = row address
```

`ResolveParamTable`/`FindParamRow` in `main.cpp` implement this
generically — any table name, any row id — intended for reuse beyond
poise (per-item stats generally, not just this one field). Row *byte
layout* within a found address still needs a source per table;
`EquipParamProtector`'s came from practice-tool's autogenerated
`param_data.rs` (`#[repr(C)]`, so declared field order + natural C
alignment gives real byte offsets).

**The `poise` field itself was live-verified, and turned out not to be
the display value directly.** `EquipParamProtector.poise` sits at
`+0x110` — confirmed structurally live (every neighboring field reads a
plausible value at its predicted offset: `weight` at `+0x20` matched
the wiki exactly for all 4 of the user's equipped pieces, 3.0/4.2/1.4/
2.9; the 7 damage-cut-rate floats and 8 material-id u16s right before
`poise` all land exactly where the struct predicts). But reading it
directly gave 0.9870/0.9890/0.9990/0.9840 for the user's 4 pieces —
clustered near 1.0, nothing like the wiki's per-item poise numbers
(1.3/1.1/0.1/1.6) — and an exhaustive live scan of the entire row
(every byte offset, ±0.02 tolerance) found **no** float anywhere
matching those wiki values. The fix: the raw field is a *poise-damage
multiplier* (1.0 = no reduction), not a point value —
`100 * (1 - raw)` reproduces the wiki numbers exactly (100×(1-0.987) =
1.3, and so on for all 4 pieces).

**Combining pieces uses diminishing returns, not a plain sum** — same
formula as the community documents for poise, and mathematically
the natural consequence of the raw fields being multipliers: combining
rates by multiplication is equivalent to combining their derived point
values via `a + b - (a*b)/100` (from `(1-a/100)(1-b/100) = 1-(a+b-ab/
100)/100`). `CombinePoise()` in `main.cpp` implements this; only
Head/Chest/Hands/Legs are summed.

**Verified against this session's own live data (2026-08-19):** the
user's 4 equipped pieces, converted then combined, gave `4.0436`,
rounding to `4.04` — reconfirmed as the exact status-screen value
immediately before this fix. `stats` now prints this total every run.

### Ring poise bonus — implemented, offset live-corroborated, formula still unverified

`EquipParamAccessory` (rings) has no `poise` field of its own — ring
poise bonuses (e.g. Wolf Ring's documented "+30% Poise") work through
`SpEffectParam` instead, DS3's general status-effect system. Each
accessory row has 5 SpEffect reference slots (`ref_id0`/`ref_id1..4`,
offsets computed from practice-tool's struct layout the same way as
everything else here); SpEffectParam itself has a `poise_rate` field.

**No poise ring was on hand this session (2026-08-19) to test directly**,
but the field's *default* value was still live-corroborated: the user's
4 actually-equipped rings (Great Swamp Ring, Covetous Silver Serpent
Ring, Sage Ring, Saint's Ring — none poise-related) each independently
read exactly `poise_rate=1.0000` for their one active SpEffect. An
initial guess that this was an *additive* bonus (default `0.0`, applied
as `armorTotal*(1+sum(rate))`) produced a nonsensical live `+400%` —
four unrelated rings each contributing a spurious "+100%" — which is
what surfaced the real signal: four independent, unrelated SpEffect
rows landing on the exact same `1.0` is too consistent to be
coincidence, and is exactly what a *multiplicative* rate defaulting to
"no change" would produce. Current formula:
`final = armorTotal * product(poise_rate across all active ring
SpEffects)` — a real poise ring would presumably carry e.g.
`poise_rate=1.30` for "+30%", multiplying in rather than adding.
Re-verified after the fix: the same 4 rings now correctly contribute no
bonus (`4.04` unchanged, not `20.22`).

**Still open:** the *combination formula* is corroborated by the
default-value evidence above but not by an actual bonus — no Wolf Ring
or equivalent has been tested. If a live reading with a real poise ring
doesn't match this multiplicative formula once one's available, that's
the next thing to check.

**Also open:** `EquipParamWeapon` has no `poise` field either — any
weapon-side poise effects (e.g. a Weapon Art) aren't covered by this at
all.

### Ring equip-load bonus — same mechanism as ring poise bonus

Havel's Ring and Ring of Favor's max-equip-load multiplier is computed
the same way as ring poise: `ComputeRingEquipLoadRateMultiplier` shares
all of the ring/SpEffect-resolution machinery with
`ComputeRingPoiseRateMultiplier` (refactored into one shared
`ComputeRingSpEffectRateMultiplier`, parameterized on which
`SpEffectParam` field to read) — just pointed at
`equip_weight_change_rate` instead of `poise_rate`. Offset computed
with the identical field-counting method as `poise_rate`, which that
offset's own live corroboration (see "Ring poise bonus" above)
validates.

Each ring reinforcement level (+0/+1/+2/+3) is already its own
`EquipParamAccessory` row with its own `giveId` in DS3 — unlike weapon
reinforcement, which is a level field *within* one row — so no separate
tier-lookup logic was needed: the normal `giveId → row → ref_id →
SpEffectParam` chain automatically picks up whichever exact tier is
equipped.

**Live-verified (2026-08-19) that this doesn't false-positive**: the
user's 4 actually-equipped rings (none load-related) all correctly
produced a `1.0` multiplier — Max Equip Load stayed at the plain
`48.0` base, and no name-based warning fired. **Still unverified**: an
actual bonus value, since no Havel's Ring or Ring of Favor was on hand
to test. `EquipLoadRingBonusWarning` (name-based detection) is kept as
a sanity-check fallback — if it fires alongside a `1.0` computed
multiplier, that means one of those two rings is equipped but the
SpEffect chain silently failed to pick up its bonus.

## Armor Defense/Absorption — live-verified exact match, all 8 types, 3 decimal places

`stats` computes and prints Absorption % for all 8 damage types
(Physical, Strike, Slash, Thrust, Magic, Fire, Lightning, Dark) —
DS3's status screen shows these as percentages, not raw defense point
totals.

`EquipParamProtector` has *both* an `i16` `defense_X` field per damage
type and an `f32` `X_damage_cut_rate` field. Checked live (2026-08-19)
on Xanthous Crown: every single `defense_X` field — physical, magic,
fire, thunder, slash, blow (strike), thrust, and dark — read exactly
`0`. The `_damage_cut_rate` fields are the real, populated data
instead, using the identical "rate near 1.0" convention already
confirmed for Poise's own field (which sits immediately after this
same cluster in the struct). So `defense_X` is vestigial/unused in
this DS3 build, and Absorption is computed the same way Poise was:
`100 * (1 - raw)` per armor piece, combined across pieces with the
same diminishing-returns formula (`CombinePoise`, reused unchanged —
the raw fields are multiplicative survival rates, so their derived
percentages combine the same way).

**Live-verified exact match, all 8 types at once, to 3 decimal
places** (2026-08-19) — not rounded-off agreement, identical at the
precision the game itself displays:

| Type | Computed | In-game |
|---|---|---|
| Physical | 9.283 | 9.283 |
| Strike | 10.955 | 10.955 |
| Slash | 10.304 | 10.304 |
| Thrust | 9.098 | 9.098 |
| Magic | 21.725 | 21.725 |
| Fire | 21.231 | 21.231 |
| Lightning | 22.980 | 22.980 |
| Dark | 24.361 | 24.361 |

Recomputes automatically alongside Total Poise/Max Equip Load whenever
an equip slot changes (same trigger, same lambda).

## Status Resistances (Bleed/Poison/Frost/Curse) — live-verified, plain addition

`stats` also computes and prints Bleed, Poison, Frost, and Curse
resistance point totals (plus Toxic, the same field cluster, read for
free even though the vanilla UI never shows it). Unlike Absorption,
these are plain `i16` point values in `EquipParamProtector` (no "rate
near 1.0" transform needed) — `resist_blood` (Bleed), `resist_poison`,
`resist_curse` sit right after `defense_thrust`; `resist_frost` sits
much further into the row, near `upper_arm_id`.

**Combined via plain addition across armor pieces** — a genuinely
different mechanic from Poise/Absorption's diminishing-returns formula,
not the same math reused again. DS3's build-up resistances are
well-documented as simple additive point totals.

**Live-verified (2026-08-19)**: computed Bleed=85, Poison=155,
Frost=111, Curse=147 — all four confirmed correct against the user's
own in-game Resistance screen. **Toxic=155 could not be independently
confirmed** the same way: the vanilla status screen doesn't display a
Toxic figure at all, so it's read from the same field cluster
(structurally sound, sitting right next to the confirmed
`resist_poison`) but isn't verified against a visible number like the
other four were.

## Attunement Slots — computed, live-verified exact match

Like required-souls-for-level and max equip load, DS3 doesn't store
Attunement Slot count anywhere — it derives it from the Attunement stat
on the fly via a documented breakpoint table (not a smooth formula),
sourced from the [Fextralife wiki](https://darksouls3.wiki.fextralife.com/Attunement):

| Attunement | 0-9 | 10-13 | 14-17 | 18-23 | 24-29 | 30-39 | 40-49 | 50-59 | 60-79 | 80-98 | 99 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Slots | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |

**Ring bonus — wrong guess, then corrected live, then solved properly.**
The first version assumed Sage Ring granted DS3's known +1
attunement-slot ring bonus (misattributed from memory) and hardcoded
that by name. The user caught this live: unequipping a *different*
ring (Saint's Ring) dropped their actual in-game slot count from 3 to
2, proving Sage Ring wasn't the source at all. A first attempt to
re-derive the real mechanism also produced a wrong dead end (manual
hex arithmetic errors made both rings' `ref_id0` SpEffect IDs look
like they didn't exist — they did; the arithmetic was just wrong).
Redone properly: dumped both rings' actual resolved `SpEffectParam`
rows side by side and diffed them byte-for-byte — identical everywhere
except exactly one byte, at `+0x142` (`change_magic_slot`, offset
computed the same way `poise_rate`'s was): Saint's Ring reads `1`,
Sage Ring reads `0`. This is now read **generically** through the same
accessory→SpEffect resolution chain as ring poise/equip-load bonuses
(summed across rings, not multiplied — a flat slot count, not a rate),
so it works for *any* attunement-slot ring, not just the one currently
equipped.

**Live-verified, both directions (2026-08-19)**: base `2` slots at
Attunement=14 (matches the breakpoint table) `+1` from Saint's Ring
= `3`, matching the user's own status screen exactly. Unequipping
Saint's Ring live dropped the computed total to `2` in the same tick,
confirming the recompute-on-gear-change trigger and the ring-bonus
read both work correctly, not just at a single snapshot.

## Equipped Spells — live-verified, one pointer-chase bug caught along the way

`stats` reads and prints the spell attuned in each of the character's
currently-active attunement slots (only the live slot count, not all
14 the game reserves — see "Attunement Slots" above), both at startup
and on a per-tick diff (spells can be re-attuned at a bonfire without
touching gear, so this has its own change-detection trigger,
independent of the gear-change one).

**Chain**: `PlayerGameData+0x470` is a **pointer**, not a direct
offset into `PlayerGameData` itself — sourced from AmySouls/
DS3RuntimeScripting's `PlayerGameData::getSpell()`, whose
`accessMultilevelPointer` helper name is exactly this: one
dereference, then the sub-offset applies to the pointed-to struct.
Slot *N*'s MagicParam id is an `int32` at `(that pointer) + 0x18 +
(N-1)*8`.

**A real bug, caught live, not just an unverified guess.** The first
version skipped that dereference — treating `PlayerGameData+0x470`
as if it were already the array base — and read every slot as empty.
The user immediately caught this: their 3 slots were actually all
filled. Diagnosed by dumping the raw bytes at that offset directly:
they were clearly a valid heap pointer (not spell data), not junk.
Dereferencing it once and re-reading found exactly the expected
8-byte-strided layout, resolving to real spell ids.

**Live-verified exact match (2026-08-19)**: computed slots 1-3 as
`2400000`/`2402000`/`2411000`, resolved via a newly-added `Magic`
category in `item_names.tsv` (~110 spell names; today the names come
from the game's own text) to **Fireball**,
**Fire Orb**, and **Great Combustion** — a coherent Pyromancer
loadout, confirmed correct by the user.

**Known gap**: the "empty slot" sentinel hasn't been directly
observed — all 3 of the user's slots were filled, so `magicId <= 0`
(the current empty-detection check) is a reasonable guess, not a
live-confirmed one.

## Active Right/Left Hand, Top (Spell) and Bottom Slot — all live-verified

`stats` reports which specific item is *currently active* in each of
DS3's 4 quick-access categories — not just what's equipped in R1-R3/
L1-L3, but which ONE of those is actually drawn right now, plus the
active quick-item (bottom slot). Updates automatically the instant any
of them changes, independent of both gear swaps and level-ups (its own
per-tick trigger, ~3 cheap int32 reads).

**Correction (2026-10-06):** the two hand-slot offsets were swapped.
`PlayerGameData + 0x2BC` is the **left** hand's active slot and `+0x2C0`
the **right**'s. The user noticed the companion window's "Right" line
changing when they cycled the left hand, and vice versa. Every earlier
check, including the two-handed AR one, had both hands on the same slot
index (0/0, then 1/1), where a swap is invisible. After the fix, cycling
each hand alone changed only its own line (user-confirmed). The
paragraph below keeps its original offsets as history.

**Right/left hand** — `RightHandSlot`/`LeftHandSlot` (`PlayerGameData`
`+0x2BC`/`+0x2C0`, corrected above), the same fields already found and live-verified for
two-handing detection (see "Weapon Attack Rating" below) — this just
surfaces them as their own named feature and resolves the actual item
name.

**Bottom slot (quick item)** — found via a community Cheat Engine
table's `"m_selectedEquipItemSlotIdx"` entry (`Address: GameDataMan`,
`Offsets: [0x4E0, 0x10]`), calibrated against that same table's own Lua
scripts, which explicitly show `PlayerGameData = readPointer(GameDataMan
+ 0x10)` — exactly this project's own `profile.xBase`. That means the
CE entry's trailing `0x10` is the same hop already baked into `xBase`,
leaving a single offset: `xBase + 0x4E0`. **Live-verified, both
states** (2026-08-19): read `0`, then read `1` after the user cycled
their active quick item live — exact match. Content resolution (which
item occupies a given quick-item bar slot) uses
`EquipGameData::getInventoryItemIdByQuickSlot()`'s own offset family
from AmySouls/DS3RuntimeScripting. **Live-verified correct** against
the user's actual active quick item (Purple Moss Clump) the same
session.

**Top slot (spell) — solved 2026-10-06.** An earlier 12KB diff of
`PlayerGameData` itself showed nothing changing while spells were
cycled. That diff never followed pointers, and the attuned-spell array
already lived *behind* one (`PlayerGameData+0x470` → `EquipMagicData`).
`memdiff pgd 470` watched that object instead, with two other
structures (the `WorldChrMan` player object and its stat struct) watched
alongside as controls. Exactly one field in `EquipMagicData` changed,
once per press, in step with the user: **`EquipMagicData+0x88`**, an
i32 index into the slot array. Its position fits: `0x18 + 14×8 = 0x88`,
the first field after the 14-entry slot array.

**Live-verified** the same session: `stats` resolves the index to a
spell name, and its log matched the user's HUD press for press — Soul
Greatsword → Chaos Bed Vestiges → Soul Greatsword → Chaos Bed Vestiges.
The game only offered those two of the three attuned spells while
cycling (Fire Orb was skipped), and the index tracked that exactly:
it went 0↔1, never 2.

## Weapon Attack Rating — live-verified exact match, Physical and Fire

`stats` computes and prints Attack Rating (the numbers DS3's equipment
screen calls "Attack Power" — Physical, and one or more elemental
figures for infused weapons) for all 6 weapon slots (R1-R3, L1-L3),
reconstructed from three live param tables rather than read from a
single field — DS3 doesn't store a weapon's final AR anywhere; the
game computes it the same way, on demand, from these same tables.

**The formula, in order:**

1. **`EquipParamWeapon`** — `atk_base_physics` is the *unreinforced*
   base value; `correct_strength`/`correct_agility` are the weapon's
   own scaling coefficients (0-100ish); `correct_type` selects a
   growth curve (step 3); `reinforce_type_id` + the numeric upgrade
   level (from the item's `giveId`, same low-2-digits field the rest
   of this file already reads for name lookups) selects a row in step 2.
   Unlike `EquipParamProtector`, DS3 does **not** bake each upgrade
   level into its own row here — confirmed live by searching for
   Pyromancy Flame's +4 row directly and finding only ~2,500 total
   rows in the whole table, far too few for a per-level scheme.
2. **`ReinforceParamWeapon`**, row id = `reinforce_type_id + level` —
   holds the level's multipliers (`physics_atk_rate`,
   `correct_strength_rate`, `correct_agility_rate`). The `+ level`
   indexing (as opposed to `* 100 + level`, tried first) was pinned
   down live: Reinforced Club's `reinforce_type_id=0` couldn't tell
   the two schemes apart, but Fists' `reinforce_type_id=3000` did
   immediately — `3000*100` would be row 300000, past the table's 444
   rows, while row 3000 itself exists and reads a flat `1.0` across
   every rate (Fists can't be reinforced with regular titanite, so
   that's the correct "no bonus" row).
3. **`CalcCorrectGraph`**, row id = the weapon's `correct_type` — a
   piecewise growth curve (5 stat breakpoints → 5 growth-percent
   values, each segment with its own curve exponent) converting a raw
   attribute value into a 0-100 "how much of this stat's investment
   applies" percentage. Field layout and the piecewise formula
   (including how a negative exponent "flips" the curve) are sourced
   from the [Souls Modding wiki](https://www.soulsmodding.com/doku.php?id=ds1-refmat:param:calccorrectgraph).

Combined: `AR = baseAtk + floor(baseAtk * (corrStr/100*growthStr/100 +
corrDex/100*growthDex/100))` — the same formula shape as
[Derling/ds3-attack-rating-calculator](https://github.com/Derling/ds3-attack-rating-calculator),
an existing public DS3 AR tool, reimplemented here against live memory
instead of that tool's offline data snapshot.

**Live-verified exact match (2026-08-19).** Computed one-handed AR for
Reinforced Club (120) and Fists (44) didn't match the user's in-game
readings (128, 55) — the gap turned out to be two-handing's `floor(Str
* 1.5)` effective-Strength bonus (both weapons were being viewed
two-handed in-game). Recomputing with `Str = floor(12*1.5) = 18` gave
**exactly** 128 and **exactly** 55 — two independent exact matches on
the same character, confirming every piece of the formula above (not
just plausible-looking, actually correct end to end). `stats` now
prints both the one-handed and two-handed figure for every weapon slot
side by side, since the live two-handing *state* isn't read yet (no
pointer chain hunted for it) — showing both sidesteps needing one.

### Elemental damage types — live-verified on a Fire infusion, one real mistake caught along the way

Extended to Magic/Fire/Lightning/Dark by infusing the Reinforced Club
with Fire and re-checking (2026-08-19). Two things confirmed live, not
assumed:

- **`attack_element_correct_id`** (which stats can *potentially*
  contribute to which damage type) stayed **identical** (`10000`)
  between the unenchanted and Fire-infused row of the same weapon —
  so that association is a fixed, weapon-archetype-wide constant, not
  something that changes per infusion. What changes per infusion is
  each `correct_X` coefficient itself (`0` = that stat contributes
  nothing). Given that, the standard stat-grouping is hardcoded rather
  than decoded from `AttackElementCorrectParam`'s raw 25-slot layout
  (still undeciphered) — it matches both a public DS3 AR calculator's
  own reference table and a community Elden Ring formula writeup (same
  underlying engine convention): Physical←Str+Dex, Magic←Int,
  Fire←Int+Faith, Lightning←Faith, Dark←Int+Faith.
- **`reinforce_type_id` DOES change per infusion** (`0` for Normal,
  `600` for Fire on this weapon) — each infusion gets its own
  `ReinforceParamWeapon` category, still resolved via the same
  `reinforce_type_id + level` scheme validated for Physical.
  `correct_strength_rate`/`correct_agility_rate` read a flat `0.0` at
  *both* level 0 and level 10 on the Fire row — confirming Fire
  infusion's well-documented "zero stat scaling, flat damage" behavior
  is intentional and permanent across the whole upgrade path, not an
  unenchanted-level artifact.

**A real mistake, caught and fixed by the live test — not just a
confirmation.** `correct_luck` reads `14.0` on the Fire row, unchanged
from Normal (unlike magic/faith, which zero out). An initial version
included it unconditionally in Physical's bonus ("it's nonzero, so it
must apply"), giving `Physical=94` against the user's confirmed live
reading of `93` — a genuinely *wrong* number, not just an untested one.
Luck isn't infusion-gated the way magic/faith are; it looks like an
inherent per-weapon-category value that only actually applies under
some other condition (most likely Hollow infusion specifically, via a
different `attack_element_correct_id`). **Luck is deliberately omitted**
rather than guessed — recomputing without it gave exactly `93/93`,
matching the user's screen precisely for both Physical and Fire.

**Superseded 2026-10-06** — see "AttackElementCorrectParam, the
requirement penalty, and the second curve" below. The Luck mystery
above is solved there, and the hardcoded stat grouping is replaced by
the game's own table.

### Two-handing state — live, fully decoded, both toggle directions confirmed

`stats` reads the player's actual current handedness instead of always
showing both one-handed and two-handed figures. Offsets sourced from
AmySouls/DS3RuntimeScripting's `PlayerGameData::getWeaponSheathState()`
/ `getRightHandSlot()` / `getLeftHandSlot()` (`player_game_data.cpp`),
all relative to the same `PlayerGameData` base already used for
`EquipGameData` elsewhere in this file — confirmed by an exact offset
match (`getEquipGameData()` returns `address+0x228`, identical to this
project's own independently-derived `kEquipGameDataOffset`).

**Live-verified, fully decoded (2026-08-19)** — not just sourced from
a reference project, actually toggled by the user in both directions
while `stats` watched: baseline (one-handed) read `WeaponSheathState=1`;
two-handing R1 changed it to `3`; switching back to one-handed
returned it to `1`; two-handing the *left*-hand weapon instead gave
`2` — a clean 3-state enum (`1`=one-handed, `2`=left two-handed,
`3`=right two-handed), confirmed both ways, not assumed from a single
sample. `RightHandSlot`/`LeftHandSlot` (which of that side's 3 weapon
slots — 0-2 — is the currently *drawn* one, independent of handedness)
gate which single slot the two-handed state actually applies to: only
the currently-active slot on the matching side shows two-handed AR,
every other weapon slot always shows one-handed (it isn't the weapon
being gripped right now, regardless of the global sheath state).
Confirmed working live: equipping and two-handing a Hand Axe in L1
correctly showed `Phys=127, two-handed` for L1 while R1 (not the
active two-handed slot) correctly stayed at its one-handed figure in
the same tick.

**Known gap:** `RightHandSlot`/`LeftHandSlot` stayed `0` throughout
testing (the character never swapped which weapon was drawn), so the
slot-index-to-R1/R2/R3 mapping is inferred from the source project's
own "range 0-2" documentation, not independently live-verified across
all three slots on either side.

### AttackElementCorrectParam, the requirement penalty, and the second curve — 20/20 exact

Checked 2026-10-06 against 20 in-game Attack Power and Spell Buff
readings across 13 weapons, all from the inventory screen (it shows
any weapon's figures at your current stats without equipping it).
The run started with 4 of the first 9 wrong. Every miss traced to one
specific mechanism, and the final model reproduces all 20 with no
tuning constants. `ar` prints figures in the menu's own
`base+bonus`/`base-penalty` form, so each one compares directly.

**1. `AttackElementCorrectParam` decides which stat feeds which damage
type.** Layout from soulsmods/Paramdex: 32 flag bits, then two s16[25]
arrays (25 = 5 stats × 5 damage types). The ordering
(`type*5 + stat`, stats Str/Dex/Int/Fth/Luck) comes from Elden Ring's
named equivalent and is confirmed three ways. First, the default row
(10000, used by almost every weapon) reads flags `0x0C43083`, which
is bit for bit the grouping this project had hardcoded before:
Phys←Str,Dex, Magic←Int, Fire←Int,Fth, Lightning←Fth, Dark←Int,Fth.
Second, unique weapons decode to their known quirks: Anri's Straight
Sword Phys←Str,Dex,Fth,Luck; Saint Bident Phys←Str,Dex,Fth; Golden
Ritual Spear Magic←**Faith**. Third, Golden Ritual Spear's in-game
Magic reads `77+39`. The old grouping (Magic←Int) predicted `+0`; this
decode predicts exactly `+39`.

This also solved the old Luck mystery. Normal, Fire and the other
infusions share row 10000, which has Luck switched off, so the Fire
club's nonzero `correct_luck` (14) never applied. **Hollow** infusion
rows point at row 10015 (Phys←Str,Dex,**Luck**) and **Blessed** at
10014 (Phys←Str,Dex,**Fth**), which matches DS3's documented behaviour
for both. `ReinforceParamWeapon` has no Luck rate at all (Paramdex), so
Luck's coefficient is applied without upgrade scaling. That one piece is
**unverified**: the user has no Hollow weapon or gem. `stats` and `ar`
mark Luck-scaled weapons.

**2. The game floors base and bonus separately.** Heysel Pick: base
`61×1.15 = 70.15`, bonus `31.95`. The game shows `70+31 = 101`; a single
combined floor would give 102.

**3. Unmet stat requirements: no scaling, minus floor(40% of base).**
Requirements are u8s at `EquipParamWeapon+0xEE..0xF1` (Str/Dex/Int/Fth).
They were found by dumping rows with well-known requirements
(Greatsword 28/10, Greataxe 32/8, Black Knight Sword 20/18, Sorcerer's
Staff 6/0/10/0), and the order matches Paramdex. A damage type is
penalized if *any* stat feeding it (per the table above) is short.
Verified on three red in-game figures, the user being Dex 9: Long Sword
`110-44`, Golden Ritual Spear Physical `73-29`, Heysel Pick Physical
`93-37`. "Keep 60%" would have given 55 for the last one, not 56.
Two-handing's `floor(Str×1.5)` counts toward the Str requirement.

**4. Elemental damage and Spell Buff use a second curve id, at
`EquipParamWeapon+0x18A`.** Paramdex leaves it unnamed (`Unk26`, right
after `atkBaseDark`). Physical scales on `correctType` (`+0xE8`), as
before. Int/Faith-driven scaling uses this byte instead. How it was found:

- Solve each catalyst's reading for the curve value it needs at stat 40.
- Evaluate every `CalcCorrectGraph` row at 40 and match.
- Search six catalyst rows for a byte equal to the implied row id in
  every one. `0x18A` was the only match.

Values seen: 16 for most staffs and talismans (g(40)=52.96), 5 for
Heretic's Staff (60.00), 6 for Saint's Talisman (43.75), 15 for Izalith
Staff, and 0 for Pyromancy Flame and plain weapons (the same as their
`correctType`, so Physical/Fire results verified earlier don't change).

**5. Spell Buff = `100 + floor(100 × f)`**, where `f` is the scaling
fraction of the catalyst's spell element: sorcery → Magic, pyromancy →
Fire, miracle → Lightning. It's computed with the same table flags and
the same `0x18A` curve as elemental AR, on a fixed base of 100. The
element mapping is what makes Izalith Staff work: it has a Faith
coefficient (51), but its row feeds Magic from Int only, so curve 15 ×
Int 84% = **171**, exact. Summing Faith as well would give 214.

**Correction to an earlier claim:** the 2026-08-19 "Pyromancy Flame
Spell Buff = 230, exact match" was really its **Fire Attack Rating**.
The two are different numbers: `148+150 = 298` Fire vs `201` Spell Buff
at +6, and both now match.

**Every reading, all exact** (Str 12, Dex 9, Int 40, Fth 40, Lck 7):

| Weapon | In-game | Notes |
|---|---|---|
| Hand Axe | Phys 110+17 | read while two-handing: the menu shows two-handed figures for every weapon while you're two-handing |
| Long Sword | Phys 110−44 | Dex requirement 10 unmet |
| Golden Ritual Spear | Phys 73−29, Magic 77+39, Spell Buff 150 | Magic←Faith via its own table row; sorcery curve 16 |
| Heysel Pick | Phys 93−37, Magic 70+31, Spell Buff 145 | |
| Pyromancy Flame +6 | Fire 148+150, Spell Buff 201 | pyromancy: Fire stats (Int+Fth), curve 0 |
| Sorcerer's Staff / Heretic's Staff / Witchtree Branch / Izalith Staff | Spell Buff 152 / 157 / 135 / 171 | curves 16 / 5 / 16 / 15 |
| Saint's Talisman / Sunlight Talisman / Saint-tree Bellvine | Spell Buff 149 / 143 / 141 | curves 6 / 16 / 16 |

How much of this was a prediction rather than a fit: Witchtree,
Izalith, Golden Ritual Spear and Heysel Pick spell buffs were predicted
before they were read, under an earlier version of the curve rule. The
final rule (points 4 and 5) was worked out from the misses, and it
explains all ten catalysts. No catalyst has yet been checked
against it blind.

**Still unverified in-game** (the data tables fully specify them, but
there's been no reading to check against): **Lightning** and **Dark**
damage on weapons, Luck's magnitude on **Hollow** weapons, and **vow**
catalysts (spell element unknown, Spell Buff not computed). With
infusion gems, the blacksmith's infusion menu previews every infusion's
AR, and `ar --infusions <giveId>` prints the predictions to compare.

**General-purpose infrastructure added alongside this:** the
`paramrow` command (`paramrow <table name> <decimal row id> [byte
count] [save path]`) hex-dumps any live param row by table name + row
id, the same way `peek`/`dump` do for raw addresses — used throughout
this investigation and reusable for any future per-item-stat feature.

## Milestone 2 — live mode (`stats --live`)

Built and live-tested 2026-10-06 against Milestone 2 ("polling
loop + latency/rate metrics, hotkey pause/resume").

- **Runs until closed.** Ctrl+C prints a final summary (reads ok,
  avg/max latency, time spent paused). If the game process exits, the
  tool notices on the next tick via `GetExitCodeProcess`, a handle query
  rather than a memory read, and stops.
- **Pause/resume hotkey: F10, and it works with the game focused.** Paused
  means the brief's "zero reads, not just a hidden window": the single
  polling thread blocks in `GetMessage` until the key arrives, so no
  `ReadProcessMemory` call can happen and it uses no CPU.
  - The first version used `RegisterHotKey` (Ctrl+Alt+G). Live testing
    showed it fired with the game unfocused but **never with DS3
    focused**. That fits the game registering raw input with
    `RIDEV_NOHOTKEYS`, which turns off every system hotkey while it's in
    the foreground. Ctrl is also a game control.
  - The fix is a `WH_KEYBOARD_LL` hook on its own thread. That thread
    sleeps in `GetMessage` between key events, so there's no polling.
    Every key except F10 passes straight through. F10 is consumed, and
    auto-repeat is ignored so holding it doesn't toggle repeatedly.
  - Verified with the game focused: `PAUSED (F10)`, 10.1s with no ticks
    and no log output, `RESUMED after 10.1s paused`, then the user's
    rolls showed up tick by tick.
- **Measured, not assumed, 10Hz.** Ticks run on a fixed-deadline
  schedule: each one is due 100ms after the previous *deadline*. The old
  `Sleep(100)` came after the work, so the work time added to every
  tick. Falling more than one tick behind resyncs rather than bursting.
  Measured over 5s windows: 9.98–10.03 Hz.
  - The one exception so far: two windows at ~9.05 Hz, each about half
    a second of skipped ticks. Both happened while the user was
    switching focus to the game, so they look like a system stall
    rather than the tool. The extended stress test (Milestone 5) should
    show whether it happens at other times.
- **Own overhead, logged** (brief: "logged and displayed, not just
  inferred"): read latency about 17–21µs average, 25–140µs maximum (the
  brief targets under 1ms). Tool CPU 0.00–0.31% of one core, measured
  from `GetProcessTimes` deltas. Working set 5.0 MB, private memory
  1.5 MB (`GetProcessMemoryInfo`).

**Also verified by the user 2026-10-06:** Ctrl+C in the tool's own
console stops it cleanly, and with the keyboard hook active the game
felt no different (no input lag or slowdown noticed).

**Not yet tested:** the stop when the game exits, and a measured
game-FPS comparison with the tool on and off. The FPS comparison is
part of the brief's Phase 2 checks.

## Milestone 3 — companion window (`window`) — retired 2026-10-06, replaced by the live page

Built and live-tested 2026-10-06 against Milestone 3:
a separate, small, always-on-top window, not drawn over the game.

- **Two threads.** A polling worker does every `ReadProcessMemory` call
  on the same fixed-deadline 10Hz schedule as `stats --live`. It fills a
  snapshot under a lock and posts it to the window. The UI thread only
  draws, double-buffered with plain Win32/GDI and no extra libraries.
  Dragging the window (a modal loop that would starve a single-threaded
  poller) therefore can't stall the readings.
- **F10** uses the same keyboard hook as `stats --live`, but posts to
  the window rather than to a thread, because a window's queue survives
  modal loops that drop thread messages. While paused the worker blocks
  on an event: zero reads, no CPU.
- **Shows:** HP/FP/Stamina bars, Level / Souls / next-level cost,
  current equip load with DS3's roll tier (<30% light, <70% medium,
  ≤100% heavy, above that overloaded), and the active right/left weapon,
  spell and quick item. Underneath are the brief's metrics: last-read
  timestamp, read latency, measured poll rate, and the tool's own CPU
  and memory. Item names and equip load are re-resolved only when gear
  or an active slot changes, so a normal tick is just int32 reads.
- **Verified live:** the user confirmed every value matched the game.
  F10 paused (log: `PAUSED`, then 6.6s with no reads, then `RESUMED`).
  Measured 10.00–10.02 Hz, CPU 0.00–0.52% of one core. Memory was flat
  over 60s / ~600 repaints: working set 7.4–7.5 MB, private 2.2 MB,
  handle count 112. Fonts are created once rather than per repaint.
- **Limitation — exclusive fullscreen.** In DS3's exclusive fullscreen
  mode the game owns the display and no other window can appear over
  it, always-on-top or not. The user checked the window by alt-tabbing.
  Practical setups are a second monitor, or running the game windowed
  or borderless. The same limit applies to the Milestone 4 overlay: the
  brief's design (a transparent click-through window tracking the game)
  needs the game windowed or borderless, since drawing inside exclusive
  fullscreen would mean injecting into the game's renderer.

## Milestone 4 — overlay (`overlay`)

Built and live-tested 2026-10-06 against Milestone 4,
with the game windowed and the companion window on a second monitor.

- **A panel, not a sheet.** The overlay is a panel-sized
  `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE`
  popup. It sits over the top-right of the game's client area, a spot
  DS3's own HUD leaves empty. A colour key gives rounded corners and the
  panel itself is drawn at 88% opacity. Making it panel-sized rather
  than a transparent window over the whole game keeps the area Windows
  has to composite small.
- **Click-through, never focused.** Mouse clicks on the panel reach the
  game (`WS_EX_TRANSPARENT`, plus `HTTRANSPARENT` / `MA_NOACTIVATE` as
  belt and braces).
- **Tracks the game window by event, not polling.** An out-of-context
  `SetWinEventHook` watches the game process's window-location changes
  and system foreground/minimize changes. Out-of-context means no
  injection: events arrive on this tool's own message loop. Each
  snapshot also triggers a cheap recheck as a fallback, and
  `SetWindowPos` is skipped when nothing has moved.
- **Only visible while the game is the foreground window**, so it
  doesn't float over other apps after alt-tab, and hidden while the
  game is minimized.
- **F10** hides the overlay and pauses polling together, as the brief
  asks. **F11** is the separate perf-HUD toggle (read time, latency,
  Hz, CPU, memory), hidden by default. F11 is only consumed in overlay
  mode.
- **Repaints only on change.** The overlay redraws only when something
  it shows changes, which is most ticks while standing still. The
  companion window still repaints every tick, since it shows the
  last-read timestamp.
- **Verified live:** the user confirmed the panel appears over the
  game, follows the window when dragged, passes clicks through, hides
  on alt-tab, and responds to F10 and F11. Both panel states were
  captured: 4 lines normally, 7 with perf figures. The log records
  F10's pause/resume and F11's toggles.
- **Overhead:** with the game focused and both windows open, the
  process used 0.00–1.88% of one core per 5s sample, **about 0.8%
  average** over a minute. It spikes only while the user drags one of
  the tool's own windows. Working set 10.8 MB, private memory 2.3 MB.
- **Game FPS: a steady 60 with the overlay active and with it hidden
  and polling paused (F10)**, so no measurable difference at the
  user's normal frame rate, which meets the brief's criterion. Caveat:
  DS3 is capped at 60fps, so on a machine with headroom the cap would
  hide a small cost. This shows the tool doesn't push the game below
  60, not that its cost is zero.

## Player position and facing — live-verified, survives relaunch

Built 2026-10-06. `stats`, the companion window and the
overlay show live `Pos=(x, y, z)` (y is height) and the facing angle.

**How it was found.** `memdiff chrins --len 11000 --summary` watched the
player's character object and its whole module block (68 KB). It first
learned noise for 4s while the user stood still, then the user walked
forward and turned on the spot. Several copies of one (x, y, z) triple
moved together (x 268.5 → 245.6, z 605.7 → 600.9, y flat on level
ground), and one float went 0.946 → −0.785, a turn of about 99° in
radians. The new `--summary` option lists each changed cell as a float
with its change count and first/last/min/max, which made the walk stand
out among about 1,000 animation values.

**The chain:** `WorldChrMan → +0x80 → +xa` (the module table that
already holds the poise module at `+0x40`) `→ +0x68` (physics module),
then:

- **facing angle** at `+0x74`, radians
- **position** at `+0x80 / +0x84 / +0x88`, with `+0x8C = 1.0`

The module's `+0x08` points back at the player's own character object.
It's the same AOB-derived root as HP, so no fixed address is involved.

**Verified live:**

- **Spiral-staircase climb:** y rose from −56.34 to −48.60 while x and z
  went round in a circle and facing swept a full 360°.
- **Full game relaunch:** a new process (PID 23320 → 14956), with no code
  changes, resolved the chain and read (257.46, −48.59, 602.49) against
  (257.68, −48.60, 604.75) before closing. That's the same spot at the
  top of the stairs.
- **Quit to menu and reload:** works with the fix below.

**Found along the way: live modes must re-resolve after quit-to-menu.**
`stats --live`, `window` and `overlay` resolved every address once at
startup. Quitting to the menu destroys the character, and the cached
chain then read a placeholder (0, 1, 0). After a reload the player can be
rebuilt at a new address. All three modes now re-read the game's
current-player pointer (`WorldChrMan + 0x80`, 2 reads) every tick:

- **zero** → "not in game", and no player reads at all
- **changed** → every chain is re-resolved, retried at most once a
  second mid-load because it runs AOB scans

Verified: quit-to-menu logged "Player unloaded — reads paused", reloading
logged "Player loaded — all addresses re-resolved", every slot
re-reported correctly, and position kept tracking. Item `uniqueId`s were
renumbered by the reload (the club's went `0x808000BB` → `0x808004E4`),
but names and values resolved correctly, because nothing caches them.

**Game-exit stop verified** the same session: closing the game while
`stats --live` ran logged `Game process exited (code 0) -- stopping`,
with 751 of 751 reads OK.

**Coordinate space:** coordinates are one
shared space across map sections. Position is continuous across the
m33 → m31 border (see "Current area and last bonfire").

## Current area and last bonfire — live-verified (relaunch check pending)

Built 2026-10-06. `stats`, the companion window and the
overlay show the current **area** (named sub-area) and the **last bonfire
rested at**. `stats` also shows the map section id.

**Where they come from.** The offsets and their meanings come from
[The Grand Archives' DS3 Cheat Engine table](https://github.com/The-Grand-Archives/Dark-Souls-III-CT-TGA),
used as a source of facts only (no licence; see `data/THIRD_PARTY_NOTICES.md`):

| Value | Chain | Format |
|---|---|---|
| Map section | `[WorldChrMan+0x80] + 0x1FE0` | 4 bytes = `mAA_BB_CC_DD`, AA in the top byte |
| Play region (named sub-area) | `[WorldChrMan+0x80] + 0x1ABC` | i32 |
| Last bonfire | `[GameMan] + 0xACC` | i32 bonfire entity id |

`GameMan` is a new root, found by the table's byte pattern with the same
RIP-relative resolution as `WorldChrMan`. The table's `GameDataMan`
pattern is byte-for-byte this project's BaseA pattern, so the two
sources agree. The two player-side values hang off the same player
pointer (`s1`) that live modes already re-check every tick, so they
follow reloads automatically.

**Verified live** with `stats --live` during one walk:

| Time | What the user did | Map | Region | Last bonfire |
|---|---|---|---|---|
| 13:32 | Start, in Firelink Shrine | m40 | 400102 Firelink Shrine | 4002950 Firelink Shrine |
| 13:33:43 | Walked through the gate to the cemetery (user saw the banner there) | m40 | → 400100 Cemetery of Ash – Iudex Gundyr | |
| 13:33:51 | Rested at the bonfire there | | | → 4002952 Iudex Gundyr |
| 13:34:01 | Walked back through the gate | | → 400102 | |
| 13:34:23 | Rested at Firelink to warp | | | → 4002950 |
| 13:34:37 | Warped (unload and reload handled) | → m33 | → 330020 Crucifixion Woods – Road of Sacrifices | → 3302956 Road of Sacrifices |
| 13:34:47 | Walked toward Undead Settlement | → m31 | → 310021 Undead Settlement – Before Road of Sacrifices | |
| 13:35:05 | Rode the elevator up (y −206 → −184) | | 0 while riding | |
| 13:35:19 | Top of the elevator (user saw the banner there) | | → 310002 Undead Settlement – Cliff Underside | |
| 13:35:58 | Walked back to the bonfire | → m33 | → 330020 | |

**What this showed:**

- **The banner follows the play region, not the map section.** The map
  switched to m31 at the bottom near the bonfire, but the banner came at
  the top of the elevator, as region 310002 appeared. So the tool shows
  the region as the area name.
- **Region 0 means "between regions"** (elevators, mid-load). The
  displays keep the last real region during those moments.
- **Coordinates are one shared space across map sections.** At the
  m33 → m31 crossing, position went (−4.42, −213.48, −678.84) →
  (−4.56, −213.38, −679.44), with no jump.
- **Last bonfire changes exactly on resting**, including the rest you
  make to warp, and holds while you walk.

**Names** come from the game's own text, read from your install:
bonfire names from `BonfireWarpParam` and
the menu text (`generated/bonfire_names.tsv`), each play region's area
from the place names its map floors carry (`data/region_areas.tsv`,
`generated/regions.tsv`). Every id seen in the walk resolved to a name
matching where the user was.

**Relaunch-verified (2026-10-06):** after a full game restart (new
process, PID 5996) with no code changes, area, map section and last
bonfire all resolved. The user confirmed "Irithyll – Tower of Yorshka"
and "Distant Manor" (their last bonfire) were right.

**Also fixed:** `build.ps1` now compiles with `/utf-8`. Without it MSVC
read the UTF-8 source as Windows-1252, and the companion window showed
"facing −166Â°".

## Areas and session stats — live-verified

Built 2026-10-06.

**Areas.** `data/region_areas.tsv` groups the play regions into the 22
areas players talk about, each a place name from the game's own text. It
is written from the game's own files by `tools/extract_treasures.py
--region-areas`; the PvP arenas have no area.
(A recommended soul level per area was shown here until 2026-10-09; it
was removed, user's call)

**Session stats** (companion window; summary printed when `stats` or the
window exits): session time, deaths, souls per hour, souls lost and
recovered, time in the current area, and a per-area time breakdown at
exit. The session counts only while the tool is polling with a
character loaded, so F10 pauses, menus and loading screens don't count.

Two game counters from The Grand Archives' table, on `GameDataMan` (this
project's BaseA root):

- `+0x98` **Death Num**: verified, it went 31 → 32 on one death.
- `+0xA4` **Play Time**: verified as milliseconds. It rose 212,871
  across about 220s of wall-clock time; the game's clock pauses during
  death screens and loading.

The table's `+0x94` "True Death Num" reads 0 on this character, so it
isn't the normal death count, and the tool doesn't use it.

The soul bookkeeping handles three cases:

- **Death:** a death loses the souls held just before they dropped to
  0. The drop and the death counter can land on different ticks, in
  either order.
- **Bloodstain:** souls returning by exactly the last loss count as
  recovered, not gained.
- **Spending:** any other drop in souls (levelling, buying) is
  spending, not a loss.

**Verified live:** the user killed a few enemies, then died on purpose
holding 2,279 souls (their count), then recovered the bloodstain. The log
showed `Died: lost 2279 souls.` and then
`Bloodstain recovered: 2279 souls.` The window showed 1 death,
"2279 souls (2279 recovered)", and 18,149 souls/h (959 souls gained in
3:10, about 18,170/h). Respawning after death unloads and reloads the
character mid-session, and the stats carried through it.

## Session recording and the results page

Built 2026-10-06.

**Total deaths across the playthrough.** The game's own death counter
(`[GameDataMan]+0x98`) is shown beside the session's count:
"1 this session (33 total)" in the companion window, `Deaths=1/33total`
in `stats`.

**Every live session is saved.** `stats --live`, `window` and `overlay`
write `sessions/<character>/<YYYYMMDD-HHMMSS>.jsonl`. It's JSON Lines,
flushed per line so a crash loses nothing. Sessions under a minute
aren't saved, so quick checks don't leave clutter. `sessions/` and
`reports/` are git-ignored: they're personal play data.

| Event | Fields |
|---|---|
| `start` | character, level, deathsTotal, playTimeMs, startedAt |
| `enter` | area (on every area change) |
| `death` | area, x, y, z, soulsLost, deathsTotal |
| `bloodstain` | souls recovered |
| `level` | from, to |
| `sample` | every 30s: level, souls, deathsTotal, area, playTimeMs |
| `end` | seconds, deaths, gained, lost, recovered, level, deathsTotal, playTimeMs |

**Perf figures (Milestone 5 stress test, 2026-10-07).** Beside each
`.jsonl`, `overlay` and `stats --live` write `<stamp>.perf.csv`: one row
per 5-second perf window (and the part-window before an F10 pause), with
`time, session_t, window_s, ticks, rate_hz, latency_avg_us,
latency_max_us, cpu_pct_core, working_set_mb, private_mb`. It opens with
the `.jsonl`, so a session too short to save leaves no CSV either.
`report` reads it (rows that aren't ten valid fields are skipped) and
the page's **Tool performance** card charts it per session: poll rate,
read latency (average and worst), CPU and memory (working set and
private), as four small charts on their own axes, from zero, with gaps
over 15 s (pauses, game closed) breaking the line. Above them, one line
sums up the session for the stress test: poll rate average and lowest,
latency average and worst, CPU, and memory first → last with the drift
from the first to the last quarter. The session picker lists the
longest first. Checked with a synthetic 2-hour CSV (a pause, a latency
spike, slow memory creep, a cut-off row) rendered in headless Edge in
both themes; **not yet run in a real session.**

The character name (`PlayerGameData + 0x88`, UTF-16, from The Grand
Archives' table) keeps characters apart, and will let a co-op partner's
sessions merge in later. It read "Little John" live.

**`report`** builds `reports/results.html` from `templates/results_template.html`
plus every saved session, then opens it. It shows:

- headline tiles: time recorded, deaths (session and total), souls lost
  and recovered, souls gained per hour, deadliest area
- time per area and deaths per area (bars)
- level over the game's own play-time clock, so it spans sessions
- a death map per area: positions from above, equal x/z scales, with a
  scale bar
- the guide's own performance per session (see "Perf figures" above)
- a sessions table
- a player picker once more than one character is recorded

It's inline JS/SVG with no network, in light and dark themes, with hover
tooltips. Colours and chart rules follow the dataviz reference palette.

**Verified:** a real 2-minute session (Road of Sacrifices, warp to
Irithyll, one death, bloodstain recovered) was saved with all 11 events.
The page showed matching figures: 2m 05s, 1 death (33 total), 2,279 lost
and recovered, time split Road of Sacrifices 1m 28s / Irithyll 0m 37s,
and the death on the Irithyll map. It was checked by rendering in
headless Edge in both themes.

**Bugs found and fixed on the way:**

- **Crash on closing the window.** The window/overlay crashed with exit
  code 139 after the session summary was added: the summary locked the
  shared snapshot after its lock had been destroyed. It now reads the
  snapshot before cleanup. Verified by closing the window and seeing a
  clean "Closed.".
- **Template token replaced in the wrong place.** The page's header
  comment also contained the data token, and the first occurrence got
  replaced, so the page rendered empty. The tool now replaces the last
  occurrence, and the comment no longer contains the token.

**3D area map** (added the same day, replacing a flat "death map" of dots
on a blank background). Sessions also record:

- `pos` (x, y, z): once you've moved 1 unit, at most twice a second,
  roughly 100–200 KB per hour
- `bonfire` (name + position): when the last rested bonfire changes

The placeholder (0, 1, 0) an unloaded character reads is skipped.

The page draws each area's walked trail in 3D on a plain canvas, with no
libraries. Paths are coloured by height (a sequential blue ramp, chosen
separately for light and dark), with deaths and bonfire rests on top and
hover tooltips. A trail is split into separate runs at area changes and
at jumps (warps and respawns: more than 25 units or 10 seconds).

After a warp the position moves a few seconds before the area name, so
a run that starts with a jump and is followed by an area change within
10 seconds is filed under the new area. Without this, every warp left
one point in the old area (e.g. the Firelink bonfire on High Wall's map),
which doubled that map's extent and put its centre in empty space. Fixed
2026-10-07; it applies to existing logs too.

Views and controls:

- It starts **top-down**, where an orthographic view makes distances
  true, with a scale bar.
- The controls follow the common browser 3D convention (three.js
  OrbitControls): **drag** to orbit, **right-drag or Shift+drag** to
  pan, **scroll** to zoom toward the cursor, **double-click** or the
  "Top-down" button to reset. Orbiting pivots on the centre of the
  drawn area.
- A **height slider** (×1 to ×10) on the map's right edge exaggerates
  climbs, which look flat at true scale when tilted.
- **Panning is limited** to the drawn area: the view's centre stays
  within 40 px of the area's box, and a box smaller than the view stays
  wholly on screen (user's request, 2026-10-07).

Checked by rendering a synthetic session (a spiral staircase, ramps,
deaths and a bonfire) in headless Edge, top-down and tilted, in both
themes. Then tried by hand on real data (2026-10-07: Firelink, High
Wall): orbit, pan, zoom, reset, the height slider and the panning limit
all work.

**Co-op note (for the co-op work):** The Grand Archives' table also
lists other connected players' names at
`WorldChrMan → +0x38 / +0x70 / +0xA8 … → +0x1FA0 → +0x88`. Seamless
Co-op partners are characters in your own game's memory, so a partner's
name, HP and so on may be readable from one PC, without a network link.

## Event flags — live-verified (in progress)

Built 2026-10-06. DS3 records progress as numbered on/off **event
flags**: item pickups, boss kills, bonfires lit, doors and shortcuts,
quest steps.

**Method** from SoulSplitter's DS3 `ReadEventFlag`
([FrankvdStam/SoulSplitter](https://github.com/FrankvdStam/SoulSplitter),
GPL-3.0). It was reimplemented from that description with no code
copied; the byte patterns for `SprjEventFlagMan` and `FieldArea` are our
own (see "Memory roots — our own byte patterns"). A flag id splits into digits:

- **group:** `id / 10,000,000`, which picks a flag table
- **area:** `id / 100,000 % 100`, the world area
- **block:** `id / 10,000 % 10`, a block within the area
- **chunk:** `id / 1,000 % 10`, one of ten 1,000-flag chunks
- **bit:** `id % 1,000`

Area flags look up their block in `FieldArea`'s world-info table. The
flag word is then at
`[[[EventFlagMan+0x218] + group*0x18] + chunk*0x10 + category*0xA8] + (bit>>5)*4`.

One layout detail had to be corrected live: the world-info owner's
`+0x10` holds a **pointer** to the area array (pointing at `owner+0x30`),
not the array itself. Before that fix every flag read as unreadable.

**Verified live** on a new game, with `flagwatch` watching 1.62 million
flags while the user picked up the Ashen Estus Flask, lit the Cemetery
of Ash bonfire, picked up a Titanite Shard, killed Iudex Gundyr, and lit
his arena's bonfire:

- **`14000800` flipped off → ON the moment Iudex died.** That's his id
  in SoulSplitter's boss list, so both the bit lookup and the id
  reconstruction are right.
- The single-flag reader agrees: `14000800` reads ON, and `13000800`
  (Vordt, not yet fought) reads off.
- Other flips line up by timing, but each still needs pinning to a
  cause:
  - `14000130`: first bonfire lit
  - `54000050` (the `5xxxxxxx` range) and `50002180`: item pickups or
    rewards
  - `14000002`: second bonfire
  - `24005xxx`: probably enemies or triggers
- Some chunks are reachable under two ids: Iudex also showed as
  `4100800`, and it reads ON too. `flagwatch` now reports each chunk
  once, under the higher group, which is the form the boss ids use.

**Flag ids come from the game's own tables** (`paramsearch`, same
session). Searching the live tables for the flags that flipped tied each
one to its cause, in the user's order of actions:

| Action | Flag | Flipped | Game table |
|---|---|---|---|
| lit the Cemetery of Ash bonfire | `14000001` | 15:59:12 | `BonfireWarpParam` row 2, `LocationEventId` (`+0x00`) |
| picked up the Titanite Shard | `54000050` | 15:59:30 | `ItemLotParam` row 4000050, `getItemFlagId` (`+0x80`), item goods 1000 = Titanite Shard |
| killed Iudex Gundyr | `14000800` + `50002180` | 16:00:47 | boss flag + `ItemLotParam` row 2180 = goods 2137 Coiled Sword (his drop) |
| lit Iudex's arena bonfire | `14000002` | 16:00:50 | `BonfireWarpParam` row 3, `LocationEventId` |

So **item pickup flags** are `ItemLotParam.getItemFlagId` (2,790 lots,
each naming its items, which `item_names.tsv` resolves). **Bonfire-lit
flags** are `BonfireWarpParam.LocationEventId` (82 bonfires). Their
`WarpEventId` (`4001951`, `4001952`) is the last-bonfire id minus 1000,
which links each row to `bonfire_names.tsv`. **Boss flags** are
SoulSplitter's 25 ids. No third-party flag list is needed for items or
bonfires. Layouts are from Paramdex (`ITEMLOT_PARAM_ST`,
`BONFIRE_WARP_PARAM_ST`). `14000130` (which flipped near the first
bonfire) is in neither table, so it's some other event.

**Relaunch-verified (2026-10-06):** after a full game restart (new
process, PID 32244) with no code changes, all five flags from the test
still read ON: `14000800`, `54000050`, `50002180`, `14000001`,
`14000002`. Vordt's `13000800` still reads off. Both byte patterns
resolved to the same module offsets, and the flags persist as save data.
**Door flag (2026-10-06):** opening the door out of Iudex Gundyr's arena
flipped exactly one flag, `64000260` (area 40), with nothing else
changing; it reads ON afterwards. Doors and shortcuts are ordinary event
flags too, in the `6xxxxxxx` range here.

## Unfound items per map section — live-verified (list form)

Built 2026-10-06. `items` lists the current map section's one-time world
pickups as found or not found, with item names. The companion window
shows "Items 2 / 33 found in this map section", and the overlay
"Items 2 / 33 found here", refreshed once a second.

**Source: the game's own tables.**

- **Which rows:** world pickups are the `ItemLotParam` rows whose
  `getItemFlagId` reads `5AAxxxxx`, AA = the map section. Enemy drops
  repeat and have no flag; boss rewards sit in the global `500xxxxx`
  range.
- **Found:** a pickup is found when its flag is ON.
- **Names:** each lot's items (`ItemLotId1-8` + category: weapon,
  armor `0x10000000`, ring `0x20000000`, goods `0x40000000`) resolve
  through `item_names.tsv`.

**Grouping rules, from what the data showed on m40:**

- **Sets:** lots sharing a flag are one pickup. One corpse gives the
  whole Pale Shade set: lots 4000140–143, flag 54000140.
- **New Game+:** lots 200,000,000 above a base lot share its flag but
  give upgraded items. They're the NG+ version, chosen by the journey
  (`GameDataMan + 0x78`, The Grand Archives' "ClearCount").
- **+N rings:** pickups of only +1/+2/+3 rings are hidden on the first
  journey. This is a written rule, not game data: the lot's own
  ClearCount byte (`+0x94`) reads `0xFF` ("any") even for NG+-only
  rings like Life Ring+3.

The result for m40 (Firelink Shrine, Cemetery of Ash, Untended Graves)
on journey 1 is 33 pickups.

**Verified live (new game):**

- The Titanite Shard picked up earlier showed as found.
- The user then picked up the item just past the door out of Iudex's
  arena. The count went 1 → 2 within a second, and `items` named it
  Broken Straight Sword.
- The user remembered it as a Homeward Bone, so the inventory was
  checked: it held a Broken Straight Sword and no Homeward Bone. The tool
  was right.

**Known gaps:**

- Items the game gives through scripted events rather than item lots
  aren't counted; the Ashen Estus Flask is one.
- Lot 4010 (flag 54004010) gives an item id (10080) with no name in the
  table.
- A map section can span several named areas (m33 covers Road of
  Sacrifices and Farron Keep).
- No positions yet: those come with the map files, which
  turns this into the nearby-items list.

## Bosses and shard progress — live-checked

Built 2026-10-06.

**Bosses:** `data/bosses.tsv` lists each boss's event flag, area,
whether it's required or optional, and a note. The flags are
SoulSplitter's ids (`14000800` verified flipping live). Names are the
ones on the bosses' health bars, read from your install
(`generated/boss_names.tsv`); areas are where the game's map files place
them; required / optional follows our own route.
DLC bosses are required or optional within their DLC. Defeated = the
flag is ON.
`progress` groups bosses by area. The companion window lists the current
area's bosses, and the overlay shows "Bosses X / Y defeated here".

**Shards:** Estus Shards (goods 2141) and Undead Bone Shards (goods
2143) found out of the total. Pickups are counted from the world-pickup
data across every map section (m30–m55), with the same
grouping and journey rules; found = the flag is ON.

**Checked on the new game:**

- Iudex Gundyr reads defeated, every other boss alive.
- Base game: 1 of 19 defeated, 12 required left.
- Shards: Estus 0 / 11, Undead Bone 0 / 10. Those totals came straight
  out of the item table, with no number entered by hand, and they match
  the well-known DS3 totals (11 Estus Shards, 10 Undead Bone Shards).

Only world pickups count towards shards; one handed over by an NPC
wouldn't appear in the total. (The totals matching suggests none are.)

## Key items — built, checked on a new game

Built 2026-10-06; rebuilt 2026-10-09 from the game's data. `data/key_items.tsv` holds the game's own key-item category
(66 rows, 62 items) plus Loretta's Bone, with our own kinds and short
descriptions; names are the game's, read from your install. Cinders of a
Lord is four ids, one per Lord; each name is shown once.

Each item has one of three states:

- **held:** in the inventory now
- **obtained:** not held, but a world pickup that gives it has its flag
  ON (used up; Cinders of a Lord leave the inventory once placed)
- **not yet**

Key items given by NPCs or bosses have no world pickup, so only "held"
can be known for them. Where an item is a world pickup, its map section
comes from the pickup data. Those all landed where the
items really are:

- Cell Key m30 (High Wall)
- Loretta's Bone m31 (Undead Settlement)
- Old Cell Key, Jailbreaker's Key, Jailer's Key Ring m39 (Irithyll
  Dungeon)
- Small Envoy Banner m50 (The Dreg Heap)
- Carthus / Izalith / Quelana / Grave Warden tomes m38
- Crystal Scroll m34 (Grand Archives)

On the new game: 0 of 35 obtained, which is correct. The held and
obtained states haven't yet been seen turning on live; they need a key
item picked up. Held detection uses the same inventory walk as
`inventory`; for example the held Titanite Shard reads as goods 1000.

## Upgrade tracker — built, checked against the game's data

Built 2026-10-06. For each equipped weapon (`upgrades`), and for the
active right-hand weapon in the companion window, it shows the next
level's materials against what you hold.

**How it works**, all from the game's own tables:

- **Material row:** the next level's material set is row
  `EquipParamWeapon.materialSetId` (`+0x58`) plus
  `ReinforceParamWeapon[reinforceTypeId + nextLevel].materialSetId`
  (`+0x56`, u8) of `EquipMtrlSetParam`. That table holds five material
  ids at `+0x00` and their counts at `+0x14` (Paramdex
  `EQUIP_MTRL_SET_PARAM_ST`).
- **Held:** goods quantities summed from the inventory.
- **Max level:** there's no upgrade row for the next level.

**Checked:**

- **The data:** a Long Sword has material set 0, and its level offsets
  are 1..10, the level itself. Rows 1–10 read Titanite Shard ×2/×4/×6,
  Large Titanite Shard ×2/×4/×6, Titanite Chunk ×2/×4/×6, then Titanite
  Slab ×1.
- **An outside source:** the shard counts match the Fextralife Titanite
  Shard page ("+1: 2 shards, +2: 4 shards, +3: 6 shards"). My own guess
  had been 1/2/3, which is why it was checked.
- **The new game:** Long Sword and Knight Shield both read
  "+0 -> +1: Titanite Shard 1/2". The 1 held is the shard picked up in
  the Cemetery.

**Not yet checked:** special weapons (boss weapons, Twinkling Titanite
or Titanite Scale weapons). They take the same formula through their own
upgrade types, but none was equipped. Soul costs aren't shown.

## Item positions from the map files — live spot-checked

Built 2026-10-06. Every world pickup's position comes from the game's own
map files, read from **your own install**:

```
python -I tools\extract_treasures.py ["<DS3 Game folder>"]
```

This writes `data/generated/treasures.tsv` (map, lot, lot2, x, y, z,
part). The file is git-ignored and never committed, because it is game
data. Run it once per install; it takes a few seconds.

**How it reads the files.** The game folder stays packed. Nothing is
unpacked to disk or patched, and the archives are only opened for reading.

- `tools/ds3_archive.py` decrypts the `Data5` / `DLC1` / `DLC2` `.bhd`
  headers with the game's public RSA keys. It looks up
  `/map/mapstudio/mXX_XX_00_00.msb.dcx` by name hash
  (`h = h*37 + c`, lower case) and decrypts the file's AES-128-ECB ranges
  using Windows' own bcrypt.dll. It then inflates the DCX (DFLT = zlib).
  The format facts come from BinderTool (Atvaark, MIT).
- `tools/extract_treasures.py` parses the MSB3. Each `Treasure` event
  (type 4) names an ItemLotParam row and a map part, and the part's
  position is the pickup's. The layout facts come from SoulsFormats
  (GPL-3.0); it is reimplemented here and no code is copied.
- DLC archives win over Data5 (m47 is in both).
- The result is 1,005 treasures over 18 map sections. Kiln (m41), m46,
  m47 and m51_01 have none.

**In the guide:**

- Lot → pickup flag and item names come from the live ItemLotParam, as
  for the unfound-items list.
- An NG+ lot (≥ 200,000,000) takes the position of its base lot.
- The coordinates are the same space as the player's, so no conversion
  is needed.

**Commands and window:**

- `nearby [N]` lists the closest unfound pickups with distance, clock
  direction (12 = ahead) and height difference.
- `nearby --watch` logs how far you stood from each item you pick up.
- The companion window has a **Nearest** line ("Ember  39 m, 3 o'clock,
  +24 m"), updated once a second.

**Checked live (new game, Firelink).** Distance between where you stood
on pickup and the item's map position:

| Pickup | Distance |
|---|---|
| Homeward Bone (lot 4000060) | 1.5 m |
| Ember (4000070) | 0.9 m |
| East-West Shield (4000090) | 1.4 m |
| Ember (4000120) | 0.7 m |

That's a mean of 1.1 m. All four were found by following the Nearest
line. The clock direction was first rotated 180°: the facing angle
points opposite to `atan2(dx, dz)`. It was fixed and confirmed in game.

**Still open:**

- 6 more spot-checks, which the task asks for (10)
- items in other areas and in the DLCs
- pickups with no MSB treasure (scripted, NPC) have no position and are
  left out of `nearby`

## Spoiler tiers — live-verified

Every hint follows one **spoiler tier**. The ladder and the default were
chosen by the user on 2026-10-06:

| Tier | Nearest pickup | Bosses in this area | `items` / `keys` (unfound) |
|---|---|---|---|
| **Off** | hidden | "2 bosses here: 1 defeated (Vordt), 1 not yet" | count only |
| **Vague** (default) | "something unfound, 40 m, 3 o'clock" | same | count only |
| **Category** | "a ring, 40 m, 3 o'clock, +24 m" | same | `items`: the kind of thing; `keys`: count only |
| **Full** | "Chloranthy Ring, 40 m, 3 o'clock, +24 m" | every name | names |

The kinds come from the item tables:

- weapon or shield
- armour (with the number of pieces for a set)
- ring
- spell
- upgrade material (titanite, gems)
- flask upgrade (Estus / Undead Bone Shard)
- key item (`data/key_items.tsv`)
- souls
- gesture
- consumable

Things you already have are never hidden: found items, obtained key
items, and defeated bosses. Defeated bosses are named at every tier,
because you've already seen them (the user's call). Counts (Items,
Shards, Keys) and the upgrade line show at every tier. Below Full,
`progress` lists only the areas where you've defeated a boss.

**Checked live, 2026-10-06:**

- F9 cycled Off → Vague → Category → Full in game.
- The Nearest line and the overlay's last line changed at each tier.
- The Bosses line changed in the Cemetery of Ash (Iudex Gundyr).
- The saved tier came back after a game restart.

**Changing it:**

- **F9** in window or overlay mode cycles Off → Vague → Category → Full.
  It is saved to `settings.ini` (repo root, git-ignored).
- The overlay shows the tier with the nearest pickup, for example
  "[Vague] 40 m 3 o'clock: something unfound". The companion window has
  a "Hints" line.
- Commands (`nearby`, `items`, `keys`, `progress`) follow the saved tier.
  Add `--full` to see everything for one run.

## Boss resistances and weaknesses — live-verified

Built 2026-10-06. The stats are read live from the game's `NpcParam`
table. Each boss's row is found through the map files:

- `tools/extract_treasures.py` also writes `data/generated/enemies.tsv`
  (every placed enemy: entity id, NpcParam id, position).
- A boss's enemy part has `EntityID = defeat flag − 10,000,000`
  (Iudex Gundyr 14000800 → entity 4000800 → NpcParam 511000). That
  covers 23 of 25 bosses.
- Abyss Watchers (c3040, entity 3300801) and Dancer of the Boreal Valley
  (c5270, entity 3000899) were found by model and arena, and are listed
  in code.

**Fields** (Paramdex `NPC_PARAM_ST`):

- `+0x19C`: eight damage rates for physical, slash, strike, thrust, magic,
  fire, lightning and dark. Absorption = 1 − rate. Paramdex calls them
  `regainRate_*`.
- `+0x104..0x10A`: poison / toxic / bleed / curse resistance.
- `+0x1D8`: frost resistance. 999 = immune.

**Checked against the Fextralife boss pages.** All eight absorptions
match exactly for:

| Boss | Physical | Strike | Slash | Thrust | Magic | Fire | Lightning | Dark |
|---|---|---|---|---|---|---|---|---|
| Iudex Gundyr | 15% | 12% | 18% | 16% | 5% | 2% | −14% | 38% |
| Vordt | 35% | 18% | 38% | 36% | 24% | 18% | 15% | −27% |
| Abyss Watchers | 15% | 14% | 18% | 16% | 10% | 8% | −12% | 35% |

The status resistances agree too: Iudex bleed 200 and frost 63, Vordt
immune, Abyss Watchers' bleed 542 ("Resistant").

**Not shown:**

- **HP.** The row holds the base value before the game's area scaling
  (Vordt 1,190 vs 1,328 in game).
- **Champion's Gravetender and Halflight.** They are human-type enemies
  (c0000) with a blank row; their defences come from equipped gear,
  which isn't read. They're labelled as such.
- **Multi-phase / multi-body bosses.** These use the single row behind
  the defeat flag's entity.

**Where it shows:**

- `resist [--full]` prints a table of absorptions and statuses, at
  spoiler tier Full only (or with `--full`), defeated bosses included.
- The overlay's boss line covers the nearest boss still to fight in the
  current area (never a "later" boss, see below): weakness, strongest
  resistance, immunities and status numbers, with the name. Full only.

  It wraps onto up to three lines. Checked live on a new game at Iudex
  Gundyr.

**Full only since 2026-10-07 (user's call).** Before that, Vague showed
the weakness and Category the numbers without the name. Below Full the
server now sends no boss numbers at all, so the page can't show them.

**Bosses for later (2026-10-07, user's request).** A boss is "later"
when it's undefeated and its step in `data/route.tsv` isn't open yet:
the normal route doesn't take you there until other bosses are dead
(the Dancer after three Lords of Cinder, the Dragonslayer Armour after
the Dancer, the DLC after its entry boss...). `BossIsLater` in
`main.cpp` reads this from the route's `needs`. Such a boss:

- is left out of "the boss ahead" (page chart, overlay line) and out of
  the "here" counts, which gain "(+N later)"
- shows dimmed on the page with a **Later** tag; hovering says "Comes
  later in the game", or at Full what opens it ("Opens after 3 of Abyss
  Watchers / Yhorm the Giant / ... (0 so far)")
- reads "later: opens after ..." in `progress`

Known gap: the early Dancer (killing Emma) has no flag here, so she
stays "later" until three Lords are dead. Checked live in High Wall
after Vordt: `1 / 1 here (+1 later)`, the Dancer dimmed, no chart.

## Unfound items per named area — live-verified

Built 2026-10-06. Each pickup's named area comes from the map files, the
same way the game decides the player's:

1. The treasure's part is an object (type 1) or enemy (type 2). Its
   `CollisionPartIndex` (object type data `+0x08`, enemy `+0x1C`, an
   index into the whole PARTS list) names the collision it stands on.
2. The collision's `PlayRegionID` (type data `+0x38`) is the play region.
   `data/region_areas.tsv` turns that into an area.
3. **m40 holds two worlds on the same geometry:** Cemetery of Ash /
   Firelink Shrine and Untended Graves / Dark Firelink. Its collisions
   only carry the Untended ids (4000xx). The part's `MapStudioLayer`
   (`+0x48`) bit 0 tells them apart.
   - Bit 0 clear means Untended only. This matched all 11 Untended items
     the wiki names, and all 5 items picked up in normal Firelink have
     it set.
   - Normal-world parts get the matching 4001xx id (400001 ↔ 400101
     Cemetery, 400002 ↔ 400102 Firelink).
4. Parts with no collision link (some objects, dummy objects/enemies)
   take the region of the nearest linked part in the same world.
5. Three region ids used by the map files were missing from the name
   table and were added to `region_areas.tsv`: 301011 Lothric Castle,
   320001 Archdragon Peak, 341011 Grand Archives.

All 1,005 positioned pickups resolve to an area. The extractor writes the
region as a new `treasures.tsv` column, so re-run
`tools\extract_treasures.py` after updating.

**Where it shows:**

- The window's Items line: "0 / 4 found in Cemetery of Ash".
- The overlay's "Items X / Y found here".
- `items` (per area by default; `--map NN` for a whole map section).

Each falls back to the map section when an area has no positioned
pickups. Live-checked: the count switched when walking from the Cemetery
of Ash into Firelink Shrine.

**Two `nearby` fixes found on the way:**

- **Untended Graves items in the normal world.** They overlap the
  Cemetery and Firelink in space. Each world's pickups now only show
  while you're in that world.
- **Items from other map sections.** Map coordinate spaces overlap in
  number: m30 High Wall reaches z 499, m40 Cemetery spans z 445–645.
  High Wall's Titanite Scales showed as 73 m away in the Cemetery.
  `nearby` and Nearest now use only your current map section. The cost:
  items just across a connected border (e.g. m33 → m31) don't show until
  you cross it.

## Live page — a second-monitor dashboard (overlay mode)

Built 2026-10-06 at the user's request. It replaces the companion window
and trims the overlay down to the critical lines.

**How it runs:**

- `overlay` starts a small HTTP server in the same process, bound to
  **127.0.0.1 only** (port 8765, or the next free one up to 8774). It
  opens the page in the default browser.
- The server rejects requests whose `Host` header isn't
  localhost/127.0.0.1, as a DNS-rebinding guard.
- It serves:
  - `templates/live.html` (read from disk on every request, so edits
    show on refresh)
  - `/api/state`: one JSON snapshot. The page polls it twice a second
    and gets only new trail points each time.
  - `POST /api/tier?t=0..3`: the spoiler tier, the same setting as F9.
  - `POST /api/page?theme=a|b|c&muted=...`: the page's theme and muted
    panels (saved to `settings.ini`).
- Nothing on the page can touch the game. The guide still only reads.
- The polling worker **waits for the game**:
  - It lists processes every 2 seconds, then attaches through
    `AttachReadOnly`, which still refuses any DS3 without the Seamless
    Co-op module.
  - When the game closes it logs that run's session summary and waits
    again. Each run starts a fresh session and map trail.

**The page** is one screen with no page scroll at 1280×720 and up. It
comes in **three themes** from the dashboard design, switched in the header
with no state lost:

- **Bonfire** (default): warm and serif. The boss panel leads with the
  weakness; items are a compass rail.
- **Ledger:** cool and data-dense. A vitals strip, items as a table, and
  absorption as a heat strip.
- **Cartographer:** the map fills the screen and the panels float over
  it. Follow centres you in the part the cards don't cover.

Below 1280×720 every theme falls back to one scrolling column (Character,
Bosses, Nearby, Route, Map, Gear). The theme and the muted panels are
tool settings, saved to `settings.ini` as `page_theme` and `page_muted`
through `POST /api/page?theme=a|b|c&muted=char,map,...`. Every panel has
an eye button; a muted panel only shows its name, e.g. "(Map)". Which
Route / Missables tab is open is remembered per browser.

- **Character:** HP / FP / stamina, equip load with light / medium /
  heavy ticks and roll type (warning colour when overloaded), level,
  souls, next level's cost ("Enough souls to level up" when you can),
  spell, quick item and attributes. The level planner shows what +1
  gives under each attribute (hover for every weapon) and a "Next
  point" line. Cartographer has no room for the per-attribute line, so
  it's in the hover there.
- **Map:** this session's path from above (current map and world only,
  since map coordinate spaces overlap), deaths, bonfire rests, you with
  your facing, and numbered unfound items from tier Category up. Drag to
  pan (turns Follow off), scroll or + / − to zoom, Follow re-centres.
  Hovering an item row or its pin highlights both.
- **Bosses:** at the tier:
  - Off and Vague: the count and the defeated bosses
  - Category: the list with generic names ("Required boss"), no numbers
  - Full: real names, plus the absorption chart and status resistances
    of the closest boss still to fight (never a defeated or "later" one;
    with none left: "No bosses left to fight here")

  Defeated bosses are always named. Undefeated ones show their distance;
  "later" bosses are dimmed (see "Bosses for later"). A human-type boss
  says its defences come from its gear, which isn't read yet.
- **Gear:** the six weapon slots with attack rating at current stats
  (two-handed when gripped so) and which one is in hand. The in-hand
  weapon's next upgrade is shown as materials held / needed. A ▲ marks a
  weapon with a better infusion at your stats (hover for the top three).
- **Nearby items:** every positioned pickup in the current named area.
  The list scrolls inside its panel.
  - Off and Vague: a count only (user's call, 2026-10-06; the API sends
    no labels at those tiers)
  - Category: the kind of item, with arrow, distance, clock direction
    and height
  - Full: the name, with the kind beside it
  - Found items are always named, struck through, at the bottom.
- **Route & progress:** the route hint and step chips (done / open /
  locked, "+N not shown at this tier") over seven progress tiles:
  deaths, session time, souls per hour, bosses, items here, Estus
  shards and key items. A **Missables** tab in the same panel holds the
  missable list with its Done / Don't care marks. While a missable is
  pending in your area, a "Missable here" line sits above the route
  hint.

**Overlay settings from the page.** The **Overlay ▾** button in the
header opens a panel with these settings, each applied to the overlay at
once and saved to `settings.ini`:

- show the overlay (hides it without pausing; F10 still hides *and*
  pauses)
- each of its lines on or off, plus the perf figures (F11 toggles the
  same setting)
- corner of the game window
- opacity (30–100%), text size (11–24 px) and width (240–560 px)

The settings travel as `POST /api/overlay?key=value` and are validated
(out-of-range values are rejected).

**Preview:** while the panel is open, the page renews
`POST /api/preview?on=1` every 1.5 s. The overlay then stays visible over
the game even when the game isn't focused, so changes show live. Closing
the panel or the tab ends it, and the preview lapses 4 s after the
renewals stop.

**Quit button:** a header button that asks for a second click within
3 s, then sends `POST /api/quit`. The guide closes as with Ctrl+C,
saving the session. Checked: it closed in about 0.1 s.

**Opening the page:** at start the guide waits 2.5 s. If a page left
open from an earlier run reconnects in that time, it doesn't open
another tab.

**The overlay** now shows only:

- the spoiler tier with the nearest unfound item (at Vague, only how
  many are left: "[Vague] 8 unfound items here", like the page)
- bosses done / total in this area
- the nearest boss's weaknesses and resistances as far as the tier
  allows

It's 340 px wide; long lines wrap (at " | " breaks first), and the panel
grows to fit them.

**Themes: checked live in game 2026-10-07**, built with MSVC (no
warnings): all three themes, tier changes, item ↔ pin hover, map
trail/drag/zoom/Follow, the popover pinning the overlay, and theme and
mutes surviving a restart. One bug: a rest at the same bonfire as last
time isn't marked. Before that, checked in headless
Chromium against a mock `/api/state`:

- all three themes at every tier, at 1920×1080 and 1280×720
- the one-column fallback at 1000×800
- item ↔ pin hover, muting, theme switching with the popover open, the
  tier buttons and the Missables tab
- the theme and mutes reaching the server and surviving a reload

The C++ changes (the `page_*` settings, `category` at Full, no labels at
Vague, boss `dist`) were syntax-checked with MinGW only, not built with
MSVC.

**Checked (before the themes):**

- Server: the page and JSON are served, a foreign `Host` gets a 403, and
  the tier set from the page reached the overlay and `settings.ini`.
- Waiting for the game, and attaching when it started.
- In Chrome at 1920×953: the page is exactly one screen with no panel
  overflowing. The map mute, the whole-area item list (Firelink Shrine:
  8 unfound + 6 found = the 14 of the Items count) and the chip grid all
  work.

## Route hints — built, checked on a new game

Built 2026-10-06. `data/route.tsv` lists 23 steps in the order the game
opens them: the main path, the optional areas (Smouldering Lake, Consumed
King's Garden, Untended Graves, Archdragon Peak) and the DLC. Each step
has:

- **what unlocks it**, as boss defeat flags. Three Lords of Cinder bring
  the Dancer; Deacons + Wolnir open Irithyll (Small Doll); all four
  Lords open the Kiln; the Dreg Heap needs the four Lords *or* Sister
  Friede.
- **the area its way in starts from**
- **its bosses**
- **the way in**, in our own words

The route is written for this project: one step
per area of the game's own map data, each gated by the boss flags the
data tests check against `bosses.tsv`.

**How the hint is worked out:**

- **Status:** a step is locked, open, or done (all its bosses dead; a
  step without bosses counts as done once open).
- **Visited areas:** this character's saved sessions, the current area,
  areas with a picked-up item, and areas with a dead boss.
- **The suggestion:** the earliest open, unfinished main step (then
  optional, then DLC).

**At each spoiler tier** (user's choice):

| Tier | Hint |
|---|---|
| Off | nothing |
| Vague | only visited areas: "High Wall of Lothric: a required boss is still alive." |
| Category | also "A new area is open beyond Undead Settlement." |
| Full | "Next: High Wall of Lothric -- Warp from the Firelink Shrine bonfire once the Coiled Sword is in it. Boss: Vordt of the Boreal Valley." |

The step list follows the same rules. Locked steps and unvisited areas
only show at Full; otherwise there's a "+N not shown" count.

**Where it shows:**

- the live page's "Route & progress" card (hint + step chips; hover for
  bosses left and, at Full, the way in)
- an overlay "Route:" line, with its own toggle in the Overlay panel
- `route [--full]`

**Checked** on the new character (Iudex dead): Cemetery of Ash and
Firelink Shrine done, High Wall open with Vordt left. The Vague hint
names only High Wall; Full names the way in and the boss.

**Not covered:** the early Dancer (killing Emma) has no tracked flag,
so the route assumes the usual three-Lords way.

## Missable-content warnings — built, engine checked

Built 2026-10-06; rewritten 2026-10-09 from the game's own files.
`data/missables.tsv` holds 16 entries in our own
words:

- **NPC questline steps:** Siegward ×4, Sirris ×2, Greirat ×2, Irina,
  Anri ×2, Yoel ×2.
- **Missable items:** Cuculus against the Old Demon King; the Eyes of a
  Fire Keeper.
- **Ending requirements:** giving the Fire Keeper her eyes.

**Where the entries come from:** each NPC's questline is a set of
global flags, one per step (Siegward 1380-1399, Sirris 1120-1139, ...),
moved by the event scripts. Reading every path through those scripts
gives the exact conditions for each step: talk flags (with the lines
spoken when they're set, from the talk scripts), boss kills, the
"bosses killed since" counters, purchases and items. A missable is a step
that a later event (usually a boss kill) closes off.

**Each entry has three conditions,** in the route's flag syntax:

- **relevant:** when it starts to apply, usually the NPC's step before
  it. For example, Sirris's vow applies from state 1127 (after the
  Hodrick fight).
- **trigger:** when it's too late: a boss kill (Yhorm for Siegward's
  steps), or the NPC state the game moves to (1137, Sirris gone).
- **done:** when the game itself shows it's handled: the talk flag of
  the step, or the next NPC state.

The 4-digit NPC state flags are read as global flags (area 0) by
`ReadEventFlag`; that path still needs a live check. The player can
still mark entries **✓ Done** or **Don't care** on the live page
(`POST /api/missable`); the marks are kept in
`sessions/<character>/missables.txt`.

**Status:**

- pending
- missed (the trigger came first)
- done (by the game or a mark)
- don't care

**At each spoiler tier:**

- **Off:** nothing.
- **Vague:** "Something here can still be missed", only in visited
  areas (missed ones hidden).
- **Category:** the kind of thing ("An NPC questline step"), and whether
  a boss kill locks it.
- **Full:** topic, what to do and what you'd lose.

**Where it shows:**

- the live page's Missables card (pending in this area first)
- an orange overlay alert while you're in the area of a pending entry,
  with its own toggle
- `missables [--full]`

The page grid is now four columns: Character | Map | Bosses | Missables
above Gear | Nearby items (double width) | Route & progress.

**Checked:** no entry applies yet on the new character (only Iudex is
dead). A temporary Firelink test entry exercised the rest: Vague and
Full wording, the overlay alert, marking done and undoing (saved and
reloaded), and an unknown id rejected. The page still fits one screen.

## What to level next — live-checked (live page)

Built 2026-10-06. The Character card shows, under each attribute, what
one more point would give, plus a "next point" line naming the attribute
that helps the weapon in hand most, with the next level's cost.

| Attribute | Gain shown | From |
|---|---|---|
| Vigor | max HP | CalcCorrectGraph row **100** (1/15/27/50/99 → 300/550/1000/1300/1400) |
| Attunement | max FP, + slot if a threshold is crossed | row **101** (1/15/35/60/99 → 50/120/280/350/450); `ComputeBaseAttunementSlots` |
| Endurance | stamina, marked ≈ | no curve found in the params: interpolated from the Fextralife Endurance table |
| Vitality | equip load (× ring multiplier) and the roll tier | `ComputeBaseMaxEquipLoad` |
| Str / Dex / Int / Fth / Luck | AR for the weapon in hand (two-handed when gripped so; spell buff for catalysts; "meets req." when a requirement penalty goes) | `ComputeWeaponAr` rerun with the stat +1, per equipped weapon |

Hover a box for the gain on every equipped weapon.

**Found with `graphs`,** a new diagnostic that lists every
CalcCorrectGraph row live. Rows 100 and 101 are the HP and FP curves.
They reproduce the wiki's tables (e.g. Vigor 12 → 454, 13 → 483, 27 →
1000).

**Scaling:** the gains are multiplied by current max ÷ curve value. That
carries the Ember's +30% and rings. The live HP of 590 / 627 at Vigor
12 / 13 is exactly 1.30 × 454 / 483.

**Best infusion.** The Gear card adds an **Infusion** chip per weapon.
For every infusion row of the weapon (row id = base + 100 × infusion,
0 Normal … 6 Fire … 15 Hollow, same reinforcement level) it runs
`ComputeWeaponAr` at your current stats and ranks them by total AR at
the current grip (spell buff for catalysts). It names the best other
infusion, its gain over the current one, and whether you hold the gem
(goods 1100–1240, checked every 5 s). Hover for the top three.

Totals add split damage (physical + element) together, which defences
cut harder than one damage type, and leave out status build-up; the
hover says so. Weapons that can't be infused (only the +0 row exists)
get no chip.

**Checked:** Long Sword one-handed, Normal 110+14 = 124 vs Fire 93+93 =
186, so +62, the same as `ar --infusions` (whose Fire AR was verified
in game).

**Checked live** (level 10, two-handed Long Sword):

- Vigor +42 HP (curve +32 × 1.30), Attunement +5 FP, Endurance ≈+2,
  Vitality +1.0 load (still medium roll).
- Strength +3 AR, Dexterity +1 AR. Best: Strength.
- The page still fits one screen.

## What's proven vs. still open

- ✅ Read-only process attach, verified against a live Seamless Co-op
  session (not just "should work" — actually run against PID 2856,
  2026-08-17).
- ✅ Live HP read, confirmed against the in-game HUD three times (549,
  474 after taking damage, 634 after regenerating).
- ✅ Live FP ("mana") and Stamina reads, confirmed against the HUD
  alongside HP (FP=24, Stamina=94).
- ✅ Live max and base-max for all three stats, confirmed against the
  character status screen (HP 634/715/base550, FP 24/114, Stamina
  94/94).
- ✅ Live Level and Souls, confirmed against the character status
  screen (Level=32, Souls climbing 13359→17319→20245 across three runs).
- ✅ Live Vigor/Attunement/Endurance/Strength/Dexterity/Intelligence/
  Faith/Luck/Vitality, confirmed against the status screen and by the
  `SoulLevel = AttributesTotal - 89` identity (121 - 89 = 32, matches).
- ✅ Read latency: ~2–50µs per single-value read, ~8–34µs for all
  6-8 per-tick memory reads together, far under the brief's <1ms
  target, at a 10Hz poll rate.
- ✅ Required-souls-for-next-level is computed from a documented public
  formula, not read from memory — cross-checked against the source
  wiki's own table, and now against the actual in-game level-up screen
  too (2026-08-19): Level 47→48 computed as 13435, exact match against
  the user's own status screen reading.
- ✅ **Equipped-item identity (weapons, armor, rings), including
  reinforcement level.** All 14 slots resolved to real, named items in
  one live run — see "Equipped item identity — solved" below for the
  full disassembly-verified chain and the table of confirmed results.
  Chain: same `PlayerGameData` base as CharacterStats,
  `+0x228 (EquipGameData) +0x24 + slot*4`, sourced directly from
  DS3RuntimeScripting's `EquipGameData::getInventoryItemIdBySlot()`.
  Verified live for weapons: swapping R1's weapon twice changed only
  R1's printed value (129→204→220) while all five other slots stayed
  exactly put. Armor/ring/arrow/covenant offsets use the identical
  formula (cross-referenced against DS3RuntimeScripting's
  `InventorySlot` enum) but aren't yet independently live-verified.

  ### Equipped item identity — solved

  Resolving `inventoryItemId` to an actual item (name, upgrade level)
  needs its real param ID (`giveId`). DS3RuntimeScripting resolves this
  by *calling a live function inside the game process*
  (`GetInventoryItem`) — out of bounds for "read-only, always." Getting
  there read-only took two attempts:

  **Attempt 1 (dead end, kept for the record):** a live pointer chain
  found via `scan`/`findptr`/`peek` alone matched *by coincidence* for
  one item (R1's weapon) and failed for every other slot. Investigating
  the failures (see git history) showed the container wasn't a single
  flat array at all — which pointed straight at attempt 2.

  **Attempt 2 (the real fix): read-only offline disassembly.**
  `x64dbg` (live-attach) and portable `radare2` (fully offline, static
  analysis only) were evaluated; `radare2` was chosen specifically
  because it never touches the running game beyond a read-only byte
  capture, matching the project's "read-only, always" rule even for
  investigation tooling, not just the shipped tool:
  1. `dump`/`peek` gained an optional save-to-file argument — still a
     plain `ReadProcessMemory` call, just also written to disk
     byte-for-byte, so a captured function can be disassembled without
     retyping hex by hand (the earlier session's hand-decode error).
  2. Captured 1024 bytes of live code around DS3RuntimeScripting's
     hardcoded `GetInventoryItem` address and disassembled it completely
     offline with a portable copy of [radare2](https://github.com/radareorg/radare2)
     (`radare2 -e scr.color=0 -a x86 -b 64 -c "o file.bin <baseaddr>; s <baseaddr>; pd N"`).
     This immediately explained the earlier hand-decode failure: the
     hardcoded address landed *mid-instruction*, 0x10 bytes into a real
     function's body, not at a function boundary — exactly the kind of
     mistake a real disassembler catches and hand-decoding doesn't.
  3. The properly-aligned code revealed the actual container: **a
     two-segment array**, not one flat array — a `threshold` field
     splits `inventoryItemId` into a low-index segment and a
     high-index segment, each with its own base pointer and 16-byte
     entries (matching DS3RuntimeScripting's `InventoryItem{uniqueId,
     giveId, quantity, unknown1}` struct exactly). This is *why*
     attempt 1 worked for one lucky high index and broke near the
     segment boundary — it was reading across a segment split it didn't
     know existed.
  4. Cross-referencing those exact offsets against raw bytes already
     captured from `EquipInventoryData` earlier the same session (no
     new game reads needed) gave concrete field values immediately;
     `peek` then confirmed all of them live.

  **The verified chain:**

  ```
  EquipInventoryData (already resolved -- see above)
    +0x24 = threshold (int32, read live each time -- observed 128 in
             the session this was found, but the field is what's
             trusted, not the number)
    if inventoryItemId < threshold:
      +0x58 = pointer to the LOW segment
      entry = *(+0x58) + inventoryItemId * 16
    else:
      +0x48 = pointer to the HIGH segment
      entry = *(+0x48) + (inventoryItemId - threshold) * 16

  entry (16 bytes):
    +0x0 int32  uniqueId   (ItemUniqueIdPrefix-tagged: Weapon 0x80800000,
                             Armor 0x90800000, Accessory 0xA0000000,
                             Goods 0xB0000000)
    +0x4 int32  giveId     (the real EquipParamWeapon/Protector/
                             Accessory/Goods row ID; Weapon has no
                             prefix bits, Goods is OR'd with 0x40000000)
    +0x8 uint32 quantity
    +0xC int32  unknown1
  ```

  **Verified against real, named items** (giveId cross-checked against
  a community param name table — see below — not just internal
  consistency):

  | Slot | uniqueId prefix | giveId | Resolved name |
  |---|---|---|---|
  | R1 | Weapon | 8030000 | Reinforced Club (base level) |
  | L1 | Weapon | 13400004 | Pyromancy Flame +4 (base row 13400000 + reinforcement) |
  | R2/R3/L2/L3 | Weapon | 110000 | Fists (the empty-slot placeholder, not a real dead end) |
  | Head | Armor | 291935456 (0x10000000 prefix + 23500000) | Xanthous Crown |
  | Chest | Armor | 345936456 (+ 77501000) | Conjurator Robe |
  | Hands | Armor | 345937456 (+ 77502000) | Conjurator Manchettes |
  | Legs | Armor | 315438456 (+ 47003000) | Worker Trousers |
  | Ring1-4 | Accessory | 536891152/312/192/222 (0x20000000 + 20240/20400/20280/20310) | Great Swamp Ring, Covetous Silver Serpent Ring, Sage Ring, Saint's Ring |

  All fourteen equipped slots resolved correctly in one run, every
  `uniqueId` prefix matching its slot category, `quantity=1` on every
  one (correct for equipped gear) — not cherry-picked, this is the
  tool's actual full output.

  ### Full inventory — also solved

  `inventory` walks every possible `inventoryItemId`, not just the 14
  equipped slots — every item ever acquired, worn or not. The upper
  bound isn't guessed: `EquipInventoryData+0x10` is the *exact* loop
  bound the game's own code uses to walk this same array (seen directly
  in the disassembly that found the whole chain — see above), so the
  walk covers the container's real capacity, not an arbitrary range.
  Freed/never-used slots read back with an unrecognized `uniqueId`
  prefix and are skipped automatically by the same sanity check `equip`
  uses.

  Run live against the same character as the table above: 176 real
  items found scanning all 2048 capacity slots in 2.82ms (well inside
  the brief's latency target). Every category showed up correctly —
  stacked Goods (arrows, souls, upgrade materials — quantities up to 24
  seen, not just 1), unequipped Weapons/Armor/Accessories sitting in
  the box, and the same already-verified equipped items (id=220 still
  resolves to the Reinforced Club, id=128 matches byte-for-byte what an
  earlier investigation in this same session found). Real item count
  stopping right around 307 also lines up with the separate `+0x88`
  count field this repo was already reading — one more independent
  cross-check, not just internal consistency.

  ### Name resolution — also solved

  `equip` and `inventory` now print resolved English names, not just
  numeric IDs — through `generated/item_names.tsv` (weapons, armor,
  accessories, goods and spells), which `extract` reads from the game's
  own text in your install. Loading it is
  optional — if the file is missing, `equip`/`inventory` still print
  raw `giveId`/`uniqueId`, same as before.

  Weapon names needed one extra step: `giveId` encodes reinforcement
  level as +0..+10 within each 100-wide infusion block (confirmed live
  this session — L1's Pyromancy Flame +4 was `giveId=13400004`, base
  row `13400000`), and the name table only has entries at the
  unreinforced row, so the lookup strips the low two digits first
  (`giveId - giveId % 100`) before searching. Verified against the same
  14 equipped slots as the table above — every name matches exactly,
  including catching a real gap honestly: a handful of `inventory`
  entries (a run of sequential Goods IDs) print `(name not found)`
  rather than a guess, because those specific rows simply aren't in the
  4 extracted tables.

  ### Live re-resolution in `stats` — solved

  Attributes/base-max and equipped-item identity used to be read once
  at `stats` startup and never again — meaning a level-up or gear swap
  mid-run would go stale until the next invocation. Fixed:

  - **Level-up**: `stats` tracks the last-seen Level every tick; the
    moment it changes, attributes and base-max are automatically
    re-read and reprinted before that tick's HP/FP/Stamina line. Not
    counted in the reported read latency, which is meant to reflect
    steady-state polling cost, not the rare level-up event.
  - **Gear changes**: all 14 equipped slots (weapons, armor, rings) are
    checked every tick — a swap can happen at any time, not just on
    level-up. Any slot whose `inventoryItemId` changed is immediately
    resolved to full identity (category, `uniqueId`, `giveId`, name)
    and printed, using the exact same chain `equip` uses.

  **Verified live** (2026-08-18, same character, PID 13400): ran
  `stats 150` (~15s) in the background and swapped gear in-game during
  the run. Both changes were caught on the correct tick with full
  resolved identity, and normal HP/FP/Stamina polling continued
  unaffected the whole time (latency stayed at the same 15-20µs seen
  throughout this session — the extra 14 slot-check reads per tick are
  effectively free against a 100ms interval):

  ```
  [13:32:29.235] R1 slot changed: inventoryItemId=220 -> [Weapon] uniqueId=0x8080021E giveId=8030000    qty=1   Reinforced Club
  [13:32:31.504] L1 slot changed: inventoryItemId=233 -> [Weapon] uniqueId=0x808001D6 giveId=13290000   qty=1   Saint's Talisman
  ```

  (R1 went Exile Greatsword → Reinforced Club; L1 went Pyromancy Flame
  → Saint's Talisman — both real swaps performed live during the run,
  both resolved correctly.) Level-up detection uses the identical
  diff-and-refresh pattern but wasn't separately live-tested this
  session (would require actually leveling up the character, a
  permanent, costly action) — low risk, since it's a straightforward
  comparison over reads already proven correct elsewhere in this file,
  but noted here rather than silently assumed.

  **Restart-verified 2026-10-06:** `equip`/`inventory` run against a
  fresh process (PID 23320, a new game launch) with zero code changes
  resolved all slots and 303 inventory items correctly.

  **All equip slots live-verified 2026-10-06** — `stats` now also
  watches Arrow1/Bolt1/Arrow2/Bolt2/Covenant. With it running, the
  user swapped Head, Legs, each of Ring1-4, Arrow1, Arrow2, Bolt1 and
  Covenant (Warrior of Sunlight on/off) one at a time. Each change
  appeared on exactly the right slot with the right item, both
  directions, and nothing else moved. Taking off Saint's Ring also dropped
  Attunement Slots 4→3 on the same tick. Bolt2 is the only slot
  without a live swap (nothing on hand to put in it).

  **Level-up detection: live-verified 2026-10-06** on a fresh character,
  in overlay mode. One Vigor point at the Fire Keeper (level 9 → 10):
  - The session log got `{"type":"level","t":178.8,"from":9,"to":10}`.
  - The live page showed level 10, Vigor 12 → 13 and max HP 627 straight
    away, with the next-level cost moving on from 819 to 840.
  - Souls lost stayed 0: spending souls isn't counted as a loss.
  On a level change the guide also recomputes every equipped weapon's
  AR, which depends on Str/Dex/Int/Fth and used to go stale.

  Found on the way: the session log was opened without read sharing, so
  it couldn't be read (or a `report` built) while still recording. It
  now opens with `_wfsopen(..., _SH_DENYWR)`, and a live session file
  reads fine mid-game.
- ✅ **Current equip load — now COMPUTED, live-verified 2026-10-06.**
  The original read (found with no source lead via fscan/frescan and
  findptr, see "Current equip load and item discovery" above) used a
  findptr anchor, `DarkSoulsIII.exe+0x47F3B98` → `+0x1E7C`. In a later
  game session it read garbage (−0.0, −12855763968.0, huge random
  floats), so it doesn't survive a relaunch. It's replaced by the sum of
  every equipped item's param-table `weight`, restart-proof like poise
  and absorption. Fields: `EquipParamWeapon+0x0C` (weapons, shields,
  catalysts, ammo), `EquipParamProtector+0x20` (armor, already checked
  against the wiki for 4 pieces), and `EquipParamAccessory+0x08` (rings,
  covenant), the last two offsets from Paramdex. Weapon rows have the
  +0..+10 reinforcement digits stripped, since weight doesn't change with
  upgrades. **Exact match** against the status screen: 25.8 / 48.0 with
  the user's full loadout (13 weighted items across weapons, armor and
  rings), then 23.3 after the user unequipped the Brigand Twindaggers
  (2.5) live. `stats` prints the per-item breakdown and recomputes it on
  every gear change.
- ❌ **Item Discovery** — came from the same findptr anchor as the old
  equip load read, so it's broken after a relaunch too. `stats` no
  longer prints it. It needs its own restart-proof source: either a
  computed value (Luck plus item/ring bonuses, like max equip load) or a
  pointer chain found through an AOB-derived base.
- ✅ **Max equip load** — computed from Vitality (`40 + Vitality`, per
  the Fextralife wiki), the same approach as required-souls-for-level.
  Verified against this session's own live data (Vitality=8 → 48.0,
  exact match). ⚠️ Ring-bonus multipliers (Havel's Ring, Ring of Favor)
  are now computed via `SpEffectParam` (same mechanism as ring poise
  bonus, see "Ring equip-load bonus" above) — live-verified not to
  false-positive against the user's own non-load rings, but the actual
  bonus value is still unverified (no Havel's/Favor ring on hand). A
  name-based warning remains as a sanity-check fallback.
- ⚠️ **Hyperarmor poise: removed 2026-10-07** (user's call). The
  current-poise read (`SprjChrSuperArmorModule +0x28`, never
  live-positive-tested) is gone from `stats`; the investigation is in
  the git history. Built clean, **not yet run against the game**: check
  `stats` still prints its usual line, without `Poise=`.
- ✅ **Armor Poise (the passive equipment stat)** — computed live from
  `EquipParamProtector` via a generic live param-table reader (reusable
  for other tables/fields, per the request behind this feature). Needed
  two live-verified pieces, not one: the row/field navigation (checked
  against `weight`, an exactly-matching known-good field) and the raw
  `poise` field's real meaning, discovered by exhaustively scanning a
  whole row for the wiki's known values and finding none — the field
  turned out to be a damage-multiplier needing `100*(1-raw)` before
  combining. See "Armor Poise" above. Verified against this session's
  own live data (2026-08-19): 4 equipped pieces combine to 4.04, exactly
  matching the status screen, reconfirmed live right before the fix.
  Both Max Equip Load and Total Poise now also recompute automatically
  the instant an equip slot changes (not just at startup) — live-
  verified by swapping Hands and Chest mid-run and watching the total
  update each time (2.88 → 3.46 → 11.86).
- ✅ **Armor Defense/Absorption (all 8 damage types)** — same
  `EquipParamProtector` reader and diminishing-returns combine as Armor
  Poise, applied to a different field cluster. Discovered live
  (2026-08-19) that `EquipParamProtector`'s flat `defense_X` fields
  (i16) read exactly `0` for every damage type on every armor piece
  checked — vestigial/unused in this DS3 build — so Absorption is
  computed from the `_damage_cut_rate` fields (f32) instead, the same
  convention already validated for Poise. Live-verified **exact match
  to 3 decimal places, all 8 types simultaneously**: computed
  Phys=9.283 Strike=10.955 Slash=10.304 Thrust=9.098 Magic=21.725
  Fire=21.231 Lightning=22.980 Dark=24.361 against the user's own
  in-game Stats screen readings of the identical 8 numbers. See "Armor
  Defense/Absorption" above.
- ✅ **Status Resistances (Bleed/Poison/Frost/Curse)** — same
  `EquipParamProtector` reader, but plain point addition across armor
  pieces instead of the diminishing-returns formula used for Poise/
  Absorption (a genuinely different DS3 mechanic, not just reused
  math). Live-verified (2026-08-19): computed Bleed=85, Poison=155,
  Frost=111, Curse=147 all confirmed correct against the user's own
  in-game Resistance screen. Toxic=155 (same field cluster, read for
  free) could not be independently confirmed — the vanilla UI doesn't
  display a Toxic figure at all. See "Status Resistances" above.
- ✅ **Attunement Slots** — computed from Attunement via the documented
  Fextralife breakpoint table, plus a ring bonus read *generically*
  from `SpEffectParam` (`change_magic_slot`, same accessory→SpEffect
  chain as ring poise/equip-load bonuses, summed rather than
  multiplied). Corrected live twice on the way to this: an initial
  guess (Sage Ring, hardcoded by name) was disproved by the user
  unequipping a *different* ring and watching their real slot count
  drop; the real mechanism was then found by diffing both rings'
  actual `SpEffectParam` rows byte-for-byte. Live-verified both
  directions (2026-08-19): base 2 + 1 from Saint's Ring = 3 matching
  the status screen, then dropping to 2 in the same tick when that ring
  was unequipped live. See "Attunement Slots" above.
- ✅ **Equipped Spells** — reads each active attunement slot's spell via
  `PlayerGameData+0x470` (a pointer, one dereference needed — not a
  direct offset), resolved through a newly-added `Magic` name category
  (~110 spells; names now from the game's own text).
  A real bug was caught live, not just an unverified first pass: the
  initial version skipped the pointer dereference and read every slot
  as empty; the user immediately caught this (their 3 slots were all
  filled), leading to the fix. Live-verified exact match (2026-08-19):
  computed Fireball/Fire Orb/Great Combustion for the user's 3 slots,
  confirmed correct. Has its own per-tick change-detection trigger,
  independent of gear changes, since spells can be re-attuned without
  touching equipment. See "Equipped Spells" above.
- ✅ **Active right/left hand + bottom (quick item) slot** —
  `RightHandSlot`/`LeftHandSlot` reused from the two-handing work;
  bottom slot found via a community CE table entry
  (`m_selectedEquipItemSlotIdx`), calibrated against that table's own
  Lua scripts against `profile.xBase`. Live-verified correct **twice**,
  against two completely different gear states in the same session
  (Fire Reinforced Club/Pyromancy Flame/Purple Moss Clump, then R2 Hand
  Axe/L2 Caestus/Ashen Estus Flask+2 after the user swapped weapons and
  cycled items) — both exact matches. **All three slot indices (0-2)
  re-verified on both hands 2026-10-06** by cycling R1→R2→R3 and
  L1→L2→L3 live. ✅ **Top slot (active spell)** — `EquipMagicData+0x88`,
  found with `memdiff` and live-verified press-for-press against the
  HUD 2026-10-06. See "Active Right/Left Hand, Top (Spell) and Bottom
  Slot" above.
- ⚠️ **Ring poise bonus (SpEffectParam)** — implemented, offset
  live-corroborated by a real signal (4 unrelated equipped rings each
  independently reading the exact same `poise_rate=1.0000` default,
  which is what exposed and fixed an initially-wrong additive-formula
  guess), but the *combination formula itself* is still unverified
  against an actual poise ring (none on hand this session). See "Ring
  poise bonus" above.
- ✅ **Weapon Attack Rating and catalyst Spell Buff** — computed
  live from EquipParamWeapon + ReinforceParamWeapon + CalcCorrectGraph
  + AttackElementCorrectParam. **20/20 in-game readings exact
  (2026-10-06)** across Physical, Fire, Magic, the unmet-requirement
  penalty, two-handing, and Spell Buff on all ten catalysts owned. ⚠️
  Lightning/Dark damage, Hollow's Luck magnitude, and vow catalysts are
  decoded from the tables but not yet checked in-game (no gems or
  weapons on hand to test). See "AttackElementCorrectParam, the
  requirement penalty, and the second curve" above.
- ✅ **Two-handing state** — read live from `PlayerGameData`'s
  `WeaponSheathState`/`RightHandSlot`/`LeftHandSlot` (offsets sourced
  from AmySouls/DS3RuntimeScripting, confirmed against the same base
  already used for `EquipGameData` elsewhere in this file). Fully
  decoded live (2026-08-19), not just sourced: the user physically
  toggled grip in both directions while `stats` watched — one-handed=1,
  right two-handed=3, switched back to 1, then left two-handed=2 — a
  clean 3-state enum confirmed both ways. `stats` now shows the
  weapon's actual current AR (correctly two-handed only for the single
  active slot on the matching side) instead of always printing both
  variants. See "Two-handing state" above.
- ✅ Pointer chain expressed entirely in module-relative terms — no
  absolute address is hardcoded anywhere.
- ✅ **Confirmed across a real game restart** (2026-08-18): everything
  derived through the "where were we" checkpoint in this project's
  history — HP/FP/Stamina (current/max/base-max), Level/Souls,
  all 9 attributes, and equip-slot raw IDs — was originally derived and
  verified against PID 2856. Every subsequent command this session ran
  against a *different* process, PID 13400 — the game had been closed
  and relaunched in between — with zero code changes, and every value
  still resolved and read correctly (`stats 5`: HP=550/715, FP=114/114,
  Stamina=94/94, Level=47, Souls=1760, full attribute block, all 6
  weapon slots). This is exactly what "restart-proof" was meant to
  mean: a different PID is definitive proof of a different process
  instance, not just "should work." The equipped-item-identity and
  full-inventory chains (`equip`/`inventory`) were themselves *derived*
  against PID 13400, so this was their first run, not a restart test —
  but they rest on the exact same kind of module-relative, AOB-derived
  offsets as everything else here, so the same confidence applies going
  forward.
- ✅ **Milestone 2** (`stats --live`): persistent polling, measured
  10Hz, F10 pause/resume with the game focused (zero reads while
  paused), and its own CPU/memory/latency logged every 5s. See
  "Milestone 2 — live mode" above.
- ✅ **Milestone 3** (`window`): always-on-top companion window with
  live values and metrics, F10 pause. See "Milestone 3 — companion
  window" above. Not visible over exclusive fullscreen (by design).
- ✅ **Milestone 4** (`overlay`): click-through HUD panel tracking the
  game window, F10 hides + pauses, F11 perf HUD, 60fps unchanged. See
  "Milestone 4 — overlay" above.
- ⏳ Not built yet (Milestone 5): the extended stress
  test (no memory leak, creeping latency or dropped reads over a long
  session).
