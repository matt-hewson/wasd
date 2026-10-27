// WASD: World Awareness & State Display -- Milestone 1 proof of concept
//
// Hard safety constraints:
//   - READ-ONLY, always. This process never requests PROCESS_VM_WRITE and
//     never calls WriteProcessMemory anywhere. Only PROCESS_VM_READ and
//     PROCESS_QUERY_INFORMATION are ever requested from OpenProcess.
//   - Only ever attaches to the Seamless Co-op (EAC-disabled) build.
//     Vanilla online DS3 runs under the exact same executable name, so
//     before opening a read handle we require that a Seamless Co-op
//     module (ds3sc.dll) already be loaded in the target process. If
//     it isn't found, we refuse to attach. This is a best-effort guard,
//     not a guarantee -- never rely on this tool's judgement alone,
//     always know yourself which session you're running.
//
// Modes (argv[1]):
//   probe [--list-modules]
//       Milestone 1 self-test: find process, verify Seamless Co-op
//       marker, open read-only handle, do 5 timed reads of the module's
//       own PE header ('MZ'). Proves the read-only path end to end
//       without depending on any gameplay pointer offset. Default mode
//       if no argument is given.
//
//   scan <int32 value>
//       Read-only equivalent of a Cheat Engine "first scan". Walks the
//       process's committed, read-write, private (heap) memory regions
//       looking for 4-byte-aligned int32 cells equal to <value>, and
//       writes every match to build\candidates.txt. Use this with the
//       value currently shown on your HP HUD.
//
//   rescan <int32 value>
//       Read-only "next scan". Re-reads every address in
//       build\candidates.txt, keeps only the ones that now equal
//       <value>, and rewrites the file. Run this after your HP changes
//       (take a hit, heal, etc.) to narrow the candidate list down.
//
//   watch <hex address> [iterations]
//       Repeatedly reads a single int32 address (e.g. one narrowed-down
//       candidate) every 100ms and prints it with a timestamp + read
//       latency, so it can be eyeballed against the in-game HUD in real
//       time. Defaults to 50 iterations (~5s); Ctrl+C stops early.
//
//   findptr <hex target address> [hex max back-offset, default 0x800]
//       Read-only pointer scan. Searches all readable memory for 8-byte
//       pointer cells whose value V satisfies 0 <= target-V <= maxOffset
//       (i.e. V points at or just before target -- the usual "pointer to
//       start of a struct, field is N bytes in" shape). Any hit whose own
//       address falls inside DarkSoulsIII.exe's module range is tagged
//       STATIC and printed with a ready-to-use `resolve` chain, since
//       module+offset is stable across restarts (heap addresses are not).
//       Results are saved to build\ptrhits.txt. If no STATIC hit turns
//       up, re-run findptr against one of the heap hits to go one level
//       deeper.
//
//   resolve <hex offset0> [hex offset1 ...] <hex finalOffset>
//       Walks a chain of module-relative offsets and prints the resulting
//       int32 value + latency, repeated every 100ms for 30 reads. With
//       one offset: reads moduleBase+offset0 directly (pure static, no
//       pointer). With more than one: moduleBase+offset0 is treated as a
//       pointer cell, dereferenced, +offset1 applied, dereferenced again,
//       and so on, until the last offset is added after the final
//       dereference and read as the value (not dereferenced again). This
//       is how we turn a findptr STATIC hit into something that survives
//       a game restart: only module-relative offsets are hardcoded, the
//       actual heap addresses are re-resolved live each run.
//
//   wcm [hex target address]
//       Byte-signature scan for WorldChrMan (our own pattern, found in
//       the game's code: see "memory roots" below and `roots`),
//       resolved against the LIVE module image so no separate RVA/VA
//       translation is needed. Prints the module-relative offset (this
//       is what survives a restart) and walks world_chr_man -> +0x80 ->
//       +xa -> +0x18, same navigation the practice tool uses for player
//       state. Pass a previously-confirmed HP address (from watch) as
//       the optional argument and it'll compute + print the final
//       struct-relative offset and a ready-to-run `resolve` command.
//
//   hp [iterations]
//       The built-in, permanent version of what `wcm` + `resolve`
//       demonstrate by hand: re-derives the full WorldChrMan/XA/HP
//       pointer chain fresh every run (nothing hardcoded but a module-
//       relative byte pattern and one struct-field offset -- see
//       kHpFieldOffset), then polls it at 10Hz, printing HP + read
//       latency each time, with an avg/max latency summary at the end.
//       Defaults to 100 reads (~10s); Ctrl+C stops early. This is the
//       command to run after a game restart to confirm the chain still
//       holds.
//
//   overlay
//       Click-through HUD panel over the game window's top-right (game
//       windowed or borderless) with only the critical hints, plus the
//       live page: a local web dashboard at http://localhost:8765 (this
//       PC only) with everything else, opened in the browser at start.
//       F10 hides the overlay + pauses polling, F11 toggles perf figures,
//       F9 cycles the spoiler tier (also settable on the page). Ctrl+C
//       exits.
//
//   flag <id> [id ...] [--trace] | flag --bosses
//       Reads DS3 event flags (item pickups, boss kills, bonfires, quest
//       steps). --bosses checks the 25 boss-defeated flags.
//
//   missables [--full]
//       Missable-content warnings from data\missables.tsv (NPC questline
//       steps, missable items, ending requirements): pending / missed /
//       done, at the spoiler tier. Marks made on the live page apply.
//
//   route [--full]
//       Route hints: the game's steps (main path, optional areas, DLC)
//       from data\route.tsv, open / done / locked from boss flags, and the
//       suggested next step -- all at the spoiler tier.
//
//   resist [--full]
//       Bosses' damage absorptions and status resistances, live from
//       NpcParam (rows found via data\generated\enemies.tsv). Defeated
//       bosses always; the rest at spoiler tier Full or with --full.
//
//   nearby [N] | nearby --watch
//       The N (default 10) closest unfound world pickups: distance, clock
//       direction, height, item. Needs data\generated\treasures.tsv from
//       tools\extract_treasures.py (positions from the map files).
//       --watch logs how far you stood from each item you pick up.
//
//   upgrades
//       Each equipped weapon's next upgrade: materials needed vs. held.
//
//   keys
//       Key items (keys, quest items, spell tomes): held, obtained (used
//       up) or not yet, with what each opens.
//
//   progress
//       Bosses per area (defeated or not, required/optional) and Estus /
//       Undead Bone Shards found out of the total.
//
//   items [--map NN]
//       The one-time world pickups in the current named area (or the given
//       map section), found or not, from ItemLotParam + their event flags.
//
//   paramsearch <table> <int32 value> [hex row bytes]
//       Finds every row/offset of a live game table holding a number
//       (e.g. which ItemLotParam row sets a given pickup flag).
//
//   relay create|<code>|pair:<key> [--url URL] [--send HEX] [--seconds N] [--sync [--still] [--name N]]
//       Creates or joins a co-op relay session (the relay in settings.ini,
//       or --url) and prints what happens: the code, the partner coming
//       and going, frames received. With --sync, a live co-op link for a
//       made-up character instead (deltas, resync), printing the partner's
//       state as it changes. No game needed.
//
//   relay-host [--port N] [--seconds N] [--stale-ms N] [--empty-room-ms N] [--unjoined-room-ms N]
//       Runs only the co-op relay, on 127.0.0.1 (port 8790 unless given),
//       until stopped. The app hosts the same relay itself with host_relay=1.
//
//   flagwatch [--learn n] [--iter n]
//       Prints every event flag that flips (with its id) while you play;
//       learns and suppresses self-toggling flags first.
//
//   report [--no-open]
//       Builds reports/results.html from every saved session
//       (sessions/*/*.jsonl, recorded by the live modes) and opens it.
//
//   window
//       Retired: the Milestone 3 companion window was replaced by the
//       live page (see overlay). Runs overlay mode.
//
//   stats --live
//       Milestone 2: same as stats, but runs until closed. F10 pauses/
//       resumes (works with the game focused; zero reads while paused),
//       Ctrl+C stops with a summary. Prints the status line only on
//       change, plus a perf line (rate/latency/CPU/memory) every 5s.
//
//   stats [iterations]
//       Same as `hp`, but reads FP (DS3's "mana") and Stamina alongside
//       HP each tick -- all three live on the same resolved struct, at
//       +0xD8 / +0xE4 / +0xF0 respectively. Prints all three + one
//       latency figure covering all 3 reads. Defaults to 100 reads
//       (~10s).
//
//   dump <hex module offset> [byte count, default 256] [save path]
//       Read-only hex dump of raw bytes at DarkSoulsIII.exe+offset.
//       Diagnostic tool for hand-inspecting candidate code/data
//       addresses. Pass a save path to also write the exact bytes to a
//       file (byte-for-byte, via the same ReadProcessMemory call --
//       still 100% read-only against the game) for loading into a real
//       offline disassembler.
//
//   peek <hex absolute address> [byte count, default 64] [save path]
//       Read-only hex dump of raw bytes at an absolute address (not
//       module-relative like `dump`) -- for inspecting scan/rescan/
//       findptr hits. Investigative only, not restart-proof by itself.
//       Same optional save-to-file behavior as `dump`.
//
//   equip
//       Resolves every equipped slot (weapons, armor, rings) to its
//       real {uniqueId, giveId, quantity} record. Disassembly-verified
//       two-segment array lookup -- see docs/TECHNICAL.md for the full
//       derivation and live confirmation against real item names.
//
//   inventory
//       Same underlying chain as `equip`, but walks the ENTIRE
//       inventory (every item ever acquired, not just the 14 equipped
//       slots) using the container's own capacity field as the loop
//       bound -- the same bound the game's own code uses for this
//       array. Prints every slot that resolves to a real item.
//
//   ar [--verbose] [--row <giveId>] [--infusions <giveId>]
//       Attack Rating (base+bonus / base-penalty, 1H and 2H) and Spell
//       Buff for every weapon in the inventory, or for any
//       EquipParamWeapon row. See docs/TECHNICAL.md "Weapon Attack Rating".
//
//   memdiff <pgd|chrins|chrmods|chrdata|hex addr> [hex deref ...]
//           [--len hex] [--learn n] [--iter n]
//       Live diff of a structure: learns which int32s change on their
//       own, then prints only what changes after that. Found the
//       active-spell index.
//
//   inv [--id <decimal inventoryItemId>]
//       Investigates EquipInventoryData's raw memory layout to find the
//       InventoryItem[] array pointer, so equip-slot IDs can eventually
//       be resolved to real item identity without calling into game
//       code. Hex-dumps EquipInventoryData, then treats every QWORD in
//       that dump that looks like a heap pointer as a candidate array
//       base and test-reads `candidate + inventoryItemId*16` as an
//       {uniqueId, giveId, quantity, unknown1} struct. Defaults to
//       using the current R1 weapon slot's ID; pass --id to test a
//       different one. Purely investigative -- see docs/TECHNICAL.md for
//       status.
//
// No mode here ever writes to the target process. All modes reuse the
// same read-only attach path as probe.

#include <winsock2.h>  // live page server (before windows.h, which would pull in winsock v1)
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <share.h>  // _SH_DENYWR (session log readable while recording)
#include <shobjidl.h>  // IFileOpenDialog, SetCurrentProcessExplicitAppUserModelID (WASD.exe)
#include <winhttp.h>   // the co-op relay's WebSockets (src/core/relay_client.cpp)
#include <bcrypt.h>    // the relay host's handshake hash and session codes, the pairing key

#include <cctype>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <wchar.h>
#include <cwctype>

#include "version.h"  // WASD_VERSION_STRING, WASD_APP_NAME ...

#pragma comment(lib, "user32.lib")  // RegisterHotKey / message queue (stats --live)
#pragma comment(lib, "psapi.lib")   // GetProcessMemoryInfo (stats --live perf line)
#pragma comment(lib, "gdi32.lib")   // overlay drawing
#pragma comment(lib, "ws2_32.lib")  // live page server (overlay mode, 127.0.0.1 only)
#pragma comment(lib, "shell32.lib") // ShellExecuteW: open the results page (report mode)
#pragma comment(lib, "advapi32.lib") // RegGetValueW: Steam's install path (extract)
#pragma comment(lib, "ole32.lib")    // the app window's folder picker (WASD.exe)
#pragma comment(lib, "winhttp.lib")  // the co-op relay's WebSockets
#pragma comment(lib, "bcrypt.lib")   // SHA-1 / SHA-256 and random bytes (the co-op relay host, pairing)

// The code lives in src/core (nothing DS3-specific: files, logs, settings,
// sessions, the overlay, the app window, the live page server, memory
// tools) and src/ds3 (everything that knows the game). It's still one
// translation unit: the parts are included here, in this order, because
// each uses what the parts above it define, and core code still calls DS3
// code. A part moves to core only once a second game needs it too.
namespace {
#include "ds3/game.cpp"
#include "core/paths.cpp"
#include "core/process.cpp"
#include "core/memory_tools.cpp"
#include "ds3/player.cpp"
#include "ds3/formulas.cpp"
#include "ds3/params.cpp"
#include "ds3/gear_stats.cpp"
#include "ds3/area.cpp"
#include "core/settings.cpp"
#include "core/text.cpp"
#include "core/session_log.cpp"
#include "ds3/character.cpp"
#include "ds3/live_stats.cpp"
#include "core/hexdump.cpp"
#include "ds3/inventory.cpp"
#include "core/memdiff.cpp"
#include "ds3/coop.cpp"
#include "ds3/coop_sync.cpp"
#include "ds3/snapshot.cpp"
#include "ds3/level_plan.cpp"
#include "ds3/poll.cpp"
#include "core/overlay.cpp"
#include "core/files.cpp"
#include "ds3/mapdata.cpp"
#include "core/app_window.cpp"
#include "ds3/flags.cpp"
#include "ds3/pickups.cpp"
#include "ds3/progress.cpp"
#include "ds3/positions.cpp"
#include "ds3/resist.cpp"
#include "ds3/route.cpp"
#include "ds3/missables.cpp"
#include "ds3/live_data.cpp"
#include "core/relay_client.cpp"
#include "core/relay_server.cpp"
#include "core/tailscale.cpp"
#include "ds3/coop_link.cpp"
#include "ds3/coop_session.cpp"
#include "core/live_server.cpp"
}  // namespace

// The command to run: the first argument, or `overlay` when there is none,
// so double-clicking wasd-cli.exe starts the overlay and live page (the
// default was `probe`, the Milestone 1 self-test, which printed a few reads
// and closed -- found 2026-10-07). `probe` now runs only when named.
std::wstring CommandOf(int argc, wchar_t** argv) {
  return argc > 1 && argv[1] && argv[1][0] ? std::wstring(argv[1]) : std::wstring(L"overlay");
}

// Keep the window open after a failure, but only when the console is this
// process's alone -- i.e. Explorer made it for a double-click, and it would
// vanish with the error. From a terminal or a script (the console is shared:
// 2+ processes) never wait, so nothing scripted can hang.
bool ShouldPauseOnExit(int exitCode, unsigned long consoleProcessCount) {
  return exitCode != 0 && consoleProcessCount == 1;
}

// The test build (tests/cpp/run_tests.cpp) includes this file with
// DS3_NO_MAIN defined and supplies its own entry point.
#ifndef DS3_NO_MAIN
int RunCommand(int argc, wchar_t** argv) {
  std::wstring mode = CommandOf(argc, argv);
  if (mode == L"paths") {  // where this copy reads and writes
    const AppPaths& p = Paths();
    // UTF-8 (scripts and tests read this); the console shows it right with `chcp 65001`.
    std::printf("assets     %s\nuser data  %s  (%s)\ngenerated  %s\nmode       %s\n", Utf8(p.assets).c_str(),
                Utf8(p.user).c_str(), Utf8(p.userSource).c_str(), Utf8(p.generated).c_str(),
                p.devMode ? "dev checkout" : "installed");
    return 0;
  }
  if (mode == L"--version" || mode == L"version") {
    std::printf("%s %s\n", WASD_APP_NAME, WASD_VERSION_STRING);
    return 0;
  }
  if (mode == L"relay") {  // relay create|<code>: a co-op relay session (no game needed)
    LoadSettings();
    for (int i = 3; i < argc; ++i)
      if (std::wstring(argv[i]) == L"--sync") return RunRelaySync(argc, argv);  // a made-up character's live link
    return RunRelay(argc, argv);
  }
  if (mode == L"relay-host") {  // the co-op relay on its own (no game needed)
    LoadSettings();
    return RunRelayHost(argc, argv);
  }
  if (mode == L"coop") {  // coop <code>: what a co-op code holds (no game needed)
    if (argc < 3) {
      std::fwprintf(stderr, L"usage: %ls coop <WASD-... code>\n", argv[0]);
      return 1;
    }
    CoopPacket p = CoopDecodeText(Utf8(argv[2]));
    if (!p.ok) {
      std::printf("Not a usable code: %s\n", p.error.c_str());
      return 1;
    }
    std::printf("%s", DescribeCoopPacket(p).c_str());
    return 0;
  }
  // Spoiler tier: saved setting, or --full for this run.
  LoadSettings();
  for (int i = 2; i < argc; ++i)
    if (std::wstring(argv[i]) == L"--full") InterlockedExchange(&g_spoilerTier, kTierFull);

  if (mode == L"probe") {
    bool listModules = false;
    for (int i = 2; i < argc; ++i) {
      if (std::wstring(argv[i]) == L"--list-modules") listModules = true;
    }
    return RunProbe(listModules);
  }

  if (mode == L"scan") {
    if (argc < 3) {
      std::fwprintf(stderr, L"usage: %ls scan <int32 value>\n", argv[0]);
      return 64;
    }
    return RunScan(static_cast<int32_t>(_wtoi(argv[2])));
  }

  if (mode == L"rescan") {
    if (argc < 3) {
      std::fwprintf(stderr, L"usage: %ls rescan <int32 value>\n", argv[0]);
      return 64;
    }
    return RunRescan(static_cast<int32_t>(_wtoi(argv[2])));
  }

  if (mode == L"fscan") {
    if (argc < 3) {
      std::fwprintf(stderr, L"usage: %ls fscan <float value> [tolerance, default 0.05]\n",
                    argv[0]);
      return 64;
    }
    float value = static_cast<float>(_wtof(argv[2]));
    float tolerance = argc > 3 ? static_cast<float>(_wtof(argv[3])) : kDefaultFloatTolerance;
    return RunFScan(value, tolerance);
  }

  if (mode == L"frescan") {
    if (argc < 3) {
      std::fwprintf(stderr, L"usage: %ls frescan <float value> [tolerance, default 0.05]\n",
                    argv[0]);
      return 64;
    }
    float value = static_cast<float>(_wtof(argv[2]));
    float tolerance = argc > 3 ? static_cast<float>(_wtof(argv[3])) : kDefaultFloatTolerance;
    return RunFRescan(value, tolerance);
  }

  if (mode == L"watch") {
    if (argc < 3) {
      std::fwprintf(stderr, L"usage: %ls watch <hex address> [iterations] [--float]\n", argv[0]);
      return 64;
    }
    uintptr_t address = static_cast<uintptr_t>(std::wcstoull(argv[2], nullptr, 16));
    int iterations = 50;
    bool asFloat = false;
    for (int i = 3; i < argc; ++i) {
      std::wstring arg = argv[i];
      if (arg == L"--float") {
        asFloat = true;
      } else {
        iterations = _wtoi(argv[i]);
      }
    }
    return RunWatch(address, iterations, asFloat);
  }

  if (mode == L"findptr") {
    if (argc < 3) {
      std::fwprintf(stderr, L"usage: %ls findptr <hex target address> [hex max back-offset]\n",
                    argv[0]);
      return 64;
    }
    uintptr_t target = static_cast<uintptr_t>(std::wcstoull(argv[2], nullptr, 16));
    uintptr_t maxBackOffset =
        argc > 3 ? static_cast<uintptr_t>(std::wcstoull(argv[3], nullptr, 16)) : 0x800;
    return RunFindPtr(target, maxBackOffset);
  }

  if (mode == L"resolve") {
    if (argc < 3) {
      std::fwprintf(stderr, L"usage: %ls resolve <hex offset0> [hex offset1 ...]\n", argv[0]);
      return 64;
    }
    std::vector<uintptr_t> offsets;
    for (int i = 2; i < argc; ++i) {
      offsets.push_back(static_cast<uintptr_t>(std::wcstoull(argv[i], nullptr, 16)));
    }
    return RunResolve(offsets);
  }

  if (mode == L"wcm") {
    bool haveTarget = argc > 2;
    uintptr_t target = haveTarget ? static_cast<uintptr_t>(std::wcstoull(argv[2], nullptr, 16)) : 0;
    return RunWcm(haveTarget, target);
  }

  if (mode == L"hp") {
    int iterations = argc > 2 ? _wtoi(argv[2]) : 100;
    return RunHp(iterations);
  }

  if (mode == L"stats") {
    bool live = false;
    int iterations = 100;
    for (int i = 2; i < argc; ++i) {
      if (std::wstring(argv[i]) == L"--live") {
        live = true;
      } else {
        iterations = _wtoi(argv[i]);
      }
    }
    return RunStats(iterations, live);
  }

  if (mode == L"dump") {
    if (argc < 3) {
      std::fwprintf(stderr,
                    L"usage: %ls dump <hex module offset> [byte count, default 256] [save "
                    L"path]\n",
                    argv[0]);
      return 64;
    }
    uintptr_t offset = static_cast<uintptr_t>(std::wcstoull(argv[2], nullptr, 16));
    SIZE_T length = argc > 3 ? static_cast<SIZE_T>(std::wcstoull(argv[3], nullptr, 16)) : 256;
    std::wstring savePath = argc > 4 ? argv[4] : L"";
    return RunDump(offset, length, savePath);
  }

  if (mode == L"roots") return RunRoots();

  if (mode == L"dumpmodule") {
    if (argc < 3) {
      std::fwprintf(stderr, L"usage: %ls dumpmodule <save path>\n", argv[0]);
      return 64;
    }
    return RunDumpModule(argv[2]);
  }

  if (mode == L"peek") {
    if (argc < 3) {
      std::fwprintf(stderr,
                    L"usage: %ls peek <hex absolute address> [byte count, default 64] [save "
                    L"path]\n",
                    argv[0]);
      return 64;
    }
    uintptr_t address = static_cast<uintptr_t>(std::wcstoull(argv[2], nullptr, 16));
    SIZE_T length = argc > 3 ? static_cast<SIZE_T>(std::wcstoull(argv[3], nullptr, 16)) : 64;
    std::wstring savePath = argc > 4 ? argv[4] : L"";
    return RunPeek(address, length, savePath);
  }

  if (mode == L"paramrow") {
    if (argc < 4) {
      std::fwprintf(stderr,
                    L"usage: %ls paramrow <table name, e.g. EquipParamWeapon> <decimal row id> "
                    L"[byte count, default 512] [save path]\n",
                    argv[0]);
      return 64;
    }
    std::wstring tableName = argv[2];
    int64_t rowId = _wtoi64(argv[3]);
    SIZE_T length = argc > 4 ? static_cast<SIZE_T>(std::wcstoull(argv[4], nullptr, 16)) : 512;
    std::wstring savePath = argc > 5 ? argv[5] : L"";
    return RunParamRow(tableName, rowId, length, savePath);
  }

  if (mode == L"equip") {
    return RunEquip();
  }

  if (mode == L"inventory") {
    return RunInventory();
  }

  if (mode == L"inv") {
    bool haveSlotOverride = false;
    int32_t slotOverride = -1;
    for (int i = 2; i < argc; ++i) {
      if (std::wstring(argv[i]) == L"--id" && i + 1 < argc) {
        haveSlotOverride = true;
        slotOverride = _wtoi(argv[i + 1]);
        ++i;
      }
    }
    return RunInv(haveSlotOverride, slotOverride);
  }

  if (mode == L"ar") {
    std::vector<int32_t> rows;
    for (int i = 2; i < argc; ++i) {
      if (std::wstring(argv[i]) == L"--verbose") g_arVerbose = true;
    }
    for (int i = 2; i + 1 < argc; ++i) {
      std::wstring arg = argv[i];
      int32_t giveId = _wtoi(argv[i + 1]);
      if (arg == L"--row") {
        rows.push_back(giveId);
        ++i;
      } else if (arg == L"--infusions") {
        int32_t level = giveId % 100;
        int32_t weaponBase = giveId - giveId % 10000;  // infusions occupy +0..+1500 in 100s
        for (int32_t inf = 0; inf <= 1500; inf += 100) rows.push_back(weaponBase + inf + level);
        ++i;
      }
    }
    return RunAr(rows);
  }

  if (mode == L"missables") {
    return RunMissables();
  }

  if (mode == L"route") {
    return RunRoute();
  }

  if (mode == L"resist") {
    return RunResist();
  }

  if (mode == L"nearby") {
    bool watch = false;
    int count = 10;
    for (int i = 2; i < argc; ++i) {
      std::wstring a = argv[i];
      if (a == L"--watch") watch = true;
      else if (_wtoi(argv[i]) > 0) count = _wtoi(argv[i]);
    }
    return RunNearby(count, watch);
  }

  if (mode == L"upgrades") {
    return RunUpgrades();
  }

  if (mode == L"keys") {
    return RunKeys();
  }

  if (mode == L"progress") {
    return RunProgress();
  }

  if (mode == L"items") {
    int mapOverride = -1;
    for (int i = 2; i + 1 < argc; ++i) {
      if (std::wstring(argv[i]) == L"--map") mapOverride = _wtoi(argv[++i]);
    }
    return RunItems(mapOverride);
  }

  if (mode == L"graphs") {
    return RunGraphs();
  }

  if (mode == L"paramsearch") {
    if (argc < 4) {
      std::fwprintf(stderr, L"usage: %ls paramsearch <table name> <int32 value> [hex row bytes, default 100]\n", argv[0]);
      return 64;
    }
    SIZE_T rowBytes = argc > 4 ? static_cast<SIZE_T>(std::wcstoull(argv[4], nullptr, 16)) : 0x100;
    return RunParamSearch(argv[2], static_cast<int32_t>(std::wcstol(argv[3], nullptr, 10)), rowBytes);
  }

  if (mode == L"flagwatch") {
    int learn = 15, iter = 3000;  // 3s learning, then 10 minutes
    for (int i = 2; i + 1 < argc; ++i) {
      std::wstring arg = argv[i];
      if (arg == L"--learn") learn = _wtoi(argv[++i]);
      else if (arg == L"--iter") iter = _wtoi(argv[++i]);
    }
    return RunFlagWatch(learn, iter);
  }

  if (mode == L"flag") {
    std::vector<uint32_t> ids;
    bool bosses = false, trace = false;
    for (int i = 2; i < argc; ++i) {
      std::wstring arg = argv[i];
      if (arg == L"--bosses") bosses = true;
      else if (arg == L"--trace") trace = true;
      else ids.push_back(static_cast<uint32_t>(std::wcstoul(argv[i], nullptr, 10)));
    }
    if (ids.empty() && !bosses) {
      std::fwprintf(stderr, L"usage: %ls flag <event flag id> [...] [--trace] | flag --bosses\n", argv[0]);
      return 64;
    }
    return RunFlag(ids, bosses, trace);
  }

  if (mode == L"report") {
    bool open = !(argc > 2 && std::wstring(argv[2]) == L"--no-open");
    return RunReport(open);
  }

  if (mode == L"window") {
    Log("The companion window was replaced by the live page -- running overlay mode.");
    return RunOverlay();
  }

  if (mode == L"extract") return RunExtractCommand(argc, argv);
  if (mode == L"overlay") {
    return RunOverlay();  // (--window is accepted and ignored: the live page replaced it)
  }

  if (mode == L"memdiff") {
    if (argc < 3) {
      std::fwprintf(stderr,
                    L"usage: %ls memdiff <pgd|chrins|chrmods|chrdata|hex addr> [hex deref "
                    L"offset ...] [--len hex, default 1000] [--learn n, default 30] [--iter n, "
                    L"default 600]\n",
                    argv[0]);
      return 64;
    }
    std::vector<uintptr_t> derefs;
    SIZE_T len = 0x1000;
    int learn = 30, iter = 600;
    bool summary = false;
    for (int i = 3; i < argc; ++i) {
      std::wstring arg = argv[i];
      if (arg == L"--summary") {
        summary = true;
      } else if (arg == L"--len" && i + 1 < argc) {
        len = static_cast<SIZE_T>(std::wcstoull(argv[++i], nullptr, 16));
      } else if (arg == L"--learn" && i + 1 < argc) {
        learn = _wtoi(argv[++i]);
      } else if (arg == L"--iter" && i + 1 < argc) {
        iter = _wtoi(argv[++i]);
      } else {
        derefs.push_back(static_cast<uintptr_t>(std::wcstoull(argv[i], nullptr, 16)));
      }
    }
    return RunMemDiff(argv[2], derefs, len, learn, iter, summary);
  }

  std::fwprintf(stderr,
                L"unknown mode '%ls'. Expected: probe | scan | rescan | fscan | frescan | "
                L"watch | findptr | resolve | wcm | hp | stats | dump | peek | inv | equip | "
                L"inventory | ar | memdiff | window | overlay | report | extract | paths | relay | relay-host\n",
                mode.c_str());
  return 64;
}

// WASD.exe (linked with /SUBSYSTEM:WINDOWS from the same object file).
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) { return RunApp(cmdLine ? cmdLine : L""); }

int wmain(int argc, wchar_t** argv) {
  int code = RunCommand(argc, argv);
  DWORD pids[4];
  if (ShouldPauseOnExit(code, GetConsoleProcessList(pids, 4))) {
    std::printf("\n%s stopped (exit code %d). Press Enter to close this window.\n", WASD_APP_SHORT_NAME, code);
    std::fflush(stdout);
    std::getchar();
  }
  return code;
}
#endif  // DS3_NO_MAIN
