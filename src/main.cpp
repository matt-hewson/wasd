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

#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
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

namespace {

constexpr wchar_t kTargetProcessName[] = L"DarkSoulsIII.exe";

// Best-effort marker that the attached process is the Seamless Co-op
// (EAC-disabled) build rather than vanilla online DS3. Both run under
// the same DarkSoulsIII.exe process name, so this module-presence
// check is the only practical signal we have. Confirmed present in a
// live Seamless Co-op session via `probe --list-modules` on 2026-08-17.
constexpr wchar_t kSeamlessCoopModuleMarker[] = L"ds3sc.dll";

std::wstring WidenUtf8(const std::string& s);  // defined with the JSON helpers

// ---- AppPaths: where files live -----------------
// [AppPaths begin] The tests/cpp lint allows file locations to be spelled
// out only between these markers; everything else asks Paths().
//
// Two roots:
//   assets -- read-only data\*.tsv and templates\: the folder above the exe
//             (repo\build\wasd-cli.exe -> repo; <install>\bin\WASD.exe -> <install>).
//   user   -- read-write: settings.ini, sessions\, reports\, logs\, scratch\.
//             WASD_USER_DIR if set (tests use it); else the repo itself in a
//             dev checkout (.git and src\main.cpp beside data\), so a
//             developer's sessions and settings stay where they were; else
//             %LOCALAPPDATA%\WASD, which an install, upgrade or uninstall of
//             the program files never touches.
// Generated data (treasures.tsv, enemies.tsv and the name tables, made from
// the user's own game install) sits with the assets in a dev checkout
// (data\generated, where tools\extract_treasures.py writes it) and under the
// user root otherwise.
struct AppPaths {
  std::wstring assets, user, generated;
  bool devMode = false;
  std::wstring userSource;  // why `user` is what it is, for `paths` and the logs

  std::wstring Data(const std::wstring& file) const { return assets + L"\\data\\" + file; }
  std::wstring Template(const std::wstring& file) const { return assets + L"\\templates\\" + file; }
  std::wstring Generated(const std::wstring& file) const { return generated + L"\\" + file; }
  std::wstring Settings() const { return user + L"\\settings.ini"; }
  std::wstring Sessions() const { return user + L"\\sessions"; }
  std::wstring Reports() const { return user + L"\\reports"; }
  std::wstring Logs() const { return user + L"\\logs"; }
  std::wstring ScratchDir() const { return user + L"\\scratch"; }  // scan / findptr results
  std::wstring Scratch(const std::wstring& file) const { return ScratchDir() + L"\\" + file; }
  // Map-data extractor: the script beside data\, and
  // the embeddable Python an install ships in python\ (a dev checkout uses
  // the system Python instead).
  std::wstring ExtractorScript() const { return assets + L"\\tools\\extract_treasures.py"; }
  std::wstring BundledPython() const { return assets + L"\\python\\python.exe"; }
};

struct AppPathsInput {
  std::wstring exePath;       // the running exe (GetModuleFileNameW)
  std::wstring envUserDir;    // WASD_USER_DIR, "" when unset
  std::wstring localAppData;  // %LOCALAPPDATA%, "" when unset
  std::function<bool(const std::wstring&)> exists;
};

// "C:\a\b" -> "C:\a"; "" or a bare name -> ".".
std::wstring ParentDir(const std::wstring& path) {
  size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
}

// Pure: the same inputs always give the same paths (unit-tested).
AppPaths ResolveAppPaths(const AppPathsInput& in) {
  auto trimmed = [](std::wstring d) {
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    return d;
  };
  AppPaths p;
  std::wstring exeDir = ParentDir(in.exePath);
  p.assets = exeDir.find_last_of(L"\\/") == std::wstring::npos ? std::wstring(L".") : ParentDir(exeDir);
  p.devMode = in.exists && in.exists(p.assets + L"\\.git") && in.exists(p.assets + L"\\src\\main.cpp");
  if (!in.envUserDir.empty()) {
    p.user = trimmed(in.envUserDir);
    p.userSource = L"WASD_USER_DIR";
  } else if (p.devMode) {
    p.user = p.assets;
    p.userSource = L"dev checkout";
  } else if (!in.localAppData.empty()) {
    p.user = trimmed(in.localAppData) + L"\\" + WidenUtf8(WASD_APP_SHORT_NAME);
    p.userSource = L"%LOCALAPPDATA%";
  } else {
    p.user = p.assets + L"\\userdata";
    p.userSource = L"no %LOCALAPPDATA%";
  }
  p.generated = p.devMode ? p.assets + L"\\data\\generated" : p.user + L"\\generated";
  return p;
}

std::wstring EnvVar(const wchar_t* name) {
  wchar_t buf[2048];
  DWORD n = GetEnvironmentVariableW(name, buf, 2048);
  return n > 0 && n < 2048 ? std::wstring(buf, n) : std::wstring();
}

// This process's paths, resolved once.
const AppPaths& Paths() {
  static const AppPaths paths = [] {
    AppPathsInput in;
    wchar_t exe[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (len != 0 && len != MAX_PATH) in.exePath.assign(exe, len);
    in.envUserDir = EnvVar(L"WASD_USER_DIR");
    in.localAppData = EnvVar(L"LOCALAPPDATA");
    in.exists = [](const std::wstring& f) { return GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES; };
    return ResolveAppPaths(in);
  }();
  return paths;
}

// Creates a folder and any missing parents (CreateDirectoryW makes one
// level only, and %LOCALAPPDATA%\WASD doesn't exist on a fresh install).
bool EnsureDir(const std::wstring& dir) {
  if (dir.empty() || dir == L".") return true;
  DWORD a = GetFileAttributesW(dir.c_str());
  if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
  std::wstring parent = ParentDir(dir);
  if (parent != dir && parent.size() > 2) EnsureDir(parent);  // stop at "C:"
  return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// UTF-8 for printing paths: wprintf goes through the console code page and
// turns e.g. "é" into "?" (found 2026-10-07).
std::string Utf8(const std::wstring& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}
// [AppPaths end]

// scan / rescan candidates (was build\candidates.txt, relative to the
// current folder, so it only worked when started from the repo root).
std::wstring CandidatesPath() { return Paths().Scratch(L"candidates.txt"); }

std::string Timestamp() {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  std::time_t t = system_clock::to_time_t(now);
  std::tm tmv;
  localtime_s(&tmv, &t);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                static_cast<int>(ms.count()));
  return buf;
}

// WASD.exe has no console, so it also logs to a file (OpenLogFile,
// logs\wasd-YYYYMMDD.log in the user data folder). nullptr = console only.
FILE* g_logFile = nullptr;

void Log(const char* fmt, ...) {
  std::string ts = Timestamp();
  std::printf("[%s] ", ts.c_str());
  va_list args;
  va_start(args, fmt);
  if (g_logFile) {
    va_list copy;
    va_copy(copy, args);
    std::fprintf(g_logFile, "[%s] ", ts.c_str());
    std::vfprintf(g_logFile, fmt, copy);
    std::fputc('\n', g_logFile);
    std::fflush(g_logFile);
    va_end(copy);
  }
  std::vprintf(fmt, args);
  va_end(args);
  std::printf("\n");
  // Redirected stdout is block-buffered; flush so a log file tails live
  // (stats --live runs indefinitely). Log lines are rare -- per change,
  // not per read -- so this costs nothing measurable.
  std::fflush(stdout);
}

// ---- log files (WASD.exe) ---------------------------
// One file per day, logs\wasd-YYYYMMDD.log in the user data folder; files
// more than kLogKeepDays old are deleted when the app starts.
constexpr int kLogKeepDays = 7;

std::wstring LogFileName(int year, int month, int day) {
  wchar_t buf[32];
  std::swprintf(buf, 32, L"wasd-%04d%02d%02d.log", year, month, day);
  return buf;
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
long DaysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Which of these file names are WASD logs older than keepDays before the
// given day. Anything not named wasd-YYYYMMDD.log is left alone.
std::vector<std::wstring> OldLogs(const std::vector<std::wstring>& names, int year, int month, int day, int keepDays) {
  std::vector<std::wstring> out;
  long today = DaysFromCivil(year, month, day);
  for (const auto& n : names) {
    // "wasd-" + 8 digits + ".log"
    if (n.size() != 17 || n.compare(0, 5, L"wasd-") != 0 || n.compare(13, 4, L".log") != 0) continue;
    if (!std::all_of(n.begin() + 5, n.begin() + 13, [](wchar_t c) { return c >= L'0' && c <= L'9'; })) continue;
    auto num = [&](size_t at, size_t len) { return std::stoi(n.substr(at, len)); };
    int y = num(5, 4), m = num(9, 2), d = num(11, 2);
    if (m < 1 || m > 12 || d < 1 || d > 31) continue;
    if (today - DaysFromCivil(y, m, d) > keepDays) out.push_back(n);
  }
  return out;
}

void OpenLogFile() {
  std::wstring dir = Paths().Logs();
  if (!EnsureDir(dir)) return;
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::vector<std::wstring> names;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\wasd-*.log").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do names.push_back(fd.cFileName);
    while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  for (const auto& old : OldLogs(names, st.wYear, st.wMonth, st.wDay, kLogKeepDays)) DeleteFileW((dir + L"\\" + old).c_str());
  g_logFile = _wfsopen((dir + L"\\" + LogFileName(st.wYear, st.wMonth, st.wDay)).c_str(), L"a", _SH_DENYWR);
}

void AdoptNewMapData();  // defined with the map-data tables

DWORD FindProcessIdByName(const wchar_t* name) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  DWORD pid = 0;
  if (Process32FirstW(snap, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, name) == 0) {
        pid = entry.th32ProcessID;
        break;
      }
    } while (Process32NextW(snap, &entry));
  }
  CloseHandle(snap);
  return pid;
}

std::vector<std::wstring> ListModuleNames(DWORD pid) {
  std::vector<std::wstring> result;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE) return result;

  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Module32FirstW(snap, &entry)) {
    do {
      result.push_back(entry.szModule);
    } while (Module32NextW(snap, &entry));
  }
  CloseHandle(snap);
  return result;
}

bool HasModule(const std::vector<std::wstring>& modules, const wchar_t* name) {
  for (const auto& m : modules) {
    if (_wcsicmp(m.c_str(), name) == 0) return true;
  }
  return false;
}

uintptr_t GetModuleBase(DWORD pid, const wchar_t* moduleName) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE) return 0;

  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  uintptr_t base = 0;
  if (Module32FirstW(snap, &entry)) {
    do {
      if (_wcsicmp(entry.szModule, moduleName) == 0) {
        base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
        break;
      }
    } while (Module32NextW(snap, &entry));
  }
  CloseHandle(snap);
  return base;
}

// Returns {base, size} of moduleName within pid's address space, or
// {0, 0} if not found.
std::pair<uintptr_t, SIZE_T> GetModuleBaseAndSize(DWORD pid, const wchar_t* moduleName) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE) return {0, 0};

  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  std::pair<uintptr_t, SIZE_T> result{0, 0};
  if (Module32FirstW(snap, &entry)) {
    do {
      if (_wcsicmp(entry.szModule, moduleName) == 0) {
        result = {reinterpret_cast<uintptr_t>(entry.modBaseAddr), entry.modBaseSize};
        break;
      }
    } while (Module32NextW(snap, &entry));
  }
  CloseHandle(snap);
  return result;
}

// Finds the process, verifies the Seamless Co-op marker module, and
// opens a read-only handle (PROCESS_VM_READ | PROCESS_QUERY_INFORMATION
// only). Returns nullptr and logs why on any failure -- callers must
// not proceed to read anything if this returns nullptr.
HANDLE AttachReadOnly(DWORD* outPid) {
  DWORD pid = FindProcessIdByName(kTargetProcessName);
  if (pid == 0) {
    Log("Process '%ls' not found. Launch DS3 via Seamless Co-op and retry.", kTargetProcessName);
    return nullptr;
  }
  Log("Found %ls (PID %lu)", kTargetProcessName, pid);

  auto modules = ListModuleNames(pid);
  if (!HasModule(modules, kSeamlessCoopModuleMarker)) {
    Log("SAFETY ABORT: '%ls' not found in the target process's module list. Refusing to "
        "attach -- this may be vanilla online DS3.",
        kSeamlessCoopModuleMarker);
    return nullptr;
  }
  Log("Verified '%ls' is loaded -- safe to attach.", kSeamlessCoopModuleMarker);

  HANDLE hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
  if (!hProcess) {
    Log("OpenProcess failed (error %lu).", GetLastError());
    return nullptr;
  }
  Log("Opened read-only handle (PROCESS_VM_READ | PROCESS_QUERY_INFORMATION).");

  if (outPid) *outPid = pid;
  return hProcess;
}

// ---- probe mode -----------------------------------------------------

int RunProbe(bool listModules) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;

  if (listModules) {
    auto modules = ListModuleNames(pid);
    Log("Loaded modules (%zu):", modules.size());
    for (const auto& m : modules) {
      std::printf("    %ls\n", m.c_str());
    }
  }

  uintptr_t base = GetModuleBase(pid, kTargetProcessName);
  if (base == 0) {
    Log("Could not resolve module base address.");
    CloseHandle(hProcess);
    return 4;
  }
  Log("Module base address: 0x%p", reinterpret_cast<void*>(base));

  Log("Performing 5 timed reads of the module's PE header (MZ signature)...");
  for (int i = 0; i < 5; ++i) {
    unsigned char buf[2] = {0, 0};
    SIZE_T bytesRead = 0;

    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    BOOL ok = ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(base), buf, sizeof(buf),
                                 &bytesRead);
    QueryPerformanceCounter(&end);

    double micros = (end.QuadPart - start.QuadPart) * 1000000.0 / freq.QuadPart;

    if (ok && bytesRead == sizeof(buf)) {
      Log("read #%d: bytes=0x%02X%02X (%s) latency=%.3fus", i + 1, buf[0], buf[1],
          (buf[0] == 'M' && buf[1] == 'Z') ? "OK: MZ" : "unexpected", micros);
    } else {
      Log("read #%d: FAILED (error %lu) latency=%.3fus", i + 1, GetLastError(), micros);
    }
    Sleep(100);
  }

  CloseHandle(hProcess);
  Log("Done. Handle closed.");
  return 0;
}

// ---- scan / rescan mode ----------------------------------------------

// Scans committed, read-write, private (heap) memory for 4-byte-aligned
// int32 cells equal to `value`. Read-only: uses VirtualQueryEx +
// ReadProcessMemory only, never touches the target's memory.
std::vector<uintptr_t> ScanForInt32(HANDLE hProcess, int32_t value) {
  std::vector<uintptr_t> hits;

  SYSTEM_INFO sysInfo{};
  GetSystemInfo(&sysInfo);

  auto* addr = reinterpret_cast<unsigned char*>(sysInfo.lpMinimumApplicationAddress);
  auto* maxAddr = reinterpret_cast<unsigned char*>(sysInfo.lpMaximumApplicationAddress);

  constexpr SIZE_T kChunk = 1 << 20;  // 1 MiB, multiple of 4 -> no split matches across chunks
  std::vector<unsigned char> buf(kChunk);

  while (addr < maxAddr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQueryEx(hProcess, addr, &mbi, sizeof(mbi)) == 0) break;

    bool committed = mbi.State == MEM_COMMIT;
    bool isPrivate = mbi.Type == MEM_PRIVATE;
    bool writable = (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                                     PAGE_EXECUTE_WRITECOPY)) != 0;
    bool guarded = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;

    if (committed && isPrivate && writable && !guarded) {
      auto* regionStart = reinterpret_cast<unsigned char*>(mbi.BaseAddress);
      SIZE_T remaining = mbi.RegionSize;
      SIZE_T offsetInRegion = 0;

      while (remaining > 0) {
        SIZE_T thisChunk = remaining < kChunk ? remaining : kChunk;
        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(hProcess, regionStart + offsetInRegion, buf.data(), thisChunk,
                               &bytesRead)) {
          for (SIZE_T i = 0; i + sizeof(int32_t) <= bytesRead; i += sizeof(int32_t)) {
            int32_t v;
            std::memcpy(&v, buf.data() + i, sizeof(v));
            if (v == value) {
              hits.push_back(reinterpret_cast<uintptr_t>(regionStart + offsetInRegion + i));
            }
          }
        }
        offsetInRegion += thisChunk;
        remaining -= thisChunk;
      }
    }

    addr = reinterpret_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
  }

  return hits;
}

void SaveCandidates(const std::vector<uintptr_t>& addrs) {
  EnsureDir(Paths().ScratchDir());
  std::ofstream out(CandidatesPath(), std::ios::trunc);
  for (auto a : addrs) {
    out << "0x" << std::hex << a << "\n";
  }
}

std::vector<uintptr_t> LoadCandidates() {
  std::vector<uintptr_t> addrs;
  std::ifstream in(CandidatesPath());
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    addrs.push_back(static_cast<uintptr_t>(std::strtoull(line.c_str(), nullptr, 16)));
  }
  return addrs;
}

int RunScan(int32_t value) {
  HANDLE hProcess = AttachReadOnly(nullptr);
  if (!hProcess) return 2;

  Log("Scanning committed/private/read-write memory for int32 value %d ...", value);
  LARGE_INTEGER freq, start, end;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  auto hits = ScanForInt32(hProcess, value);
  QueryPerformanceCounter(&end);
  double seconds = (end.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);

  SaveCandidates(hits);
  Log("Found %zu candidate address(es) in %.2fs. Saved to %ls", hits.size(), seconds,
      CandidatesPath().c_str());
  for (size_t i = 0; i < hits.size() && i < 20; ++i) {
    std::printf("    0x%llx\n", static_cast<unsigned long long>(hits[i]));
  }
  if (hits.size() > 20) std::printf("    ... and %zu more\n", hits.size() - 20);

  CloseHandle(hProcess);
  if (hits.empty()) {
    Log("No matches. Double check the HP value you passed matches the HUD exactly.");
    return 5;
  }
  if (hits.size() > 1) {
    Log("More than one candidate -- change your HP in-game (take a hit / heal) and run "
        "'rescan <newValue>' to narrow it down.");
  } else {
    Log("Exactly one candidate -- you can 'watch 0x%llx' now to verify it live against the HUD.",
        static_cast<unsigned long long>(hits[0]));
  }
  return 0;
}

int RunRescan(int32_t value) {
  auto candidates = LoadCandidates();
  if (candidates.empty()) {
    Log("No candidates file (or it's empty) at %ls. Run 'scan <value>' first.", CandidatesPath().c_str());
    return 6;
  }
  Log("Loaded %zu candidate(s) from %ls", candidates.size(), CandidatesPath().c_str());

  HANDLE hProcess = AttachReadOnly(nullptr);
  if (!hProcess) return 2;

  std::vector<uintptr_t> survivors;
  for (auto addr : candidates) {
    int32_t v = 0;
    SIZE_T bytesRead = 0;
    if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), &v, sizeof(v), &bytesRead) &&
        bytesRead == sizeof(v) && v == value) {
      survivors.push_back(addr);
    }
  }
  CloseHandle(hProcess);

  SaveCandidates(survivors);
  Log("%zu of %zu candidate(s) still equal %d. Saved to %s", survivors.size(), candidates.size(),
      value, CandidatesPath().c_str());
  for (size_t i = 0; i < survivors.size() && i < 20; ++i) {
    std::printf("    0x%llx\n", static_cast<unsigned long long>(survivors[i]));
  }

  if (survivors.empty()) {
    Log("No survivors -- the new value didn't match any candidate. Re-run 'scan <value>' fresh "
        "with the HP value currently on your HUD.");
    return 5;
  }
  if (survivors.size() > 1) {
    Log("Still more than one -- change HP again and rescan.");
  } else {
    Log("Narrowed to one address -- 'watch 0x%llx' to verify it live against the HUD.",
        static_cast<unsigned long long>(survivors[0]));
  }
  return 0;
}

// ---- fscan / frescan mode (float32 variant, for equip load / item
// discovery / poise -- values with no source lead in any reference
// project checked, so the only way in is the same value-scan technique
// that originally found HP, just for float32 instead of int32) --------

std::wstring FCandidatesPath() { return Paths().Scratch(L"fcandidates.txt"); }
constexpr float kDefaultFloatTolerance = 0.05f;

// Same scan as ScanForInt32, but for float32 cells within `tolerance`
// of `value` (floats displayed with limited decimal precision on the
// HUD rarely match the underlying binary value bit-for-bit, so an
// exact-equality scan would miss real hits). Read-only throughout.
std::vector<uintptr_t> ScanForFloat32(HANDLE hProcess, float value, float tolerance) {
  std::vector<uintptr_t> hits;

  SYSTEM_INFO sysInfo{};
  GetSystemInfo(&sysInfo);

  auto* addr = reinterpret_cast<unsigned char*>(sysInfo.lpMinimumApplicationAddress);
  auto* maxAddr = reinterpret_cast<unsigned char*>(sysInfo.lpMaximumApplicationAddress);

  constexpr SIZE_T kChunk = 1 << 20;
  std::vector<unsigned char> buf(kChunk);

  while (addr < maxAddr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQueryEx(hProcess, addr, &mbi, sizeof(mbi)) == 0) break;

    bool committed = mbi.State == MEM_COMMIT;
    bool isPrivate = mbi.Type == MEM_PRIVATE;
    bool writable = (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                                     PAGE_EXECUTE_WRITECOPY)) != 0;
    bool guarded = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;

    if (committed && isPrivate && writable && !guarded) {
      auto* regionStart = reinterpret_cast<unsigned char*>(mbi.BaseAddress);
      SIZE_T remaining = mbi.RegionSize;
      SIZE_T offsetInRegion = 0;

      while (remaining > 0) {
        SIZE_T thisChunk = remaining < kChunk ? remaining : kChunk;
        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(hProcess, regionStart + offsetInRegion, buf.data(), thisChunk,
                               &bytesRead)) {
          for (SIZE_T i = 0; i + sizeof(float) <= bytesRead; i += sizeof(float)) {
            float v;
            std::memcpy(&v, buf.data() + i, sizeof(v));
            if (std::fabs(v - value) <= tolerance) {
              hits.push_back(reinterpret_cast<uintptr_t>(regionStart + offsetInRegion + i));
            }
          }
        }
        offsetInRegion += thisChunk;
        remaining -= thisChunk;
      }
    }

    addr = reinterpret_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
  }

  return hits;
}

int RunFScan(float value, float tolerance) {
  HANDLE hProcess = AttachReadOnly(nullptr);
  if (!hProcess) return 2;

  Log("Scanning committed/private/read-write memory for float32 value %.4f +/- %.4f ...", value,
      tolerance);
  LARGE_INTEGER freq, start, end;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  auto hits = ScanForFloat32(hProcess, value, tolerance);
  QueryPerformanceCounter(&end);
  double seconds = (end.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);

  EnsureDir(Paths().ScratchDir());
  std::ofstream out(FCandidatesPath(), std::ios::trunc);
  for (auto a : hits) out << "0x" << std::hex << a << "\n";

  Log("Found %zu candidate address(es) in %.2fs. Saved to %s", hits.size(), seconds,
      FCandidatesPath().c_str());
  for (size_t i = 0; i < hits.size() && i < 20; ++i) {
    std::printf("    0x%llx\n", static_cast<unsigned long long>(hits[i]));
  }
  if (hits.size() > 20) std::printf("    ... and %zu more\n", hits.size() - 20);

  CloseHandle(hProcess);
  if (hits.empty()) {
    Log("No matches. Double check the value you passed matches the status screen exactly, or "
        "widen the tolerance.");
    return 5;
  }
  if (hits.size() > 1) {
    Log("More than one candidate -- change the value in-game and run 'frescan <newValue>' to "
        "narrow it down.");
  } else {
    Log("Exactly one candidate -- 'watch 0x%llx --float' to verify it live.",
        static_cast<unsigned long long>(hits[0]));
  }
  return 0;
}

int RunFRescan(float value, float tolerance) {
  std::vector<uintptr_t> candidates;
  {
    std::ifstream in(FCandidatesPath());
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      candidates.push_back(static_cast<uintptr_t>(std::strtoull(line.c_str(), nullptr, 16)));
    }
  }
  if (candidates.empty()) {
    Log("No candidates file (or it's empty) at %ls. Run 'fscan <value>' first.", FCandidatesPath().c_str());
    return 6;
  }
  Log("Loaded %zu candidate(s) from %ls", candidates.size(), FCandidatesPath().c_str());

  HANDLE hProcess = AttachReadOnly(nullptr);
  if (!hProcess) return 2;

  std::vector<uintptr_t> survivors;
  for (auto addr : candidates) {
    float v = 0.0f;
    SIZE_T bytesRead = 0;
    if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), &v, sizeof(v), &bytesRead) &&
        bytesRead == sizeof(v) && std::fabs(v - value) <= tolerance) {
      survivors.push_back(addr);
    }
  }
  CloseHandle(hProcess);

  EnsureDir(Paths().ScratchDir());
  std::ofstream out(FCandidatesPath(), std::ios::trunc);
  for (auto a : survivors) out << "0x" << std::hex << a << "\n";

  Log("%zu of %zu candidate(s) still match %.4f +/- %.4f. Saved to %s", survivors.size(),
      candidates.size(), value, tolerance, FCandidatesPath().c_str());
  for (size_t i = 0; i < survivors.size() && i < 20; ++i) {
    std::printf("    0x%llx\n", static_cast<unsigned long long>(survivors[i]));
  }

  if (survivors.empty()) {
    Log("No survivors -- re-run 'fscan <value>' fresh with the value currently on your status "
        "screen.");
    return 5;
  }
  if (survivors.size() > 1) {
    Log("Still more than one -- change the value again and frescan.");
  } else {
    Log("Narrowed to one address -- 'watch 0x%llx --float' to verify it live.",
        static_cast<unsigned long long>(survivors[0]));
  }
  return 0;
}

int RunWatch(uintptr_t address, int iterations, bool asFloat) {
  HANDLE hProcess = AttachReadOnly(nullptr);
  if (!hProcess) return 2;

  Log("Watching 0x%llx for %d reads as %s (Ctrl+C to stop early)...",
      static_cast<unsigned long long>(address), iterations, asFloat ? "float32" : "int32");

  for (int i = 0; i < iterations; ++i) {
    unsigned char raw[4] = {};
    SIZE_T bytesRead = 0;

    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    BOOL ok = ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(address), raw, sizeof(raw),
                                 &bytesRead);
    QueryPerformanceCounter(&end);
    double micros = (end.QuadPart - start.QuadPart) * 1000000.0 / freq.QuadPart;

    if (ok && bytesRead == sizeof(raw)) {
      if (asFloat) {
        float v = 0.0f;
        std::memcpy(&v, raw, sizeof(v));
        Log("value=%.4f latency=%.3fus", v, micros);
      } else {
        int32_t v = 0;
        std::memcpy(&v, raw, sizeof(v));
        Log("value=%d latency=%.3fus", v, micros);
      }
    } else {
      Log("read FAILED (error %lu) latency=%.3fus", GetLastError(), micros);
    }
    Sleep(100);
  }

  CloseHandle(hProcess);
  Log("Done. Handle closed.");
  return 0;
}

// ---- findptr / resolve mode ------------------------------------------

struct PtrHit {
  uintptr_t cellAddr;    // address of the pointer cell itself (P)
  uintptr_t cellValue;   // value stored there (V, a pointer)
  uintptr_t structOffset;  // target - V; how far past V the target sits
};

std::wstring PtrHitsPath() { return Paths().Scratch(L"ptrhits.txt"); }

// Scans all committed, readable, non-guarded memory for 8-byte-aligned
// pointer cells whose value lands at-or-just-before `target` (within
// maxBackOffset bytes). Read-only: VirtualQueryEx + ReadProcessMemory
// only.
std::vector<PtrHit> ScanForPointersNear(HANDLE hProcess, uintptr_t target,
                                         uintptr_t maxBackOffset) {
  std::vector<PtrHit> hits;

  SYSTEM_INFO sysInfo{};
  GetSystemInfo(&sysInfo);
  auto* addr = reinterpret_cast<unsigned char*>(sysInfo.lpMinimumApplicationAddress);
  auto* maxAddr = reinterpret_cast<unsigned char*>(sysInfo.lpMaximumApplicationAddress);

  constexpr SIZE_T kChunk = 1 << 20;
  std::vector<unsigned char> buf(kChunk);

  while (addr < maxAddr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQueryEx(hProcess, addr, &mbi, sizeof(mbi)) == 0) break;

    bool committed = mbi.State == MEM_COMMIT;
    bool readable = (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                                     PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                                     PAGE_EXECUTE_WRITECOPY)) != 0;
    bool guarded = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;

    if (committed && readable && !guarded) {
      auto* regionStart = reinterpret_cast<unsigned char*>(mbi.BaseAddress);
      SIZE_T remaining = mbi.RegionSize;
      SIZE_T offsetInRegion = 0;

      while (remaining > 0) {
        SIZE_T thisChunk = remaining < kChunk ? remaining : kChunk;
        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(hProcess, regionStart + offsetInRegion, buf.data(), thisChunk,
                               &bytesRead)) {
          for (SIZE_T i = 0; i + sizeof(uint64_t) <= bytesRead; i += sizeof(uint64_t)) {
            uint64_t raw;
            std::memcpy(&raw, buf.data() + i, sizeof(raw));
            uintptr_t val = static_cast<uintptr_t>(raw);
            if (val <= target && (target - val) <= maxBackOffset) {
              hits.push_back({reinterpret_cast<uintptr_t>(regionStart + offsetInRegion + i), val,
                               target - val});
            }
          }
        }
        offsetInRegion += thisChunk;
        remaining -= thisChunk;
      }
    }

    addr = reinterpret_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
  }

  return hits;
}

void SavePtrHits(const std::vector<PtrHit>& hits) {
  EnsureDir(Paths().ScratchDir());
  std::ofstream out(PtrHitsPath(), std::ios::trunc);
  for (const auto& h : hits) {
    out << "0x" << std::hex << h.cellAddr << " 0x" << h.cellValue << " 0x" << h.structOffset
        << "\n";
  }
}

int RunFindPtr(uintptr_t target, uintptr_t maxBackOffset) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;

  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  if (moduleBase == 0) {
    Log("Could not resolve module base/size.");
    CloseHandle(hProcess);
    return 4;
  }
  Log("Module range: 0x%llx - 0x%llx (size 0x%zx)", static_cast<unsigned long long>(moduleBase),
      static_cast<unsigned long long>(moduleBase + moduleSize), moduleSize);

  Log("Scanning for pointers within 0x%llx bytes before 0x%llx ...",
      static_cast<unsigned long long>(maxBackOffset), static_cast<unsigned long long>(target));
  LARGE_INTEGER freq, start, end;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  auto hits = ScanForPointersNear(hProcess, target, maxBackOffset);
  QueryPerformanceCounter(&end);
  double seconds = (end.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);

  // STATIC hits (inside the module) first, then heap hits. Within each
  // group, smallest struct offset first (more likely a real struct base).
  std::sort(hits.begin(), hits.end(), [&](const PtrHit& a, const PtrHit& b) {
    bool aStatic = a.cellAddr >= moduleBase && a.cellAddr < moduleBase + moduleSize;
    bool bStatic = b.cellAddr >= moduleBase && b.cellAddr < moduleBase + moduleSize;
    if (aStatic != bStatic) return aStatic > bStatic;
    return a.structOffset < b.structOffset;
  });

  SavePtrHits(hits);
  Log("Found %zu pointer hit(s) in %.2fs. Saved to %ls", hits.size(), seconds, PtrHitsPath().c_str());

  size_t staticCount = 0;
  for (size_t i = 0; i < hits.size() && i < 30; ++i) {
    const auto& h = hits[i];
    bool isStatic = h.cellAddr >= moduleBase && h.cellAddr < moduleBase + moduleSize;
    if (isStatic) {
      ++staticCount;
      std::printf(
          "    STATIC cell=0x%llx (DarkSoulsIII.exe+0x%llx) -> resolve 0x%llx 0x%llx\n",
          static_cast<unsigned long long>(h.cellAddr),
          static_cast<unsigned long long>(h.cellAddr - moduleBase),
          static_cast<unsigned long long>(h.cellAddr - moduleBase),
          static_cast<unsigned long long>(h.structOffset));
    } else {
      std::printf("    heap   cell=0x%llx value=0x%llx offset=0x%llx\n",
                  static_cast<unsigned long long>(h.cellAddr),
                  static_cast<unsigned long long>(h.cellValue),
                  static_cast<unsigned long long>(h.structOffset));
    }
  }
  if (hits.size() > 30) std::printf("    ... and %zu more (see %ls)\n", hits.size() - 30,
                                     PtrHitsPath().c_str());

  CloseHandle(hProcess);

  if (staticCount > 0) {
    Log("%zu STATIC hit(s) found -- try the suggested `resolve` command above.", staticCount);
  } else if (!hits.empty()) {
    Log("No STATIC hits -- all candidates are heap addresses. Pick one 'cell=' address above "
        "and run findptr on IT to go one level deeper (chain gets one hop longer).");
  } else {
    Log("No hits at all. Try a larger max back-offset (default 0x800), or re-derive the "
        "target address with scan/rescan -- it may be stale.");
  }
  return 0;
}

int RunResolve(const std::vector<uintptr_t>& offsets) {
  if (offsets.empty()) {
    std::fwprintf(stderr, L"resolve needs at least one offset\n");
    return 64;
  }

  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;

  uintptr_t moduleBase = GetModuleBase(pid, kTargetProcessName);
  if (moduleBase == 0) {
    Log("Could not resolve module base address.");
    CloseHandle(hProcess);
    return 4;
  }
  Log("Module base: 0x%llx", static_cast<unsigned long long>(moduleBase));

  for (int iter = 0; iter < 30; ++iter) {
    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    uintptr_t addr = moduleBase + offsets[0];
    bool ok = true;
    std::string chainDesc = "moduleBase+0x" + [&] {
      char b[32];
      std::snprintf(b, sizeof(b), "%llx", static_cast<unsigned long long>(offsets[0]));
      return std::string(b);
    }();

    for (size_t i = 1; i < offsets.size() && ok; ++i) {
      uintptr_t ptr = 0;
      SIZE_T bytesRead = 0;
      ok = ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), &ptr, sizeof(ptr),
                              &bytesRead) &&
           bytesRead == sizeof(ptr);
      if (!ok) {
        Log("chain broken: failed to dereference 0x%llx (hop %zu)",
            static_cast<unsigned long long>(addr), i);
        break;
      }
      addr = ptr + offsets[i];
    }

    QueryPerformanceCounter(&end);
    double micros = (end.QuadPart - start.QuadPart) * 1000000.0 / freq.QuadPart;

    if (!ok) {
      Sleep(100);
      continue;
    }

    int32_t v = 0;
    SIZE_T bytesRead = 0;
    BOOL readOk = ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), &v, sizeof(v),
                                     &bytesRead);
    if (readOk && bytesRead == sizeof(v)) {
      Log("value=%d (final addr 0x%llx) latency=%.3fus", v,
          static_cast<unsigned long long>(addr), micros);
    } else {
      Log("final read FAILED at 0x%llx (error %lu) latency=%.3fus",
          static_cast<unsigned long long>(addr), GetLastError(), micros);
    }
    Sleep(100);
  }

  CloseHandle(hProcess);
  Log("Done. Handle closed.");
  return 0;
}

// ---- memory roots: our own byte patterns -------
//
// Every global the tool reads (WorldChrMan, GameDataMan "BaseA", GameMan,
// EventFlagMan, FieldArea, the param master) is a pointer cell the game's
// code loads RIP-relatively, and XA is a struct offset it uses as
// [reg+disp32]. Each pattern below is one such instruction in the game's
// own code plus the straight-line code after it, found by
// tools/find_patterns.py in a read-only copy of the loaded module
// (`dumpmodule`): of all the instructions referring to the root, the one
// whose pattern (16 bytes or more, the instruction's displacement as a
// wildcard, no jumps in the context) matches exactly once. Every pattern
// is REX + opcode + ModRM with the disp32 at +3, so a cell resolves as
// instruction + 7 + disp32 (x86-64 encoding from the Intel / AMD
// manuals). Patterns are searched in the LIVE process's mapped module
// (read-only), so the result is already a runtime address.

struct PatternByte {
  bool wildcard;
  uint8_t value;
};

std::vector<PatternByte> ParsePattern(const char* text) {
  std::vector<PatternByte> pattern;
  std::string s(text);
  size_t pos = 0;
  while (pos < s.size()) {
    while (pos < s.size() && s[pos] == ' ') ++pos;
    size_t end = s.find(' ', pos);
    if (end == std::string::npos) end = s.size();
    std::string tok = s.substr(pos, end - pos);
    if (!tok.empty()) {
      if (tok == "?" || tok == "??") {
        pattern.push_back({true, 0});
      } else {
        pattern.push_back({false, static_cast<uint8_t>(std::strtoul(tok.c_str(), nullptr, 16))});
      }
    }
    pos = end;
  }
  return pattern;
}

// Reads the module's image into a local buffer (gaps left zeroed) and
// searches for `pattern`. Returns the live VA of the match, or 0.
uintptr_t FindPatternInModule(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize,
                               const std::vector<PatternByte>& pattern) {
  std::vector<unsigned char> image(moduleSize, 0);

  auto* moduleBasePtr = reinterpret_cast<unsigned char*>(moduleBase);
  auto* addr = moduleBasePtr;
  auto* moduleEnd = moduleBasePtr + moduleSize;
  while (addr < moduleEnd) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQueryEx(hProcess, addr, &mbi, sizeof(mbi)) == 0) break;

    bool committed = mbi.State == MEM_COMMIT;
    bool guarded = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;
    if (committed && !guarded) {
      auto* regionStart = reinterpret_cast<unsigned char*>(mbi.BaseAddress);
      auto* regionEnd = regionStart + mbi.RegionSize;
      if (regionEnd > moduleEnd) regionEnd = moduleEnd;
      if (regionEnd > regionStart) {
        SIZE_T len = static_cast<SIZE_T>(regionEnd - regionStart);
        SIZE_T bytesRead = 0;
        ReadProcessMemory(hProcess, regionStart, image.data() + (regionStart - moduleBasePtr), len,
                           &bytesRead);
      }
    }
    addr = reinterpret_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
  }

  size_t n = image.size();
  size_t m = pattern.size();
  if (m == 0 || m > n) return 0;
  for (size_t i = 0; i + m <= n; ++i) {
    bool match = true;
    for (size_t j = 0; j < m; ++j) {
      if (!pattern[j].wildcard && image[i + j] != pattern[j].value) {
        match = false;
        break;
      }
    }
    if (match) return moduleBase + i;
  }
  return 0;
}

// mov rcx,[rip+WorldChrMan]; mov rsi,[rdi+60h]; test rcx,rcx; jnz +26h
// (1,903 references to the cell; this one's context is unique).
constexpr const char* kWorldChrManPattern = "48 8B 0D ?? ?? ?? ?? 48 8B 77 60 48 85 C9 75 26";
// mov rcx,[rax+XA]; mov rax,[rcx]; mov eax,[rax+14h]; add rsp,..
// XA is the disp32 itself (1,920 uses).
constexpr const char* kXaPattern = "48 8B 88 ?? ?? ?? ?? 48 8B 01 8B 40 14 48 83 C4";

// The one number in this chain that did NOT come from a byte pattern --
// it came from the open-source DS3RuntimeScripting project's hardcoded
// SprjChrTimeActModule field layout, and was verified live against the
// in-game HUD on 2026-08-17 (HP read as 549, matched). Unlike
// WorldChrMan/XA (re-derived from code bytes every run, so they ride out
// most patches), this is a plain struct-field offset with no
// self-verifying signature behind it -- it's the first thing to
// re-check if `hp`/`wcm` ever disagrees with the HUD after a game
// update. See docs/TECHNICAL.md.
constexpr uintptr_t kHpFieldOffset = 0xD8;

// FP (DS3's "mana", used for spells/Focus Points) and Stamina, from the
// same DS3RuntimeScripting struct layout as kHpFieldOffset: current/max/
// base-max triplets 0xC bytes apart (HP 0xD8-0xE0, FP 0xE4-0xEC, Stamina
// 0xF0-0xF8), all on the SAME struct base as HP. Verified live against
// the HUD alongside HP on 2026-08-17 -- same caveat as kHpFieldOffset
// applies to these two.
constexpr uintptr_t kFpFieldOffset = 0xE4;
constexpr uintptr_t kStaminaFieldOffset = 0xF0;

// Max / base-max siblings of the three "current value" fields above,
// same struct, same triplet layout (current, +4 = max, +8 = base-max).
// Unverified until read live and checked against the HUD/status screen.
constexpr uintptr_t kHpMaxOffset = 0xDC;
constexpr uintptr_t kHpBaseMaxOffset = 0xE0;
constexpr uintptr_t kFpMaxOffset = 0xE8;
constexpr uintptr_t kFpBaseMaxOffset = 0xEC;
constexpr uintptr_t kStaminaMaxOffset = 0xF4;
constexpr uintptr_t kStaminaBaseMaxOffset = 0xF8;

struct ResolvedPointers {
  uintptr_t worldChrManCellOffset = 0;  // DarkSoulsIII.exe + this = pointer cell address
  uintptr_t xaConstant = 0;
  uintptr_t structBase = 0;      // live ChrIns/stat struct base (0 if resolution failed)
  uintptr_t hpAddress = 0;       // structBase + kHpFieldOffset (0 if resolution failed)
  uintptr_t fpAddress = 0;       // structBase + kFpFieldOffset (0 if resolution failed)
  uintptr_t staminaAddress = 0;  // structBase + kStaminaFieldOffset (0 if resolution failed)
  uintptr_t maxHpAddress = 0;
  uintptr_t baseMaxHpAddress = 0;
  uintptr_t maxFpAddress = 0;
  uintptr_t baseMaxFpAddress = 0;
  uintptr_t maxStaminaAddress = 0;
  uintptr_t baseMaxStaminaAddress = 0;
  uintptr_t entityAddress = 0;  // "s1": deref(world_chr_man + 0x80) -- the raw ChrIns pointer
                                 // itself, one hop before chrModulesAddress. DS3RuntimeScripting's
                                 // chr_ins.cpp reads several fields directly off this (not
                                 // through +xa), e.g. weightIndex at +0x50 -> deref -> +0x2B4.
  uintptr_t chrModulesAddress = 0;  // "s2": deref(chrIns + xa) -- container of per-module
                                     // pointers (SprjChrDataModule at +0x18, the physics module at
                                     // +0x68, etc). Exposed so other module fields can reuse
                                     // it without re-walking the chain from scratch.
  uintptr_t physicsModuleAddress = 0;  // deref(s2 + kChrPhysicsModuleSlot), see below
  uintptr_t positionAddress = 0;       // physics + 0x80: float x, y, z (y = height)
  uintptr_t facingAddress = 0;         // physics + 0x74: float, radians
};

// ---- player position + facing --------------------------
//
// Found 2026-10-06 with `memdiff chrins --len 11000 --summary` while the
// user stood still (noise learning), walked forward, then turned on the
// spot. Several copies of one (x, y, z) triple moved together (x 268.5
// -> 245.6, z 605.7 -> 600.9, y flat on level ground); one float went
// 0.946 -> -0.785, a ~99 degree turn in radians. The canonical copy is
// the one in the module at slot +0x68 of the module table (s2): its
// +0x08 points back
// at the player's own ChrIns (an owner pointer), it holds the facing
// angle at +0x74 and the position at +0x80 (+0x8C = 1.0, a homogeneous
// w) -- one physics module, rather than the matrices and caches that
// carry the other copies. Built on the same AOB-derived WorldChrMan/XA
// chain as HP, so no fixed address is involved.
constexpr uintptr_t kChrPhysicsModuleSlot = 0x68;
constexpr uintptr_t kPhysicsFacingOffset = 0x74;
constexpr uintptr_t kPhysicsPositionOffset = 0x80;

struct PlayerPose {
  bool ok = false;
  float x = 0.0f, y = 0.0f, z = 0.0f;
  float facingRadians = 0.0f;
  double FacingDegrees() const { return facingRadians * 57.29577951308232; }
};

// ---- level / souls (separate anchor: BaseA, not WorldChrMan) ----------
//
// Level and souls are save-profile data, not live combat state, so they
// live behind a different global than HP/FP/Stamina, GameDataMan ("BaseA").
// The pattern is our own (see "memory roots" above). The chain is from
// veeenu/darksoulsiii-practice-tool, as facts:
// lib/libds3/src/pointers.rs for the chain shape
// (`character_stats: pointer_chain!(base_a, 0x10, 0x44)`, `souls:
// pointer_chain!(base_a, 0x10, 0x44 + 12 * 4)`). The CharacterStats
// struct's field order (vigor..luck, two unknowns, vitality, level,
// souls) is also straight from that source, and matches
// DS3RuntimeScripting's independent `Attributes` struct field-for-field
// (soulLevel at the same 0x70 offset) -- two independent projects
// agreeing is why level/souls are trusted without a live HUD check on
// every field, though level and souls themselves were still verified
// live (see docs/TECHNICAL.md).
// mov rax,[rip+GameDataMan]; mov rcx,[rax+10h]; movzx edx,word [rcx+10h]
// (889 references; our own, see "memory roots").
constexpr const char* kBaseAPattern = "48 8B 05 ?? ?? ?? ?? 48 8B 48 10 0F B7 51 10 48";
constexpr uintptr_t kCharacterStatsOffset = 0x44;  // from the second dereference, see below
constexpr uintptr_t kVigorOffset = kCharacterStatsOffset + 0x00;
constexpr uintptr_t kAttunementOffset = kCharacterStatsOffset + 0x04;
constexpr uintptr_t kEnduranceOffset = kCharacterStatsOffset + 0x08;
constexpr uintptr_t kStrengthOffset = kCharacterStatsOffset + 0x0C;
constexpr uintptr_t kDexterityOffset = kCharacterStatsOffset + 0x10;
constexpr uintptr_t kIntelligenceOffset = kCharacterStatsOffset + 0x14;
constexpr uintptr_t kFaithOffset = kCharacterStatsOffset + 0x18;
constexpr uintptr_t kLuckOffset = kCharacterStatsOffset + 0x1C;
constexpr uintptr_t kVitalityOffset = kCharacterStatsOffset + 0x28;
constexpr uintptr_t kLevelOffset = kCharacterStatsOffset + 0x2C;  // = 0x70
constexpr uintptr_t kSoulsOffset = kCharacterStatsOffset + 0x30;  // = 0x74

// Equipped item slot IDs (weapons, armor, rings, arrows/bolts,
// covenant). Source: AmySouls/DS3RuntimeScripting's
// EquipGameData::getInventoryItemIdBySlot(), which reads directly (no
// extra dereference) at `(PlayerGameData base) + 0x228 + 0x24 +
// slot*4`. PlayerGameData base is the same "x" this file already
// resolves for CharacterStats (see kCharacterStatsOffset above) --
// DS3RuntimeScripting's getAttributes() reads its Attributes struct
// directly at "address+0x44" with no extra pointer hop, exactly
// matching this file's `x + kCharacterStatsOffset`, which is how we
// know the two "address"/"x" values are the same base.
// Slot order/offsets match their InventorySlot enum exactly
// (PrimaryLeftWep=0 .. TertiaryRightHand=5, PrimaryArrow=6 ..
// SecondaryBolt=9, Head=12, Chest=13, Hands=14, Legs=15, Ring1..4=17..20,
// Covenant=21) -- verified for the weapon slots via two live gear swaps
// (see docs/TECHNICAL.md); armor/ring/arrow/covenant offsets are the same
// formula, not yet independently live-verified.
constexpr uintptr_t kEquipGameDataOffset = 0x228;
// Two-handing state -- from PlayerGameData::getWeaponSheathState() /
// getRightHandSlot() / getLeftHandSlot() in AmySouls/DS3RuntimeScripting's
// player_game_data.cpp, all relative to the same PlayerGameData base as
// EquipGameData above (confirmed: their getEquipGameData() returns
// address+0x228, an exact match to kEquipGameDataOffset already
// independently derived here). Live-verified (2026-08-19), fully
// decoded, not just sourced: baseline (one-handed) read
// WeaponSheathState=1; two-handing R1 changed it to 3; switching back
// to one-handed returned it to 1; two-handing the *left*-hand weapon
// instead gave 2 -- a clean 3-state enum confirmed in both directions
// by the user physically toggling grip live. RightHandSlot/LeftHandSlot
// (which of that side's 3 weapon slots, 0-2, is the currently *active/
// drawn* one -- independent of handedness) stayed 0 throughout testing
// (the character never swapped which weapon was drawn), so the mapping
// from slot index to R1/R2/R3 (or L1/L2/L3) is inferred from the
// source's own "range 0-2" documentation, not independently
// live-verified across all three slots.
//
// CORRECTED 2026-10-06: the hand-slot offsets were swapped. 0x2BC is the
// LEFT hand's active slot and 0x2C0 the RIGHT's. The user noticed the
// companion window's "Right" line changing when they cycled the left
// hand, and vice versa. Every earlier check had both hands on the same
// slot index (0/0, then 1/1, R2 Hand Axe with L2 Caestus), where a swap
// is invisible -- including the two-handed AR check, which only looked
// at slot 1 of each side.
constexpr uintptr_t kWeaponSheathStateOffset = 0x2B8;  // i32: 1=one-handed, 2=left two-handed, 3=right two-handed
constexpr uintptr_t kLeftHandSlotOffset = 0x2BC;       // i32: which L-slot (0-2) is drawn
constexpr uintptr_t kRightHandSlotOffset = 0x2C0;      // i32: which R-slot (0-2) is drawn

// Active quick-item (bottom slot) index -- found via a community Cheat
// Engine table's "m_selectedEquipItemSlotIdx" entry (Address:
// GameDataMan, Offsets: [0x4E0, 0x10]), calibrated against the SAME
// table's own Lua scripts, which explicitly show `PlayerGameData =
// readPointer(GameDataMan + 0x10)` -- exactly this project's own
// `profile.xBase`. That means the CE entry's trailing 0x10 offset is
// the SAME hop already baked into xBase, leaving a single offset
// relative to xBase: +0x4E0. Live-verified (2026-08-19), both states:
// read 0, then read 1 after the user cycled their active quick item
// live -- exact match. The quick-item bar's actual CONTENTS (which
// item occupies each of the 10 slots) come from EquipGameData's own
// array, `EquipGameData::getInventoryItemIdByQuickSlot()` in
// AmySouls/DS3RuntimeScripting -- equipGameData+0x26C+slot*8, i.e.
// xBase+kEquipGameDataOffset+0x26C+slot*8 (not independently
// live-verified this session, but the same offset family already
// validated for weapon/armor/ring slots at kEquipSlotBaseOffset).
constexpr uintptr_t kSelectedQuickItemSlotOffset = 0x4E0;  // i32, active quick-item bar slot (0-9)
constexpr uintptr_t kQuickItemArrayOffset = kEquipGameDataOffset + 0x26C;  // = 0x494, from xBase
constexpr uintptr_t kQuickItemStride = 8;

// Active SPELL (top slot) index -- NOT FOUND, a genuine open gap, not
// just unattempted. A live memory diff across a 12KB window from
// PlayerGameData (xBase) while the user switched their active spell
// between all 3 attuned spells showed ZERO byte differences anywhere
// in that range -- ruled out of PlayerGameData entirely, not merely
// unfound. Most likely lives in the live combat-state structure
// instead (the same one HP/FP/Stamina/poise come from, resolved via
// WorldChrMan/s1/s2 elsewhere in this file), which would need its own
// fresh investigation. Left undone -- see docs/TECHNICAL.md.
// EquipInventoryData sits inside EquipGameData -- see the "Equipped
// item identity" investigation further down this file/in docs/TECHNICAL.md for
// how this offset and the two-segment item array were found.
constexpr uintptr_t kEquipInventoryDataOffset = 0x1A8;  // from EquipGameData base
constexpr uintptr_t kEquipSlotBaseOffset = kEquipGameDataOffset + 0x24;  // = 0x24C
constexpr uintptr_t kLWeapon1Offset = kEquipSlotBaseOffset + 0 * 4;
constexpr uintptr_t kRWeapon1Offset = kEquipSlotBaseOffset + 1 * 4;
constexpr uintptr_t kLWeapon2Offset = kEquipSlotBaseOffset + 2 * 4;
constexpr uintptr_t kRWeapon2Offset = kEquipSlotBaseOffset + 3 * 4;
constexpr uintptr_t kLWeapon3Offset = kEquipSlotBaseOffset + 4 * 4;
constexpr uintptr_t kRWeapon3Offset = kEquipSlotBaseOffset + 5 * 4;
constexpr uintptr_t kPrimaryArrowOffset = kEquipSlotBaseOffset + 6 * 4;
constexpr uintptr_t kPrimaryBoltOffset = kEquipSlotBaseOffset + 7 * 4;
constexpr uintptr_t kSecondaryArrowOffset = kEquipSlotBaseOffset + 8 * 4;
constexpr uintptr_t kSecondaryBoltOffset = kEquipSlotBaseOffset + 9 * 4;
constexpr uintptr_t kHeadOffset = kEquipSlotBaseOffset + 12 * 4;
constexpr uintptr_t kChestOffset = kEquipSlotBaseOffset + 13 * 4;
constexpr uintptr_t kHandsOffset = kEquipSlotBaseOffset + 14 * 4;
constexpr uintptr_t kLegsOffset = kEquipSlotBaseOffset + 15 * 4;
constexpr uintptr_t kRing1Offset = kEquipSlotBaseOffset + 17 * 4;
constexpr uintptr_t kRing2Offset = kEquipSlotBaseOffset + 18 * 4;
constexpr uintptr_t kRing3Offset = kEquipSlotBaseOffset + 19 * 4;
constexpr uintptr_t kRing4Offset = kEquipSlotBaseOffset + 20 * 4;
constexpr uintptr_t kCovenantOffset = kEquipSlotBaseOffset + 21 * 4;

struct ResolvedProfile {
  uintptr_t baseACellOffset = 0;  // DarkSoulsIII.exe + this = pointer cell address
  uintptr_t xBase = 0;  // PlayerGameData base ("x") -- 0 if resolution failed
  uintptr_t characterStatsBase = 0;  // 0 if resolution failed
  uintptr_t levelAddress = 0;
  uintptr_t soulsAddress = 0;
  uintptr_t vigorAddress = 0;
  uintptr_t attunementAddress = 0;
  uintptr_t enduranceAddress = 0;
  uintptr_t strengthAddress = 0;
  uintptr_t dexterityAddress = 0;
  uintptr_t intelligenceAddress = 0;
  uintptr_t faithAddress = 0;
  uintptr_t luckAddress = 0;
  uintptr_t vitalityAddress = 0;
  uintptr_t lWeapon1Address = 0;
  uintptr_t rWeapon1Address = 0;
  uintptr_t lWeapon2Address = 0;
  uintptr_t rWeapon2Address = 0;
  uintptr_t lWeapon3Address = 0;
  uintptr_t rWeapon3Address = 0;
  uintptr_t primaryArrowAddress = 0;
  uintptr_t primaryBoltAddress = 0;
  uintptr_t secondaryArrowAddress = 0;
  uintptr_t secondaryBoltAddress = 0;
  uintptr_t headAddress = 0;
  uintptr_t chestAddress = 0;
  uintptr_t handsAddress = 0;
  uintptr_t legsAddress = 0;
  uintptr_t ring1Address = 0;
  uintptr_t ring2Address = 0;
  uintptr_t ring3Address = 0;
  uintptr_t ring4Address = 0;
  uintptr_t covenantAddress = 0;
};

// Re-derives BaseA from the live module's own code bytes (same AOB
// technique as ResolvePlayerHp) and walks +0x10 -> +0x44 to the
// CharacterStats struct base. Read-only throughout.
ResolvedProfile ResolvePlayerProfile(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize) {
  ResolvedProfile result;
  SIZE_T br = 0;

  auto baseAPattern = ParsePattern(kBaseAPattern);
  uintptr_t baseAInstr = FindPatternInModule(hProcess, moduleBase, moduleSize, baseAPattern);
  if (baseAInstr == 0) return result;

  int32_t disp = 0;
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(baseAInstr + 3), &disp, sizeof(disp), &br);
  uintptr_t baseACell = static_cast<uintptr_t>(static_cast<intptr_t>(baseAInstr) + 7 + disp);
  result.baseACellOffset = baseACell - moduleBase;

  uintptr_t baseAValue = 0;
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(baseACell), &baseAValue,
                          sizeof(baseAValue), &br) ||
      baseAValue == 0)
    return result;

  uintptr_t x = 0;
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(baseAValue + 0x10), &x, sizeof(x),
                          &br) ||
      x == 0)
    return result;

  result.xBase = x;
  result.characterStatsBase = x + kCharacterStatsOffset;
  result.levelAddress = x + kLevelOffset;
  result.soulsAddress = x + kSoulsOffset;
  result.vigorAddress = x + kVigorOffset;
  result.attunementAddress = x + kAttunementOffset;
  result.enduranceAddress = x + kEnduranceOffset;
  result.strengthAddress = x + kStrengthOffset;
  result.dexterityAddress = x + kDexterityOffset;
  result.intelligenceAddress = x + kIntelligenceOffset;
  result.faithAddress = x + kFaithOffset;
  result.luckAddress = x + kLuckOffset;
  result.vitalityAddress = x + kVitalityOffset;
  result.lWeapon1Address = x + kLWeapon1Offset;
  result.rWeapon1Address = x + kRWeapon1Offset;
  result.lWeapon2Address = x + kLWeapon2Offset;
  result.rWeapon2Address = x + kRWeapon2Offset;
  result.lWeapon3Address = x + kLWeapon3Offset;
  result.rWeapon3Address = x + kRWeapon3Offset;
  result.primaryArrowAddress = x + kPrimaryArrowOffset;
  result.primaryBoltAddress = x + kPrimaryBoltOffset;
  result.secondaryArrowAddress = x + kSecondaryArrowOffset;
  result.secondaryBoltAddress = x + kSecondaryBoltOffset;
  result.headAddress = x + kHeadOffset;
  result.chestAddress = x + kChestOffset;
  result.handsAddress = x + kHandsOffset;
  result.legsAddress = x + kLegsOffset;
  result.ring1Address = x + kRing1Offset;
  result.ring2Address = x + kRing2Offset;
  result.ring3Address = x + kRing3Offset;
  result.ring4Address = x + kRing4Offset;
  result.covenantAddress = x + kCovenantOffset;
  return result;
}

// ---- required souls for next level (COMPUTED, not read from memory) --
//
// Unlike everything else in this file, this isn't a pointer chain --
// DS3 doesn't store "souls needed for next level" anywhere in memory,
// it derives it from current level on the fly using a documented
// formula. Sourced from the community-maintained Fextralife wiki
// (https://darksouls3.wiki.fextralife.com/Level): ~2.5% growth per
// level for levels 2-12, then a cubic for level 13+. Cross-checked
// against that same page's published level->cost table before use
// (level 33 -> formula gives 6640, table says 6640, exact match).
// Still worth confirming against the actual level-up screen next time
// you're at a bonfire -- see docs/TECHNICAL.md.
int32_t RequiredSoulsForLevel(int32_t targetLevel) {
  if (targetLevel < 2) return 0;
  if (targetLevel <= 12) {
    double v = 673.0 * std::pow(1.025, static_cast<double>(targetLevel - 2));
    return static_cast<int32_t>(v);
  }
  double x = static_cast<double>(targetLevel);
  double v = 0.02 * x * x * x + 3.06 * x * x + 105.6 * x - 895.0;
  return static_cast<int32_t>(v);
}

// ---- max equip load (COMPUTED, not read from memory) ------------------
//
// Like required-souls-for-level above, DS3 doesn't store this anywhere
// -- it's derived from Vitality on the fly. Formula sourced from the
// community-maintained Fextralife wiki
// (https://darksouls3.wiki.fextralife.com/Equipment_Load): base max
// load = 40.0 + 1.0 per point of Vitality, capping at 139 when Vitality
// hits its own cap of 99 (40+99=139, an internally consistent check).
// Verified against this session's own live data, not just the wiki:
// this character's Vitality was 8 when the max equip load field was
// found live at exactly 48.0 -- 40+8=48 exactly.
//
// Two rings modify this multiplicatively, not additively (per the same
// wiki page): Havel's Ring (+15/17/18/19% at +0/+1/+2/+3) and Ring of
// Favor (+5/6/7/8% at +0/+1/+2/+3), e.g. both unupgraded stacks as
// 1.15 * 1.05 = 1.2075, not 1.20. This function computes the
// UNMODIFIED base value only -- it does not know how to read a ring's
// reinforcement level from its resolved item data, so rather than
// silently under-report the true max load when one of these rings is
// equipped, callers should check for them first (see
// EquipLoadRingBonusWarning below) and flag the number as incomplete.
double ComputeBaseMaxEquipLoad(int32_t vitality) { return 40.0 + static_cast<double>(vitality); }

// ---- attunement slots (COMPUTED from Attunement, not read) -------------
//
// Same approach as required-souls-for-level/max-equip-load: DS3 doesn't
// store slot count anywhere, it derives it from Attunement on the fly
// via a documented breakpoint table (not a smooth formula), sourced
// from the Fextralife wiki (https://darksouls3.wiki.fextralife.com/Attunement).
// Verified against this session's own live data, not just the wiki:
// this character's Attunement was 14, base slots computed as 2 --
// exactly matching the user's own stated breakdown ("2 from my
// levels"). Ring bonus (e.g. Saint's Ring's +1) is computed separately,
// live, via SpEffectParam -- see ComputeRingAttunementSlotBonus further
// down (near the other ring-SpEffect functions, which this shares its
// accessory/SpEffect resolution machinery with).
int32_t ComputeBaseAttunementSlots(int32_t attunement) {
  if (attunement >= 99) return 10;
  if (attunement >= 80) return 9;
  if (attunement >= 60) return 8;
  if (attunement >= 50) return 7;
  if (attunement >= 40) return 6;
  if (attunement >= 30) return 5;
  if (attunement >= 24) return 4;
  if (attunement >= 18) return 3;
  if (attunement >= 14) return 2;
  if (attunement >= 10) return 1;
  return 0;
}

// ---- live param table reading (SoloParamRepository) -------------------
//
// General-purpose infrastructure: reads DS3's own loaded param tables
// (EquipParamWeapon/Protector/Accessory/Goods, CalcCorrectGraph, etc.)
// directly from live game memory. Read-only throughout -- no code
// execution, just pointer-chasing and struct reads -- and it works for
// *any* param table/row/field, not just poise. Built for poise (see
// ComputeArmorPoise below) but intended for reuse: equip-load precision
// (per-item weight instead of the current live-scanned aggregate),
// Attack Power calculation, or anything else that needs a real stat
// value straight from the game's own data.
//
// Chain and struct layouts sourced from
// veeenu/darksoulsiii-practice-tool's `params/mod.rs` (the `Params`
// type) and `params/param_data.rs` (the per-table field layouts,
// autogenerated from the game's own PARAMDEF). That project is
// AGPL-3.0; only facts are used here (the chain and the offsets it
// documents), reimplemented in C++ -- no code copied. Credited in
// data/THIRD_PARTY_NOTICES.md.
//
// The param master cell is found by our own pattern (see "memory
// roots"): mov rcx,[rip+ParamMaster]; test rcx,rcx; je;
// mov r8,rax (29 references). It used to be a hard-coded 1.15.2-only
// offset (+0x479B8B0); the pattern resolves to that same cell on 1.15.2
// and follows the code on any other build.
constexpr const char* kParamMasterPattern = "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 0B 4C 8B C0 48";

// The loaded module's size from its own PE header (SizeOfImage), for
// callers that only have the base. 0 if the header can't be read.
SIZE_T ModuleSizeFromHeader(HANDLE hProcess, uintptr_t moduleBase) {
  SIZE_T br = 0;
  int32_t peOffset = 0;
  uint32_t sizeOfImage = 0;
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(moduleBase + 0x3C), &peOffset, sizeof(peOffset), &br) ||
      peOffset <= 0 || peOffset > 0x1000)
    return 0;
  // PE signature (4) + file header (20); SizeOfImage is at +56 of the optional header.
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(moduleBase + peOffset + 24 + 56), &sizeOfImage,
                         sizeof(sizeOfImage), &br))
    return 0;
  return sizeOfImage;
}

// Module-relative offset of the param master cell, or 0. Found once per
// module base (one game launch) and remembered.
uintptr_t ParamMasterCellOffset(HANDLE hProcess, uintptr_t moduleBase) {
  static uintptr_t cachedBase = 0, cachedOffset = 0;
  if (moduleBase != 0 && moduleBase == cachedBase) return cachedOffset;
  SIZE_T size = ModuleSizeFromHeader(hProcess, moduleBase);
  uintptr_t instr = size ? FindPatternInModule(hProcess, moduleBase, size, ParsePattern(kParamMasterPattern)) : 0;
  int32_t disp = 0;
  SIZE_T br = 0;
  if (instr == 0 || !ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(instr + 3), &disp, sizeof(disp), &br))
    return 0;
  cachedBase = moduleBase;
  cachedOffset = static_cast<uintptr_t>(static_cast<intptr_t>(instr) + 7 + disp) - moduleBase;
  return cachedOffset;
}

// ParamEntry name field: if param_length <= 7, the name is stored
// directly as up to 8 wchar_t inline; otherwise it's a pointer to up
// to 90 wchar_t elsewhere. See practice-tool's `ParamName`/`ParamEntry`
// (params/mod.rs).
std::wstring ReadParamEntryName(HANDLE hProcess, uintptr_t entryPtr) {
  SIZE_T br = 0;
  int64_t paramLength = 0;
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(entryPtr + 32), &paramLength,
                          sizeof(paramLength), &br))
    return L"";
  wchar_t buf[91] = {};
  if (paramLength <= 7) {
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(entryPtr + 16), buf, 16, &br);
  } else {
    uintptr_t indirect = 0;
    if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(entryPtr + 16), &indirect,
                            sizeof(indirect), &br) ||
        indirect == 0)
      return L"";
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(indirect), buf, 90 * 2, &br);
  }
  return std::wstring(buf);
}

struct ParamTableRef {
  bool ok = false;
  uintptr_t tableBase = 0;
  uint16_t rowCount = 0;
};

// Walks the param master's entry array looking for `tableName` (e.g.
// L"EquipParamProtector"), then follows practice-tool's documented
// double-+0x68 hop to the actual table object, reading its row count
// from +0x0A. Read-only; the master cell is found by pattern once per
// launch, everything after it is re-read on every call.
ParamTableRef ResolveParamTable(HANDLE hProcess, uintptr_t moduleBase, const wchar_t* tableName) {
  ParamTableRef result;
  SIZE_T br = 0;

  uintptr_t masterCell = ParamMasterCellOffset(hProcess, moduleBase);
  uintptr_t masterPtr = 0;
  if (masterCell == 0 ||
      !ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(moduleBase + masterCell),
                          &masterPtr, sizeof(masterPtr), &br) ||
      masterPtr == 0)
    return result;

  uintptr_t start = 0, end = 0;
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(masterPtr + 0x10), &start, sizeof(start),
                     &br);
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(masterPtr + 0x18), &end, sizeof(end), &br);
  if (start == 0 || end == 0 || end < start) return result;

  size_t count = static_cast<size_t>(end - start) / sizeof(uintptr_t);
  std::vector<uintptr_t> entryPtrs(count);
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(start), entryPtrs.data(),
                          count * sizeof(uintptr_t), &br))
    return result;

  for (uintptr_t entryPtr : entryPtrs) {
    if (entryPtr == 0) continue;
    if (ReadParamEntryName(hProcess, entryPtr) != tableName) continue;

    uintptr_t ptr = 0, ptr2 = 0;
    if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(entryPtr + 0x68), &ptr,
                            sizeof(ptr), &br) ||
        ptr == 0)
      continue;
    if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(ptr + 0x68), &ptr2, sizeof(ptr2),
                            &br) ||
        ptr2 == 0)
      continue;
    uint16_t rowCount = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(ptr2 + 0x0A), &rowCount,
                       sizeof(rowCount), &br);

    result.ok = true;
    result.tableBase = ptr2;
    result.rowCount = rowCount;
    return result;
  }
  return result;
}

// Finds a row by its param ID within an already-resolved table (see
// ResolveParamTable). Row offsets live in a {id:i64, offset:isize,
// unk:i64} array (24 bytes/entry) at tableBase+0x40; the row's actual
// data is at tableBase+offset. Bulk-reads the whole offset array in
// one call rather than one ReadProcessMemory per row.
uintptr_t FindParamRow(HANDLE hProcess, const ParamTableRef& table, int64_t paramId) {
  if (!table.ok || table.rowCount == 0) return 0;
  SIZE_T br = 0;
  std::vector<unsigned char> raw(static_cast<size_t>(table.rowCount) * 24);
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(table.tableBase + 0x40), raw.data(),
                          raw.size(), &br))
    return 0;
  for (uint16_t i = 0; i < table.rowCount; ++i) {
    int64_t id = 0, offset = 0;
    std::memcpy(&id, raw.data() + static_cast<size_t>(i) * 24 + 0, 8);
    std::memcpy(&offset, raw.data() + static_cast<size_t>(i) * 24 + 8, 8);
    if (id == paramId) return table.tableBase + static_cast<uintptr_t>(offset);
  }
  return 0;
}

// ---- armor poise (COMPUTED live from EquipParamProtector, not scanned) -
//
// The passive equipment-Poise stat shown nowhere numerically in the
// vanilla UI, but well documented by the community. Two things had to
// be found, not just one:
//
// 1. The raw field at EquipParamProtector+0x110 (named `poise` in
//    practice-tool's autogenerated field layout -- offset computed
//    from that struct, cross-checked live via every neighboring field
//    lining up exactly: weight, damage-cut-rate floats, material IDs,
//    protector_category_id) is NOT the displayed poise number itself.
//    It's the complementary rate: raw = 1 - displayedPoise/100. Found
//    by live-testing: reading it directly for the user's 4 equipped
//    pieces gave 0.9870/0.9890/0.9990/0.9840 -- clustered near 1.0,
//    nothing like the wiki's per-item poise values (1.3/1.1/0.1/1.6),
//    and an exhaustive live scan of the entire row (every byte offset,
//    tolerance 0.02) found NO float anywhere matching those wiki
//    values. But `100 * (1 - raw)` reproduces them exactly: 100*(1-
//    0.987)=1.3, 100*(1-0.989)=1.1, 100*(1-0.999)=0.1, 100*(1-0.984)=
//    1.6. So the field is a poise-damage-taken multiplier (1.0 = no
//    reduction), not a point value, and must be converted before use.
// 2. Those converted per-item point values then combine with
//    diminishing returns, not a plain sum -- equivalent to
//    multiplying the raw rates together and converting once, since
//    (1-a/100)*(1-b/100) = 1-(a+b-ab/100)/100. Formula from the
//    Fextralife wiki (https://darksouls3.wiki.fextralife.com/Poise):
//    combining current total `a` with a new piece's point value `b`
//    gives `a + b - (a*b)/100`. Commutative/associative (same math as
//    combining independent probabilities), order doesn't matter.
//
// Verified against this session's own live data (2026-08-19): Xanthous
// Crown/Conjurator Robe/Conjurator Manchettes/Worker Trousers's raw
// fields (0.9870/0.9890/0.9990/0.9840), converted then combined, give
// 4.0436, rounding to 4.04 -- exactly the value shown on the user's
// status screen, reconfirmed live immediately before this fix. Only
// armor (Head/Chest/Hands/Legs) is summed here -- ring poise bonuses
// (e.g. Wolf Ring) are handled separately, see ComputeRingPoiseRateBonus
// below, since EquipParamAccessory has no `poise` field of its own.
constexpr uintptr_t kEquipParamProtectorPoiseOffset = 0x110;  // computed from the struct layout,
                                                               // see comment above
constexpr int32_t kProtectorGiveIdPrefix = static_cast<int32_t>(0x10000000);  // ItemParamIdPrefix::Protector

double CombinePoise(double a, double b) { return a + b - (a * b) / 100.0; }

struct ArmorPoiseResult {
  bool ok = false;
  double total = 0.0;
};

// giveIds: the 4 armor slots' resolved giveId values (0 or a Protector
// giveId; non-Protector/absent slots are skipped). Resolves
// EquipParamProtector once and looks up each piece's row directly.
ArmorPoiseResult ComputeArmorPoise(HANDLE hProcess, uintptr_t moduleBase,
                                    const std::vector<int32_t>& giveIds) {
  ArmorPoiseResult result;
  auto table = ResolveParamTable(hProcess, moduleBase, L"EquipParamProtector");
  if (!table.ok) return result;

  double total = 0.0;
  for (int32_t giveId : giveIds) {
    if (giveId == 0) continue;
    int64_t rowId = static_cast<int64_t>(giveId) - kProtectorGiveIdPrefix;
    uintptr_t rowAddr = FindParamRow(hProcess, table, rowId);
    if (rowAddr == 0) continue;
    float poiseRate = 0.0f;
    SIZE_T br = 0;
    if (!ReadProcessMemory(hProcess,
                            reinterpret_cast<LPCVOID>(rowAddr + kEquipParamProtectorPoiseOffset),
                            &poiseRate, sizeof(poiseRate), &br))
      continue;
    double poisePoints = 100.0 * (1.0 - static_cast<double>(poiseRate));
    total = CombinePoise(total, poisePoints);
  }
  result.ok = true;
  result.total = total;
  return result;
}

// ---- armor Defense/Absorption (all 8 damage types) --------------------
//
// DS3's status screen shows "Absorption" percentages for Physical/
// Strike/Slash/Thrust/Magic/Fire/Lightning/Dark -- not raw defense
// point totals. EquipParamProtector has BOTH an i16 `defense_X` field
// per damage type AND an f32 `X_damage_cut_rate` field -- live-checked
// (2026-08-19) on Xanthous Crown: every `defense_X` field (phys,
// magic, fire, thunder, slash, blow, thrust) reads exactly `0`, and
// `defense_dark` too. The `_damage_cut_rate` fields are the real,
// populated data -- confirmed already for poise's own field at +0x110,
// which sits right after this same cluster and uses the identical
// "rate near 1.0" convention. So `defense_X` is vestigial/unused in
// this DS3 build, and Absorption is computed here the same way poise
// was: `100 * (1 - raw)` per piece, combined across pieces with the
// same diminishing-returns formula (`CombinePoise`, reused here
// unchanged -- same math, same reasoning: the raw fields are
// multiplicative survival rates, so their derived percentages combine
// the same way poise's did).
//
// Live-verified EXACT MATCH to 3 decimal places, all 8 types at once
// (2026-08-19): computed Phys=9.283 Strike=10.955 Slash=10.304
// Thrust=9.098 Magic=21.725 Fire=21.231 Lightning=22.980 Dark=24.361
// against the user's own in-game Stats screen readings of the exact
// same 8 numbers to 3 decimal places -- not rounded-off agreement,
// bit-for-bit identical at the precision the game itself displays.
constexpr uintptr_t kEquipParamProtectorPhysCutRateOffset = 0xE0;
constexpr uintptr_t kEquipParamProtectorSlashCutRateOffset = 0xE4;
constexpr uintptr_t kEquipParamProtectorStrikeCutRateOffset = 0xE8;
constexpr uintptr_t kEquipParamProtectorThrustCutRateOffset = 0xEC;
constexpr uintptr_t kEquipParamProtectorMagicCutRateOffset = 0xF0;
constexpr uintptr_t kEquipParamProtectorFireCutRateOffset = 0xF4;
constexpr uintptr_t kEquipParamProtectorThunderCutRateOffset = 0xF8;  // Lightning
constexpr uintptr_t kEquipParamProtectorDarkCutRateOffset = 0x118;

struct ArmorDefenseResult {
  bool ok = false;
  double phys = 0.0, slash = 0.0, strike = 0.0, thrust = 0.0;
  double magic = 0.0, fire = 0.0, lightning = 0.0, dark = 0.0;
};

// giveIds: the 4 armor slots' resolved giveId values, same as
// ComputeArmorPoise. UNVERIFIED against the live status screen --
// same transform/combine math as poise (already live-verified), but
// applied to 8 new fields for the first time.
ArmorDefenseResult ComputeArmorDefense(HANDLE hProcess, uintptr_t moduleBase,
                                        const std::vector<int32_t>& giveIds) {
  ArmorDefenseResult result;
  auto table = ResolveParamTable(hProcess, moduleBase, L"EquipParamProtector");
  if (!table.ok) return result;

  double phys = 0.0, slash = 0.0, strike = 0.0, thrust = 0.0;
  double magic = 0.0, fire = 0.0, lightning = 0.0, dark = 0.0;
  for (int32_t giveId : giveIds) {
    if (giveId == 0) continue;
    int64_t rowId = static_cast<int64_t>(giveId) - kProtectorGiveIdPrefix;
    uintptr_t rowAddr = FindParamRow(hProcess, table, rowId);
    if (rowAddr == 0) continue;
    auto readPercent = [&](uintptr_t off) -> double {
      float raw = 1.0f;
      SIZE_T br = 0;
      if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(rowAddr + off), &raw,
                              sizeof(raw), &br))
        return 0.0;
      return 100.0 * (1.0 - static_cast<double>(raw));
    };
    phys = CombinePoise(phys, readPercent(kEquipParamProtectorPhysCutRateOffset));
    slash = CombinePoise(slash, readPercent(kEquipParamProtectorSlashCutRateOffset));
    strike = CombinePoise(strike, readPercent(kEquipParamProtectorStrikeCutRateOffset));
    thrust = CombinePoise(thrust, readPercent(kEquipParamProtectorThrustCutRateOffset));
    magic = CombinePoise(magic, readPercent(kEquipParamProtectorMagicCutRateOffset));
    fire = CombinePoise(fire, readPercent(kEquipParamProtectorFireCutRateOffset));
    lightning = CombinePoise(lightning, readPercent(kEquipParamProtectorThunderCutRateOffset));
    dark = CombinePoise(dark, readPercent(kEquipParamProtectorDarkCutRateOffset));
  }
  result.ok = true;
  result.phys = phys;
  result.slash = slash;
  result.strike = strike;
  result.thrust = thrust;
  result.magic = magic;
  result.fire = fire;
  result.lightning = lightning;
  result.dark = dark;
  return result;
}

// ---- armor status resistances (Bleed/Poison/Frost/Curse) --------------
//
// Unlike Poise/Absorption, these are plain i16 point values (not a
// "rate near 1.0" needing a transform) -- `resist_blood` (=Bleed),
// `resist_poison`, `resist_curse` sit right after `defense_thrust`;
// `resist_frost` sits much further into the row, near `upper_arm_id`.
// Also reads `resist_toxic` (a distinct, stronger poison-family stat
// DS3 tracks separately) since it's the same field cluster and free to
// grab, even though it wasn't explicitly asked for.
//
// Combined via plain ADDITION across armor pieces, not the diminishing-
// returns formula used for Poise/Absorption -- DS3's build-up
// resistances are well-documented as simple additive point totals, a
// different mechanic from the percentage-based Absorption stats.
//
// Live-verified (2026-08-19): computed Bleed=85 Poison=155 Frost=111
// Curse=147 confirmed correct against the user's own in-game
// Resistance screen -- the first successful use of straight addition
// (as opposed to CombinePoise) in this codebase. Toxic=155 could NOT
// be independently confirmed the same way -- the vanilla status screen
// doesn't display a Toxic figure at all, so it's read from the same
// field cluster (structurally sound, right next to the confirmed
// resist_poison) but not verified against a visible number the way
// the other four were.
constexpr uintptr_t kEquipParamProtectorResistPoisonOffset = 0xC0;  // i16
constexpr uintptr_t kEquipParamProtectorResistToxicOffset = 0xC2;   // i16
constexpr uintptr_t kEquipParamProtectorResistBloodOffset = 0xC4;   // i16, Bleed
constexpr uintptr_t kEquipParamProtectorResistCurseOffset = 0xC6;   // i16
constexpr uintptr_t kEquipParamProtectorResistFrostOffset = 0x12C;  // i16

struct ArmorResistanceResult {
  bool ok = false;
  double bleed = 0.0, poison = 0.0, toxic = 0.0, frost = 0.0, curse = 0.0;
};

// giveIds: the 4 armor slots' resolved giveId values, same as
// ComputeArmorPoise/ComputeArmorDefense.
ArmorResistanceResult ComputeArmorResistance(HANDLE hProcess, uintptr_t moduleBase,
                                              const std::vector<int32_t>& giveIds) {
  ArmorResistanceResult result;
  auto table = ResolveParamTable(hProcess, moduleBase, L"EquipParamProtector");
  if (!table.ok) return result;

  double bleed = 0.0, poison = 0.0, toxic = 0.0, frost = 0.0, curse = 0.0;
  for (int32_t giveId : giveIds) {
    if (giveId == 0) continue;
    int64_t rowId = static_cast<int64_t>(giveId) - kProtectorGiveIdPrefix;
    uintptr_t rowAddr = FindParamRow(hProcess, table, rowId);
    if (rowAddr == 0) continue;
    auto readI16 = [&](uintptr_t off) -> double {
      int16_t raw = 0;
      SIZE_T br = 0;
      if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(rowAddr + off), &raw,
                              sizeof(raw), &br))
        return 0.0;
      return static_cast<double>(raw);
    };
    bleed += readI16(kEquipParamProtectorResistBloodOffset);
    poison += readI16(kEquipParamProtectorResistPoisonOffset);
    toxic += readI16(kEquipParamProtectorResistToxicOffset);
    frost += readI16(kEquipParamProtectorResistFrostOffset);
    curse += readI16(kEquipParamProtectorResistCurseOffset);
  }
  result.ok = true;
  result.bleed = bleed;
  result.poison = poison;
  result.toxic = toxic;
  result.frost = frost;
  result.curse = curse;
  return result;
}

// ---- ring poise bonus (via SpEffectParam, not a flat EquipParamAccessory field) -
//
// Ring/weapon poise bonuses (e.g. Wolf Ring's "+30% Poise") don't get
// their own column in EquipParamAccessory the way armor's `poise`
// does -- they work through DS3's general status-effect system,
// SpEffectParam, referenced indirectly from the ring's own row.
// EquipParamAccessory has 5 SpEffect reference slots per row (`ref_id0`
// at +0x00, `ref_id1..4` at +0x48/0x4C/0x50/0x54 -- offsets computed
// from practice-tool's struct layout, same technique used for
// EquipParamProtector's `poise` field above), each either <=0 (unused)
// or a SpEffectParam row id. SpEffectParam itself has a `poise_rate`
// field at +0x1B8 (computed the same way -- struct field order times
// natural C alignment, not live-verified since no poise ring was on
// hand to test).
//
// The offset is live-corroborated (2026-08-19), even without a poise
// ring on hand: the user's 4 actually-equipped rings that session --
// Great Swamp Ring, Covetous Silver Serpent Ring, Sage Ring, Saint's
// Ring, none poise-related -- each independently read exactly
// `poise_rate=1.0000` for their one active SpEffect. An initial
// ADDITIVE-bonus guess (default 0.0, `final = base*(1+sum(rate))`) was
// tried first and produced a nonsensical +400% live (4 unrelated rings
// each contributing a spurious "+100%"), which is what exposed the
// clean, identical 1.0 across all 4 independent SpEffect rows -- too
// consistent to be four coincidental garbage reads, and exactly the
// value a MULTIPLICATIVE rate defaulting to "no change" would produce.
// Current formula: `final = armorTotal * product(poise_rate)` across
// every active ring SpEffect (a genuine poise ring would presumably
// carry e.g. `poise_rate=1.30` for a documented "+30% Poise", multiplying
// in rather than adding). Still UNVERIFIED against an actual poise ring
// -- no Wolf Ring or similar on hand this session -- but the default-
// value evidence above is a real live signal, not a guess.
constexpr int32_t kAccessoryGiveIdPrefix = static_cast<int32_t>(0x20000000);  // ItemParamIdPrefix::Accessory
constexpr uintptr_t kEquipParamAccessoryRefIdOffsets[] = {0x00, 0x48, 0x4C, 0x50, 0x54};
constexpr uintptr_t kSpEffectParamPoiseRateOffset = 0x1B8;
// equip_weight_change_rate -- the field 2 slots before poise_rate in
// declared order (soul_rate, then this, then all_item_weight_change_
// rate); offset computed with the identical field-by-field/alignment
// counting method as poise_rate above, which that offset's own live
// corroboration (see below) validates. This is what Havel's
// Ring/Ring of Favor's max-equip-load bonus runs through -- same
// mechanism as poise, just a different SpEffectParam column, and
// (unlike poise) each ring reinforcement level is already a distinct
// EquipParamAccessory row with its own giveId, so no separate "+1/+2/
// +3" handling is needed -- the normal giveId -> row -> ref_id chain
// picks up the exact tier automatically.
constexpr uintptr_t kSpEffectParamEquipWeightChangeRateOffset = 0xE4;

// Shared by ComputeRingPoiseRateMultiplier and
// ComputeRingEquipLoadRateMultiplier -- resolves EquipParamAccessory +
// SpEffectParam once, walks each ring's up-to-5 SpEffect ref slots, and
// multiplies together whatever f32 sits at `spEffectFieldOffset` in
// each active SpEffect's row (default 1.0 = no change, see the
// poise_rate derivation above for why that's the right convention).
// ringGiveIds: the 4 ring slots' resolved giveId values (0 or an
// Accessory giveId; empty/absent slots are skipped).
double ComputeRingSpEffectRateMultiplier(HANDLE hProcess, uintptr_t moduleBase,
                                          const std::vector<int32_t>& ringGiveIds,
                                          uintptr_t spEffectFieldOffset) {
  double multiplier = 1.0;
  bool anyRing = false;
  for (int32_t giveId : ringGiveIds) {
    if (giveId != 0) anyRing = true;
  }
  if (!anyRing) return multiplier;

  auto accessoryTable = ResolveParamTable(hProcess, moduleBase, L"EquipParamAccessory");
  if (!accessoryTable.ok) return multiplier;
  auto spEffectTable = ResolveParamTable(hProcess, moduleBase, L"SpEffectParam");
  if (!spEffectTable.ok) return multiplier;

  for (int32_t giveId : ringGiveIds) {
    if (giveId == 0) continue;
    int64_t rowId = static_cast<int64_t>(giveId) - kAccessoryGiveIdPrefix;
    uintptr_t rowAddr = FindParamRow(hProcess, accessoryTable, rowId);
    if (rowAddr == 0) continue;
    for (uintptr_t refOffset : kEquipParamAccessoryRefIdOffsets) {
      int32_t spEffectId = 0;
      SIZE_T br = 0;
      if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(rowAddr + refOffset),
                              &spEffectId, sizeof(spEffectId), &br))
        continue;
      if (spEffectId <= 0) continue;
      uintptr_t effectRow = FindParamRow(hProcess, spEffectTable, spEffectId);
      if (effectRow == 0) continue;
      float rate = 1.0f;
      if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(effectRow + spEffectFieldOffset),
                              &rate, sizeof(rate), &br))
        continue;
      multiplier *= static_cast<double>(rate);
    }
  }
  return multiplier;
}

double ComputeRingPoiseRateMultiplier(HANDLE hProcess, uintptr_t moduleBase,
                                       const std::vector<int32_t>& ringGiveIds) {
  return ComputeRingSpEffectRateMultiplier(hProcess, moduleBase, ringGiveIds,
                                            kSpEffectParamPoiseRateOffset);
}

// UNVERIFIED against a live Havel's Ring/Ring of Favor this session
// (2026-08-19, none on hand) -- same multiplicative-default-1.0
// convention as poise, same caveats. See "Ring equip-load bonus" in
// docs/TECHNICAL.md.
double ComputeRingEquipLoadRateMultiplier(HANDLE hProcess, uintptr_t moduleBase,
                                           const std::vector<int32_t>& ringGiveIds) {
  return ComputeRingSpEffectRateMultiplier(hProcess, moduleBase, ringGiveIds,
                                            kSpEffectParamEquipWeightChangeRateOffset);
}

// ---- ring attunement-slot bonus (via SpEffectParam) --------------------
//
// Same accessory/SpEffect resolution chain as the ring poise/equip-load
// bonuses above, but a genuinely different field TYPE (an additive u8
// slot count, not a multiplicative f32 rate) and a genuinely different
// combine rule (SUM, not product) -- so it's its own function rather
// than another ComputeRingSpEffectRateMultiplier call.
//
// Found live (2026-08-19) after an initial WRONG guess: assumed Sage
// Ring granted DS3's known +1 attunement-slot ring bonus (a real
// mechanic, misattributed to the wrong ring from memory). The user
// live-corrected this -- unequipping a *different* ring (Saint's Ring)
// dropped their slot count from 3 to 2, proving Sage Ring wasn't the
// source. Re-derived properly from there: dumped both rings' resolved
// SpEffectParam rows side by side and diffed them byte-for-byte --
// identical everywhere except exactly one byte, at +0x142 (SpEffectParam's
// `change_magic_slot` field, computed the same way poise_rate's offset
// was): Saint's Ring reads `1`, Sage Ring reads `0`. Matches the
// live-observed behavior exactly, and DS3 shares one attunement pool
// across all magic schools, so a single "magic slot" field granting
// the bonus (rather than separate sorcery/miracle/pyromancy fields)
// fits the game's own design.
constexpr uintptr_t kSpEffectParamChangeMagicSlotOffset = 0x142;  // u8

// ringGiveIds: the 4 ring slots' resolved giveId values. Returns the
// summed attunement-slot bonus (0 if no ring grants one).
int32_t ComputeRingAttunementSlotBonus(HANDLE hProcess, uintptr_t moduleBase,
                                        const std::vector<int32_t>& ringGiveIds) {
  int32_t total = 0;
  bool anyRing = false;
  for (int32_t giveId : ringGiveIds) {
    if (giveId != 0) anyRing = true;
  }
  if (!anyRing) return total;

  auto accessoryTable = ResolveParamTable(hProcess, moduleBase, L"EquipParamAccessory");
  if (!accessoryTable.ok) return total;
  auto spEffectTable = ResolveParamTable(hProcess, moduleBase, L"SpEffectParam");
  if (!spEffectTable.ok) return total;

  for (int32_t giveId : ringGiveIds) {
    if (giveId == 0) continue;
    int64_t rowId = static_cast<int64_t>(giveId) - kAccessoryGiveIdPrefix;
    uintptr_t rowAddr = FindParamRow(hProcess, accessoryTable, rowId);
    if (rowAddr == 0) continue;
    for (uintptr_t refOffset : kEquipParamAccessoryRefIdOffsets) {
      int32_t spEffectId = 0;
      SIZE_T br = 0;
      if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(rowAddr + refOffset),
                              &spEffectId, sizeof(spEffectId), &br))
        continue;
      if (spEffectId <= 0) continue;
      uintptr_t effectRow = FindParamRow(hProcess, spEffectTable, spEffectId);
      if (effectRow == 0) continue;
      uint8_t bonus = 0;
      if (!ReadProcessMemory(hProcess,
                              reinterpret_cast<LPCVOID>(effectRow +
                                                         kSpEffectParamChangeMagicSlotOffset),
                              &bonus, sizeof(bonus), &br))
        continue;
      total += static_cast<int32_t>(bonus);
    }
  }
  return total;
}

// Sanity-check fallback only, not the primary path anymore: flags a
// mismatch if Havel's Ring/Ring of Favor is equipped (by resolved item
// name) but ComputeRingEquipLoadRateMultiplier still came back at 1.0
// (no bonus applied) -- would mean the SpEffect chain above failed
// silently for that ring, not that no bonus exists. Empty string when
// nothing looks wrong.
std::string EquipLoadRingBonusWarning(const std::vector<std::string>& equippedItemNames) {
  bool haveHavels = false, haveFavor = false;
  for (const auto& name : equippedItemNames) {
    if (name.find("Havel's Ring") != std::string::npos) haveHavels = true;
    if (name.find("Ring of Favor") != std::string::npos) haveFavor = true;
  }
  if (!haveHavels && !haveFavor) return "";
  return std::string("expected a multiplier from ") +
         (haveHavels && haveFavor ? "Havel's Ring + Ring of Favor"
                                   : (haveHavels ? "Havel's Ring" : "Ring of Favor")) +
         " but the computed multiplier came back at 1.0 -- SpEffect resolution likely failed, "
         "actual max load may be higher";
}

// ---- weapon physical Attack Rating (COMPUTED live, PHYSICAL damage only) -
//
// The number shown in-game as a weapon's "Attack Power" -- Physical for
// a normal melee weapon -- reconstructed from three live param tables.
//
// **Live-verified exact match against the in-game equipment screen**
// (2026-08-19), the same day this was built: computed one-handed AR
// for Reinforced Club (120) and Fists (44) didn't match the user's
// reported 128/55 -- the gap turned out to be two-handing's `floor(Str
// * 1.5)` effective-Strength bonus (both weapons were being viewed
// two-handed). Recomputing with Str=floor(12*1.5)=18 gave EXACTLY 128
// and EXACTLY 55 -- two independent exact matches on the same tick,
// confirming every piece of the formula below (base attack,
// reinforcement scaling, correction percentages, the growth curve) is
// correct, not just plausible. `stats` now prints both the one-handed
// and two-handed figure for every weapon slot, since the live
// two-handing STATE isn't read (no pointer chain hunted for it yet --
// showing both sidesteps needing one).
//
// 1. EquipParamWeapon: `atk_base_physics` (i16 @0xC4) is the
//    UNREINFORCED base value -- confirmed live: 110 raw vs. a
//    community reference calculator's own +10 figure of 220, an exact
//    2.0 ratio (see step 2). `correct_strength`/`correct_agility`
//    (f32 @0x20/0x24) are the weapon's own scaling coefficients
//    (0-100ish) -- confirmed live: 66.0 raw vs. the same reference's
//    +10 figure of 92.4, an exact 1.4 ratio. Offsets computed from
//    practice-tool's struct field layout (same technique as every
//    other param struct in this file), and this ratio match is strong
//    live corroboration on top of that, not a blind guess. `correct_type`
//    (u8 @0xE8) is a CalcCorrectGraph row id -- see step 3.
//    IMPORTANT: unlike EquipParamProtector, DS3 does NOT bake each
//    reinforcement level into its own EquipParamWeapon row -- confirmed
//    live by trying to look up Pyromancy Flame's +4 row (giveId
//    13400004) directly and finding only ~2500 total rows in the whole
//    table (far too few for a per-level scheme). Reinforcement is a
//    second table instead:
// 2. ReinforceParamWeapon, row id = `reinforce_type_id`(i16 @0xD6) +
//    numeric level (extracted from giveId's low 2 digits, same field
//    the rest of this file already reads for weapon name lookups).
//    `physics_atk_rate`/`correct_strength_rate`/`correct_agility_rate`
//    (f32) are the level's multipliers. Confirmed live for
//    reinforce_type_id=0 (Reinforced Club, level 0): row 10 (level 10,
//    a reference calculator's own assumed max-upgrade dataset) reads
//    physics_atk_rate=2.0 and correct_strength_rate=correct_agility_rate
//    =1.4 -- an EXACT match to both ratios derived in step 1, on a
//    completely independent table. The indexing scheme's `+ level`
//    (not `* 100 + level`, tried and disproved first) was pinned down
//    live too: Fists' reinforce_type_id=3000 immediately broke the
//    *100 version (row 300000 doesn't exist, far past the table's 444
//    rows), while row 3000 itself exists and reads a flat 1.0 across
//    every rate -- Fists can't be reinforced with regular titanite, so
//    that's the correct "no bonus" row for an unreinforceable weapon.
// 3. CalcCorrectGraph, row id = the weapon's own `correct_type`. A
//    piecewise growth curve (5 stat breakpoints -> 5 growth-percent
//    values, with a per-segment curve exponent) converts a raw stat
//    value into a 0-100 "how much of this stat's investment applies"
//    percentage. Row 0 read live: breakpoints [1,18,40,60,99], growth
//    [0,25,75,85,100] -- matches the well-documented shape of DS3's
//    Strength/Dexterity soft-cap curve (40 then 60 then 99) closely
//    enough to trust the offsets, and importantly matches Reinforced
//    Club's OWN correct_type=0 exactly against a reference calculator's
//    independently-sourced "saturation_index" of 0 for this exact
//    weapon's physical scaling. The piecewise formula (including how a
//    negative curve exponent "flips" the curve) is sourced from a
//    modding wiki -- the two exact end-to-end matches above are strong
//    evidence it's right (a wrong growth-curve evaluation would have
//    thrown off both final numbers, not landed on them precisely), but
//    it hasn't been isolated and checked in unverified in-between
//    stat ranges on its own.
//
// Combined: `AR = baseAtk + floor(baseAtk * (corrStr/100*growthStr/100
// + corrDex/100*growthDex/100))` -- reference calculator's own formula
// shape (see calculator.js from Derling/ds3-attack-rating-calculator,
// an existing public DS3 AR tool), reimplemented against live memory
// instead of that tool's offline JSON snapshot.
//
// PHYSICAL ONLY. Magic/Fire/Lightning/Dark scaling needs
// AttackElementCorrectParam's 25-slot stat-to-damage-type mapping,
// which hasn't been decoded yet -- every currently equipped weapon is
// unenchanted, so this doesn't block testing against real live data,
// but it's a real gap for infused weapons. Catalysts (staves,
// talismans, Pyromancy Flames) have their own separate "Spell Buff"
// stat, not covered by this at all -- physical AR is computed for them
// too since the fields exist, but it isn't the number their equipment
// screen actually highlights.
constexpr uintptr_t kWeaponAtkBasePhysicsOffset = 0xC4;      // i16
constexpr uintptr_t kWeaponCorrectStrengthOffset = 0x20;     // f32
constexpr uintptr_t kWeaponCorrectAgilityOffset = 0x24;      // f32
constexpr uintptr_t kWeaponReinforceTypeIdOffset = 0xD6;     // i16
constexpr uintptr_t kWeaponCorrectTypeOffset = 0xE8;         // u8, CalcCorrectGraph row id
constexpr uintptr_t kReinforcePhysicsAtkRateOffset = 0x00;   // f32
constexpr uintptr_t kReinforceCorrectStrengthRateOffset = 0x1C;  // f32
constexpr uintptr_t kReinforceCorrectAgilityRateOffset = 0x20;   // f32

// Piecewise growth curve: 5 stat breakpoints (stageMaxVal), 5 output
// percentages at those breakpoints (stageMaxGrowVal), 5 per-segment
// curve exponents (adjPt). Field layout and formula from the
// Souls Modding wiki's CalcCorrectGraph page (see docs/TECHNICAL.md) --
// NOT yet independently live-verified bit-for-bit, only structurally
// plausible (breakpoints/growth values match DS3's well-documented
// Str/Dex soft-cap shape for row 0). A negative exponent "flips" the
// curve (per that same source); implemented as interpolating from the
// far end of the segment instead of the near end in that case.
struct CalcCorrectGraphRow {
  bool ok = false;
  float stageMaxVal[5] = {};
  float stageMaxGrowVal[5] = {};
  float adjPt[5] = {};
};

CalcCorrectGraphRow ReadCalcCorrectGraphRow(HANDLE hProcess, uintptr_t moduleBase,
                                             int64_t rowId) {
  CalcCorrectGraphRow result;
  auto table = ResolveParamTable(hProcess, moduleBase, L"CalcCorrectGraph");
  if (!table.ok) return result;
  uintptr_t rowAddr = FindParamRow(hProcess, table, rowId);
  if (rowAddr == 0) return result;
  SIZE_T br = 0;
  bool ok = true;
  ok &= !!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(rowAddr + 0x00),
                             result.stageMaxVal, sizeof(result.stageMaxVal), &br);
  ok &= !!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(rowAddr + 0x14),
                             result.stageMaxGrowVal, sizeof(result.stageMaxGrowVal), &br);
  ok &= !!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(rowAddr + 0x28), result.adjPt,
                             sizeof(result.adjPt), &br);
  result.ok = ok;
  return result;
}

double EvaluateCalcCorrectGraph(const CalcCorrectGraphRow& graph, double statValue) {
  if (!graph.ok) return 0.0;
  if (statValue <= graph.stageMaxVal[0]) return graph.stageMaxGrowVal[0];
  if (statValue >= graph.stageMaxVal[4]) return graph.stageMaxGrowVal[4];
  for (int seg = 0; seg < 4; ++seg) {
    double lo = graph.stageMaxVal[seg], hi = graph.stageMaxVal[seg + 1];
    if (statValue > hi) continue;
    double growLo = graph.stageMaxGrowVal[seg], growHi = graph.stageMaxGrowVal[seg + 1];
    double exp = graph.adjPt[seg];
    if (hi == lo) return growHi;
    double ratio = (statValue - lo) / (hi - lo);
    double shaped;
    if (exp >= 0.0) {
      shaped = std::pow(ratio, exp);
    } else {
      shaped = 1.0 - std::pow(1.0 - ratio, -exp);
    }
    return growLo + (growHi - growLo) * shaped;
  }
  return graph.stageMaxGrowVal[4];
}

// ---- elemental Attack Rating (all 5 damage types) --------------------
//
// Extends the physical-only formula above (see the big comment block)
// to Magic/Fire/Lightning/Dark. Live-verified EXACT MATCH against a
// Fire-infused Reinforced Club (2026-08-19): computed Physical=93,
// Fire=93 against the user's own in-game reading of 93/93.
//
// Getting there took one real live-caught mistake, not just
// confirmation of a guess:
//
// 1. `attack_element_correct_id` (i32 @0x228) stayed IDENTICAL (10000)
//    between the unenchanted and Fire-infused row of the same weapon --
//    so which stats CAN contribute to which damage type is a fixed,
//    weapon-archetype-wide association, not something that changes per
//    infusion. What changes per infusion instead is each `correct_X`
//    coefficient itself (0 = that stat contributes nothing). Given
//    that, the standard stat-to-damage-type grouping is hardcoded here
//    rather than decoded from AttackElementCorrectParam's raw 25-slot
//    layout (still undeciphered) -- it matches BOTH a public DS3 AR
//    calculator's own TYPEINDEX table and a community Elden Ring
//    formula writeup (same underlying engine convention):
//      Physical  <- Strength, Dexterity
//      Magic     <- Intelligence
//      Fire      <- Intelligence, Faith
//      Lightning <- Faith
//      Dark      <- Intelligence, Faith
// 2. `reinforce_type_id` DOES change per infusion (0 for Normal, 600
//    for this Fire row) -- confirming each infusion gets its own
//    ReinforceParamWeapon category, still resolved via the same
//    `reinforce_type_id + level` scheme validated for Physical.
//    correct_strength_rate/correct_agility_rate read a flat 0.0 at
//    BOTH level 0 and level 10 for this Fire row -- confirming Fire
//    infusion's lack of scaling is intentional and permanent across
//    the whole upgrade path, matching its well-documented "no stat
//    scaling, flat damage" behavior.
// 3. **The live-caught mistake**: `correct_luck` reads 14.0 on this
//    Fire row -- unchanged from the Normal row, NOT zeroed the way
//    correct_magic/correct_faith were. An initial version included it
//    unconditionally in Physical's bonus (reasoning: it's nonzero, so
//    it must apply), which gave Physical=94 against the confirmed live
//    reading of 93 -- a genuinely wrong number, not just an unverified
//    one. So `correct_luck` isn't an infusion-gated coefficient like
//    magic/faith; it looks like some inherent per-weapon-category
//    value that only actually applies under a separate condition this
//    project hasn't decoded (most likely Hollow infusion specifically,
//    via a different `attack_element_correct_id`). Luck is deliberately
//    NOT applied anywhere below -- omitted rather than guessed, since
//    getting caught being wrong once here is reason enough not to trust
//    the same kind of guess for Hollow without its own live test.
//
// Magic/Lightning/Dark share this exact same formula shape but haven't
// been individually live-tested (no weapon on hand infused for those) --
// the Fire test exercises the fire<-Int+Faith branch fully (both
// coefficients were 0 for this weapon, i.e. it only actually confirms
// the *no-scaling* path map cleanly onto a real result; a weapon with
// genuine nonzero magic/faith correction would be a stronger test of
// the additive two-stat formula specifically).
constexpr uintptr_t kWeaponAtkBaseMagicOffset = 0xC6;    // i16
constexpr uintptr_t kWeaponAtkBaseFireOffset = 0xC8;     // i16
constexpr uintptr_t kWeaponAtkBaseThunderOffset = 0xCA;  // i16
constexpr uintptr_t kWeaponAtkBaseDarkOffset = 0x188;    // i16
constexpr uintptr_t kWeaponCorrectMagicOffset = 0x28;    // f32
constexpr uintptr_t kWeaponCorrectFaithOffset = 0x2C;    // f32
constexpr uintptr_t kWeaponCorrectLuckOffset = 0x198;    // f32
constexpr uintptr_t kReinforceMagicAtkRateOffset = 0x04;    // f32
constexpr uintptr_t kReinforceFireAtkRateOffset = 0x08;     // f32
constexpr uintptr_t kReinforceThunderAtkRateOffset = 0x0C;  // f32
constexpr uintptr_t kReinforceCorrectMagicRateOffset = 0x24;  // f32
constexpr uintptr_t kReinforceCorrectFaithRateOffset = 0x28;  // f32
constexpr uintptr_t kReinforceDarkAtkRateOffset = 0x58;       // f32

// One damage type's AR, split the way the game's own menu shows it:
// "base + bonus" when requirements are met, "base - penalty" when not.
// Live-verified 2026-10-06 that the game floors base and bonus
// SEPARATELY (Heysel Pick magic: base 61*1.15=70.15, bonus 31.95 ->
// game shows 70+31=101, a single combined floor would give 102).
struct WeaponDamageTypeAr {
  bool present = false;  // false if this weapon deals no damage of this type
  int base = 0;          // floor(atk_base_X * reinforce rate)
  int bonus1H = 0;       // scaling bonus, or -penalty (negative) if a requirement is unmet
  int bonus2H = 0;       // same with two-handing's floor(Str*1.5)
  bool penalized1H = false;
  bool penalized2H = false;
  double oneHanded = 0.0;  // base + bonus1H
  double twoHanded = 0.0;  // base + bonus2H
};

// Stat requirements (u8 each). Located live 2026-10-06 by dumping rows
// with well-known requirements and finding them at the same offsets in
// every one: Greatsword 28/10/0/0, Greataxe 32/8, Black Knight Sword
// 20/18, Sorcerer's Staff 6/0/10/0, Saint's Talisman 4/0/0/16. Field
// order matches Paramdex's EQUIP_PARAM_WEAPON_ST (properStrength..
// properFaith right after wepmotionBothHandId).
constexpr uintptr_t kWeaponProperStrengthOffset = 0xEE;
constexpr uintptr_t kWeaponProperAgilityOffset = 0xEF;
constexpr uintptr_t kWeaponProperMagicOffset = 0xF0;
constexpr uintptr_t kWeaponProperFaithOffset = 0xF1;
// Unmet requirement for any stat feeding a damage type: that type loses
// all scaling and floor(40% of base) is subtracted. Live-verified
// 2026-10-06 against three red in-game figures, Dex 9 below each
// weapon's Dex requirement: Long Sword 110-44, Golden Ritual Spear
// 73-29, Heysel Pick 93-37 (93.15*0.4=37.26 -> 37; "keep 60%" would
// have given 55, not the game's 56).
constexpr double kUnmetRequirementPenaltyRate = 0.4;

// ---- AttackElementCorrectParam (which stat feeds which damage type) --
//
// Layout from soulsmods/Paramdex DS3/Defs/ATTACK_ELEMENT_CORRECT_PARAM_ST
// .xml: 32 single-bit flags (u32 @0x00), then 25 s16 "addRate" (default
// -1) @0x04, then 25 s16 "corrRate" (default 100) @0x36. 25 = 5 stats x
// 5 damage types. Paramdex leaves the fields unnamed for DS3; Elden
// Ring's same-named param calls the three groups isXCorrect_byY /
// overwriteXCorrectRate_byY (-1 = keep the weapon's own correct_X) /
// InfluenceXCorrectRate_byY (percent multiplier), grouped by damage type
// with stats in Str/Dex/Int/Fth/Luck order: index = type*5 + stat.
//
// Confirmed live 2026-10-06, three ways:
//  - the default row (10000, used by ~all weapons) reads flags
//    0x0C43083 = Phys<-S,D Magic<-I Fire<-I,F Lightning<-F Dark<-I,F,
//    bit-for-bit the grouping previously hardcoded here (and already
//    live-verified for Physical and Fire);
//  - unique rows decode to each weapon's known quirk: Anri's Straight
//    Sword Phys<-S,D,F,L, Saint Bident Phys<-S,D,F, Golden Ritual Spear
//    Magic<-F (not Int);
//  - Golden Ritual Spear's in-game Magic 77+39 has a nonzero bonus at
//    Int-irrelevant/Faith 40 -- the hardcoded Magic<-Int grouping
//    predicted +0, this decode predicts exactly +39.
// It also solved the old Luck mystery: Hollow infusion rows point at
// their own row (10015, Phys<-S,D,L) and Blessed at 10014 (Phys<-S,D,F),
// while Normal/Fire/etc. share 10000 with Luck off -- which is why the
// Fire Reinforced Club's nonzero correct_luck (14) never applied.
// ReinforceParamWeapon has no Luck rate at all (Paramdex), so Luck's
// coefficient is used unscaled by upgrade level -- the one piece of
// this still unverified against an in-game Hollow weapon.
constexpr uintptr_t kWeaponAttackElementCorrectIdOffset = 0x228;  // i32
constexpr uintptr_t kAecFlagsOffset = 0x00;          // u32, 25 used bits
constexpr uintptr_t kAecOverwriteRateOffset = 0x04;  // s16[25]
constexpr uintptr_t kAecInfluenceRateOffset = 0x36;  // s16[25]
constexpr uint32_t kAecDefaultFlags = 0x0C43083;     // row 10000, fallback if a row is missing
enum AecStat { kAecStr = 0, kAecDex, kAecInt, kAecFth, kAecLuck, kAecStatCount };
enum AecType { kAecPhys = 0, kAecMagic, kAecFire, kAecThunder, kAecDark, kAecTypeCount };

struct AttackElementCorrect {
  bool ok = false;
  uint32_t flags = 0;
  int16_t overwrite[25] = {};
  int16_t influence[25] = {};
  bool Has(int type, int stat) const { return (flags >> (type * 5 + stat)) & 1u; }
};

AttackElementCorrect ReadAttackElementCorrect(HANDLE hProcess, uintptr_t moduleBase,
                                              int32_t rowId) {
  AttackElementCorrect result;
  auto table = ResolveParamTable(hProcess, moduleBase, L"AttackElementCorrectParam");
  if (!table.ok) return result;
  uintptr_t rowAddr = FindParamRow(hProcess, table, rowId);
  if (rowAddr == 0) return result;
  SIZE_T br = 0;
  bool ok = true;
  ok &= !!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(rowAddr + kAecFlagsOffset),
                             &result.flags, sizeof(result.flags), &br);
  ok &= !!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(rowAddr + kAecOverwriteRateOffset),
                             result.overwrite, sizeof(result.overwrite), &br);
  ok &= !!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(rowAddr + kAecInfluenceRateOffset),
                             result.influence, sizeof(result.influence), &br);
  result.ok = ok;
  return result;
}

// "Phys<-S,D Fire<-I,F ..."
std::string DescribeAttackElementCorrect(const AttackElementCorrect& aec) {
  static const char* kTypeNames[] = {"Phys", "Magic", "Fire", "Lightning", "Dark"};
  static const char* kStatNames[] = {"S", "D", "I", "F", "L"};
  std::string out;
  for (int t = 0; t < kAecTypeCount; ++t) {
    std::string stats;
    for (int s = 0; s < kAecStatCount; ++s) {
      if (!aec.Has(t, s)) continue;
      if (!stats.empty()) stats += ",";
      stats += kStatNames[s];
      int idx = t * 5 + s;
      if (aec.overwrite[idx] != -1) stats += "[ovr" + std::to_string(aec.overwrite[idx]) + "]";
      if (aec.influence[idx] != 100) stats += "[inf" + std::to_string(aec.influence[idx]) + "]";
    }
    if (stats.empty()) continue;
    if (!out.empty()) out += " ";
    out += std::string(kTypeNames[t]) + "<-" + stats;
  }
  return out.empty() ? "(no flags)" : out;
}

// ---- catalysts: Spell Buff + which growth curve they scale on --------
//
// Catalyst type comes from EquipParamWeapon's category bitflags --
// bitfield0's enableMagic bit (sorcery), bitfield1 bits 0-2
// (pyromancy/miracle/vow). Confirmed live: Pyromancy Flame reads
// bitfield1=0x11, bit 0 = enablePyromancy.
//
// Live-derived 2026-10-06 (no community source found): Int/Faith-driven
// scaling -- elemental AR and Spell Buff -- does NOT use the weapon's
// correctType curve. It uses a second curve id stored in the u8 at
// 0x18A (Paramdex: unnamed "Unk26", right after atkBaseDark). Found by
// solving each catalyst's in-game Spell Buff for the curve value it
// needs at stat 40, matching that against every CalcCorrectGraph row
// evaluated at 40, then searching six catalyst rows for a byte equal to
// the implied row id in every one -- 0x18A was the only match:
//   Sorcerer's/Witchtree/Sunlight/Bellvine/Izalith: 16 (g(40)=52.96)
//   Heretic's Staff: 5 (g(40)=60.00)   Saint's Talisman: 6 (43.75)
//   Pyromancy Flame: 0 (75.00, same as its correctType)
// The same curve drives melee elemental damage on hybrids (Heysel Pick
// Magic 70+31, Golden Ritual Spear Magic 77+39 -- both exact). Plain
// weapons read 0 here, same as their correctType, so for them this
// changes nothing (Fire infusion's Fire AR stays verified).
// Spell Buff = 100 + floor(100 * f), where f is the scaling fraction of
// the catalyst's spell element -- sorcery: Magic, pyromancy: Fire,
// miracle: Lightning -- computed exactly as for elemental AR (same
// AttackElementCorrectParam stat flags, same 0x18A curve), just on a
// fixed base of 100 instead of the weapon's atk_base. The element
// mapping is what makes Izalith Staff work: it carries a Faith
// coefficient (51) but its default row feeds Magic from Int only, so
// curve 15 x Int 84% = 171, exact; summing Faith too would give 214.
// Golden Ritual Spear's row feeds Magic from Faith instead, hence its
// Faith-driven 150. Live-verified exact 2026-10-06 against all ten
// catalysts in the user's inventory (Sorcerer's 152, Heretic's 157,
// Witchtree 135, Izalith 171, Saint's Talisman 149, Sunlight 143,
// Saint-tree Bellvine 141, Golden Ritual Spear 150, Heysel Pick 145,
// Pyromancy Flame +6 201). Vow catalysts (bitfield1 bit 2) aren't
// covered -- none on hand, element unknown. This corrects an
// earlier claim that Pyromancy Flame's Spell Buff equaled its Fire AR
// (230 at +4): that figure was the Fire AR, and the two differ (298 vs
// 201 at +6).
constexpr uintptr_t kWeaponBitfield0Offset = 0x101;  // u8
constexpr uintptr_t kWeaponBitfield1Offset = 0x102;  // u8
constexpr uint8_t kWeaponBitfield0EnableMagic = 0x80;    // bit 7: sorcery
constexpr uint8_t kWeaponBitfield1EnablePyromancy = 0x01;
constexpr uint8_t kWeaponBitfield1EnableMiracle = 0x02;
constexpr uint8_t kWeaponBitfield1SpellMask = 0x07;  // pyromancy/miracle/vow
constexpr uintptr_t kWeaponElementCorrectTypeOffset = 0x18A;  // u8, CalcCorrectGraph row id
constexpr double kSpellBuffBase = 100.0;

struct WeaponArResult {
  bool ok = false;
  bool isCatalyst = false;
  const char* catalystKind = "";  // "sorcery" / "miracle" / "pyromancy" / "vow"
  WeaponDamageTypeAr types[kAecTypeCount];  // indexed by AecType
  int spellBuff = 0;                        // catalysts only
  int32_t attackElementCorrectId = 0;
  AttackElementCorrect aec;
  float correctLuck = 0.0f;
  bool usesLuck = false;  // Luck feeds some damage type (unverified magnitude, see above)
};

bool g_arVerbose = false;  // `ar --verbose`: dump every formula input

// giveId: the weapon's resolved giveId. str/dex/intel/faith/luck: the
// player's current live attribute values.
WeaponArResult ComputeWeaponAr(HANDLE hProcess, uintptr_t moduleBase, int32_t giveId,
                                int32_t str, int32_t dex, int32_t intel, int32_t faith,
                                int32_t luck) {
  WeaponArResult result;
  int32_t level = giveId % 100;
  int32_t baseRowId = giveId - level;

  auto weaponTable = ResolveParamTable(hProcess, moduleBase, L"EquipParamWeapon");
  if (!weaponTable.ok) return result;
  uintptr_t rowAddr = FindParamRow(hProcess, weaponTable, baseRowId);
  if (rowAddr == 0) return result;

  SIZE_T br = 0;
  int16_t atk[kAecTypeCount] = {};
  float corr[kAecStatCount] = {};
  int16_t reinforceTypeId = 0;
  uint8_t correctType = 0, elementCorrectType = 0;
  uint8_t bitfield0 = 0, bitfield1 = 0;
  uint8_t proper[4] = {};
  int32_t aecId = 0;
  bool ok = true;
  auto readAt = [&](uintptr_t off, auto& dst) {
    ok &= !!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(rowAddr + off), &dst,
                               sizeof(dst), &br);
  };
  readAt(kWeaponAtkBasePhysicsOffset, atk[kAecPhys]);
  readAt(kWeaponAtkBaseMagicOffset, atk[kAecMagic]);
  readAt(kWeaponAtkBaseFireOffset, atk[kAecFire]);
  readAt(kWeaponAtkBaseThunderOffset, atk[kAecThunder]);
  readAt(kWeaponAtkBaseDarkOffset, atk[kAecDark]);
  readAt(kWeaponCorrectStrengthOffset, corr[kAecStr]);
  readAt(kWeaponCorrectAgilityOffset, corr[kAecDex]);
  readAt(kWeaponCorrectMagicOffset, corr[kAecInt]);
  readAt(kWeaponCorrectFaithOffset, corr[kAecFth]);
  readAt(kWeaponCorrectLuckOffset, corr[kAecLuck]);
  readAt(kWeaponReinforceTypeIdOffset, reinforceTypeId);
  readAt(kWeaponCorrectTypeOffset, correctType);
  readAt(kWeaponElementCorrectTypeOffset, elementCorrectType);
  readAt(kWeaponBitfield0Offset, bitfield0);
  readAt(kWeaponBitfield1Offset, bitfield1);
  readAt(kWeaponProperStrengthOffset, proper[kAecStr]);
  readAt(kWeaponProperAgilityOffset, proper[kAecDex]);
  readAt(kWeaponProperMagicOffset, proper[kAecInt]);
  readAt(kWeaponProperFaithOffset, proper[kAecFth]);
  readAt(kWeaponAttackElementCorrectIdOffset, aecId);
  if (!ok) return result;

  bool isSorcery = (bitfield0 & kWeaponBitfield0EnableMagic) != 0;
  bool isMiracle = (bitfield1 & kWeaponBitfield1EnableMiracle) != 0;
  bool isPyromancy = (bitfield1 & kWeaponBitfield1EnablePyromancy) != 0;
  result.isCatalyst = isSorcery || (bitfield1 & kWeaponBitfield1SpellMask) != 0;
  result.catalystKind = isSorcery     ? "sorcery"
                        : isMiracle   ? "miracle"
                        : isPyromancy ? "pyromancy"
                        : result.isCatalyst ? "vow"
                                            : "";

  auto reinforceTable = ResolveParamTable(hProcess, moduleBase, L"ReinforceParamWeapon");
  if (!reinforceTable.ok) return result;
  int64_t reinforceRowId = static_cast<int64_t>(reinforceTypeId) + level;
  uintptr_t reinforceRowAddr = FindParamRow(hProcess, reinforceTable, reinforceRowId);
  if (reinforceRowAddr == 0) return result;

  float rAtk[kAecTypeCount] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  float rCorr[kAecStatCount] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};  // Luck has no rate field
  auto readReinforceAt = [&](uintptr_t off, float& dst) {
    ok &= !!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(reinforceRowAddr + off), &dst,
                               sizeof(dst), &br);
  };
  readReinforceAt(kReinforcePhysicsAtkRateOffset, rAtk[kAecPhys]);
  readReinforceAt(kReinforceMagicAtkRateOffset, rAtk[kAecMagic]);
  readReinforceAt(kReinforceFireAtkRateOffset, rAtk[kAecFire]);
  readReinforceAt(kReinforceThunderAtkRateOffset, rAtk[kAecThunder]);
  readReinforceAt(kReinforceDarkAtkRateOffset, rAtk[kAecDark]);
  readReinforceAt(kReinforceCorrectStrengthRateOffset, rCorr[kAecStr]);
  readReinforceAt(kReinforceCorrectAgilityRateOffset, rCorr[kAecDex]);
  readReinforceAt(kReinforceCorrectMagicRateOffset, rCorr[kAecInt]);
  readReinforceAt(kReinforceCorrectFaithRateOffset, rCorr[kAecFth]);
  if (!ok) return result;

  result.attackElementCorrectId = aecId;
  result.correctLuck = corr[kAecLuck];
  result.aec = ReadAttackElementCorrect(hProcess, moduleBase, aecId);
  if (!result.aec.ok) result.aec.flags = kAecDefaultFlags;
  auto overwriteAt = [&](int idx) { return result.aec.ok ? result.aec.overwrite[idx] : -1; };
  auto influenceAt = [&](int idx) { return result.aec.ok ? result.aec.influence[idx] : 100; };

  // Physical scales on correctType; elemental damage and Spell Buff on
  // the 0x18A curve (see the catalyst comment above).
  auto physGraph = ReadCalcCorrectGraphRow(hProcess, moduleBase, correctType);
  int64_t elemGraphRow = elementCorrectType;
  auto elemGraph = elemGraphRow == correctType
                       ? physGraph
                       : ReadCalcCorrectGraphRow(hProcess, moduleBase, elemGraphRow);

  const double stats1H[kAecStatCount] = {static_cast<double>(str), static_cast<double>(dex),
                                         static_cast<double>(intel), static_cast<double>(faith),
                                         static_cast<double>(luck)};
  const double str2H = std::floor(str * 1.5);
  const bool unmet1H[kAecStatCount] = {str < proper[kAecStr], dex < proper[kAecDex],
                                       intel < proper[kAecInt], faith < proper[kAecFth], false};
  const bool unmet2H[kAecStatCount] = {str2H < proper[kAecStr], unmet1H[kAecDex],
                                       unmet1H[kAecInt], unmet1H[kAecFth], false};
  auto growth = [&](int type, double statValue) {
    const auto& g = (type != kAecPhys) ? elemGraph : physGraph;
    return EvaluateCalcCorrectGraph(g, statValue);
  };

  if (g_arVerbose) {
    Log("    [verbose] giveId=%d atk P/M/F/L/D=%d/%d/%d/%d/%d reinforceRow=%lld rates "
        "P/M/F/L/D=%.3f/%.3f/%.3f/%.3f/%.3f",
        giveId, atk[0], atk[1], atk[2], atk[3], atk[4], static_cast<long long>(reinforceRowId),
        rAtk[0], rAtk[1], rAtk[2], rAtk[3], rAtk[4]);
    Log("    [verbose] correct S/D/I/F/L=%.1f/%.1f/%.1f/%.1f/%.1f reinforceCorr S/D/I/F="
        "%.3f/%.3f/%.3f/%.3f proper S/D/I/F=%u/%u/%u/%u correctType=%u elemCurve=%lld "
        "bitfield0=0x%02X bitfield1=0x%02X",
        corr[0], corr[1], corr[2], corr[3], corr[4], rCorr[0], rCorr[1], rCorr[2], rCorr[3],
        proper[0], proper[1], proper[2], proper[3], correctType,
        static_cast<long long>(elemGraphRow), bitfield0, bitfield1);
  }

  double scalingFrac[kAecTypeCount] = {};  // one-handed, unpenalized -- feeds Spell Buff
  for (int t = 0; t < kAecTypeCount; ++t) {
    double frac1H = 0.0, frac2H = 0.0;
    bool pen1H = false, pen2H = false, luckFeedsType = false;
    for (int s = 0; s < kAecStatCount; ++s) {
      if (!result.aec.Has(t, s)) continue;
      pen1H |= unmet1H[s];
      pen2H |= unmet2H[s];
      int idx = t * 5 + s;
      double coef = overwriteAt(idx) != -1 ? overwriteAt(idx) : corr[s];
      if (s == kAecLuck && coef != 0.0) luckFeedsType = true;
      double scale = coef / 100.0 * rCorr[s] * influenceAt(idx) / 100.0;
      frac1H += scale * growth(t, stats1H[s]) / 100.0;
      frac2H += scale * growth(t, s == kAecStr ? str2H : stats1H[s]) / 100.0;
    }
    scalingFrac[t] = frac1H;
    double base = static_cast<double>(atk[t]) * static_cast<double>(rAtk[t]);
    if (base <= 0.5) continue;
    result.usesLuck |= luckFeedsType;
    auto& out = result.types[t];
    out.present = true;
    out.base = static_cast<int>(std::floor(base));
    int penalty = -static_cast<int>(std::floor(base * kUnmetRequirementPenaltyRate));
    out.penalized1H = pen1H;
    out.penalized2H = pen2H;
    out.bonus1H = pen1H ? penalty : static_cast<int>(std::floor(base * frac1H));
    out.bonus2H = pen2H ? penalty : static_cast<int>(std::floor(base * frac2H));
    out.oneHanded = out.base + out.bonus1H;
    out.twoHanded = out.base + out.bonus2H;
  }

  if (isSorcery || isMiracle || isPyromancy) {
    int element = isSorcery ? kAecMagic : isPyromancy ? kAecFire : kAecThunder;
    result.spellBuff =
        static_cast<int>(kSpellBuffBase + std::floor(kSpellBuffBase * scalingFrac[element]));
  }

  result.ok = true;
  return result;
}

// " Phys=110+17 Magic=70+31 SpellBuff=152" -- the game menu's own
// base+bonus / base-penalty split. Marks Luck-scaled weapons, whose Luck
// term is the one part not yet checked against an in-game figure.
std::string FormatWeaponAr(const WeaponArResult& ar, bool twoHanded) {
  static const char* kLabels[] = {"Phys", "Magic", "Fire", "Lightning", "Dark"};
  std::string out;
  for (int t = 0; t < kAecTypeCount; ++t) {
    const auto& type = ar.types[t];
    if (!type.present) continue;
    int bonus = twoHanded ? type.bonus2H : type.bonus1H;
    char buf[64];
    std::snprintf(buf, sizeof(buf), " %s=%d%+d", kLabels[t], type.base, bonus);
    out += buf;
  }
  if (ar.isCatalyst) out += " SpellBuff=" + std::to_string(ar.spellBuff);
  if (ar.usesLuck) out += " [Luck-scaled: Luck term unverified]";
  return out.empty() ? std::string(" (no damage)") : out;
}

// Re-derives WorldChrMan/XA from the live module's own code bytes (never
// cached, never hardcoded as an absolute address) and walks
// +0x80 -> +xa -> +0x18 -> +kHpFieldOffset to the live HP address.
// Read-only throughout: VirtualQueryEx + ReadProcessMemory only.
ResolvedPointers ResolvePlayerHp(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize) {
  ResolvedPointers result;
  SIZE_T br = 0;

  auto wcmPattern = ParsePattern(kWorldChrManPattern);
  uintptr_t wcmInstr = FindPatternInModule(hProcess, moduleBase, moduleSize, wcmPattern);
  if (wcmInstr == 0) return result;

  int32_t disp = 0;
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(wcmInstr + 3), &disp, sizeof(disp), &br);
  uintptr_t wcmCell = static_cast<uintptr_t>(static_cast<intptr_t>(wcmInstr) + 7 + disp);
  result.worldChrManCellOffset = wcmCell - moduleBase;

  uintptr_t wcmValue = 0;
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(wcmCell), &wcmValue, sizeof(wcmValue),
                          &br) ||
      wcmValue == 0)
    return result;

  auto xaPattern = ParsePattern(kXaPattern);
  uintptr_t xaInstr = FindPatternInModule(hProcess, moduleBase, moduleSize, xaPattern);
  if (xaInstr == 0) return result;
  int32_t xaRaw = 0;
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(xaInstr + 3), &xaRaw, sizeof(xaRaw), &br);
  result.xaConstant = static_cast<uintptr_t>(static_cast<intptr_t>(xaRaw));

  uintptr_t s1 = 0, s2 = 0, s3 = 0;
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(wcmValue + 0x80), &s1, sizeof(s1),
                          &br) ||
      s1 == 0)
    return result;
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(s1 + result.xaConstant), &s2,
                          sizeof(s2), &br) ||
      s2 == 0)
    return result;
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(s2 + 0x18), &s3, sizeof(s3), &br) ||
      s3 == 0)
    return result;

  result.entityAddress = s1;
  result.structBase = s3;
  result.chrModulesAddress = s2;
  result.hpAddress = s3 + kHpFieldOffset;
  result.fpAddress = s3 + kFpFieldOffset;
  result.staminaAddress = s3 + kStaminaFieldOffset;
  result.maxHpAddress = s3 + kHpMaxOffset;
  result.baseMaxHpAddress = s3 + kHpBaseMaxOffset;
  result.maxFpAddress = s3 + kFpMaxOffset;
  result.baseMaxFpAddress = s3 + kFpBaseMaxOffset;
  result.maxStaminaAddress = s3 + kStaminaMaxOffset;
  result.baseMaxStaminaAddress = s3 + kStaminaBaseMaxOffset;

  uintptr_t physics = 0;
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(s2 + kChrPhysicsModuleSlot), &physics,
                    sizeof(physics), &br);
  result.physicsModuleAddress = physics;
  if (physics != 0) {
    result.positionAddress = physics + kPhysicsPositionOffset;
    result.facingAddress = physics + kPhysicsFacingOffset;
  }
  return result;
}

PlayerPose ReadPlayerPose(HANDLE hProcess, const ResolvedPointers& resolved) {
  PlayerPose pose;
  if (resolved.positionAddress == 0) return pose;
  float xyz[3] = {};
  SIZE_T br = 0;
  pose.ok = ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.positionAddress), xyz,
                              sizeof(xyz), &br) &&
            ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.facingAddress),
                              &pose.facingRadians, sizeof(pose.facingRadians), &br);
  pose.x = xyz[0];
  pose.y = xyz[1];
  pose.z = xyz[2];
  return pose;
}

// The game's current player object (WorldChrMan + 0x80, "s1"), or 0 when
// no character is loaded (main menu, mid-load). Found 2026-10-06: after
// quit-to-menu the cached chain kept "reading" (0, 1, 0) from the
// destroyed character, and after a reload the player can be rebuilt at
// a new address -- so live modes re-check this every tick (2 reads) and
// re-resolve every chain when it changes. wcmCellOffset comes from a
// ResolvePlayerHp call; it's set even when that call fails because no
// player is loaded yet.
uintptr_t ReadCurrentPlayerIns(HANDLE hProcess, uintptr_t moduleBase, uintptr_t wcmCellOffset) {
  if (moduleBase == 0 || wcmCellOffset == 0) return 0;
  uintptr_t wcm = 0, playerIns = 0;
  SIZE_T br = 0;
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(moduleBase + wcmCellOffset), &wcm,
                         sizeof(wcm), &br) ||
      wcm == 0)
    return 0;
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(wcm + 0x80), &playerIns,
                    sizeof(playerIns), &br);
  return playerIns;
}

// ---- current area --------------------------------------
//
// Offsets and their meanings from The Grand Archives' DS3 Cheat Engine
// table (github.com/The-Grand-Archives/Dark-Souls-III-CT-TGA; no explicit
// licence, so only facts are used: offsets and what each value means --
// no code or text copied). Cheat Engine lists
// offsets innermost-last, so its [1FE0, 80] off WorldChrMan is
// [WorldChrMan + 0x80] + 0x1FE0:
//   map section: player (s1) + 0x1FE0, 4 bytes = mAA_BB_CC_DD, AA in the
//                top byte (e.g. 0x28000000 = m40_00_00_00)
//   play region: player (s1) + 0x1ABC, i32 -- the named sub-area behind
//                the area banner (PlayRegionParam row id)
//   last bonfire: [GameMan] + 0xACC, i32 bonfire entity id
//                (e.g. 4002950 = Firelink Shrine)
// GameMan is found by our own pattern (see "memory roots"):
// mov rax,[rip+GameMan]; mov r12d,10000; movzx ebx,word [rax+..]
// (270 references).
constexpr const char* kGameManPattern = "48 8B 05 ?? ?? ?? ?? 41 BC 10 27 00 00 0F B7 98";
constexpr uintptr_t kPlayerMapIdOffset = 0x1FE0;
constexpr uintptr_t kPlayerPlayRegionOffset = 0x1ABC;
constexpr uintptr_t kGameManLastBonfireOffset = 0xACC;

// Module-relative offset of the GameMan pointer cell, or 0.
uintptr_t ResolveGameManCellOffset(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize) {
  uintptr_t instr = FindPatternInModule(hProcess, moduleBase, moduleSize, ParsePattern(kGameManPattern));
  if (instr == 0) return 0;
  int32_t disp = 0;
  SIZE_T br = 0;
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(instr + 3), &disp, sizeof(disp), &br))
    return 0;
  return static_cast<uintptr_t>(static_cast<intptr_t>(instr) + 7 + disp) - moduleBase;
}

struct PlayerArea {
  bool ok = false;
  uint32_t mapId = 0;        // packed mAA_BB_CC_DD
  int32_t playRegionId = 0;
  int32_t lastBonfireId = -1;
  std::string MapName() const {  // "m40_00_00_00"
    char buf[16];
    std::snprintf(buf, sizeof(buf), "m%02u_%02u_%02u_%02u", (mapId >> 24) & 0xFF,
                  (mapId >> 16) & 0xFF, (mapId >> 8) & 0xFF, mapId & 0xFF);
    return buf;
  }
};

// playerIns = the current player object (ReadCurrentPlayerIns / s1).
PlayerArea ReadPlayerArea(HANDLE hProcess, uintptr_t moduleBase, uintptr_t playerIns,
                          uintptr_t gameManCellOffset) {
  PlayerArea area;
  if (playerIns == 0) return area;
  SIZE_T br = 0;
  area.ok = ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(playerIns + kPlayerMapIdOffset),
                              &area.mapId, sizeof(area.mapId), &br) &&
            ReadProcessMemory(hProcess,
                              reinterpret_cast<LPCVOID>(playerIns + kPlayerPlayRegionOffset),
                              &area.playRegionId, sizeof(area.playRegionId), &br);
  uintptr_t gameMan = 0;
  if (gameManCellOffset != 0 &&
      ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(moduleBase + gameManCellOffset),
                        &gameMan, sizeof(gameMan), &br) &&
      gameMan != 0) {
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(gameMan + kGameManLastBonfireOffset),
                      &area.lastBonfireId, sizeof(area.lastBonfireId), &br);
  }
  return area;
}

// id -> name tables: data/region_areas.tsv, and the generated regions.tsv
// and bonfire_names.tsv (made from the user's own game files by
// tools/extract_treasures.py).
// Format: '#' comment lines, then "id<TAB>name". Missing file = no names.
std::unordered_map<int32_t, std::string> LoadIdNameTable(const std::wstring& path) {
  std::unordered_map<int32_t, std::string> names;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    size_t tab = line.find('\t');
    if (tab == std::string::npos) continue;
    names[static_cast<int32_t>(std::strtol(line.c_str(), nullptr, 10))] = line.substr(tab + 1);
  }
  return names;
}
// A bare file name would quietly read from the current folder: pass a full
// path from Paths() (Data or Generated).
std::unordered_map<int32_t, std::string> LoadIdNameTable(const wchar_t* fileName) = delete;

// The bonfire names and region labels (generated), reloaded with the map
// data -- defined with the map-data tables below.
const std::unordered_map<int32_t, std::string>& BonfireNames();
const std::unordered_map<int32_t, std::string>& RegionNames();

struct AreaNames {
  std::string Region(int32_t id) const {
    const auto& regions = RegionNames();
    auto it = regions.find(id);
    return it != regions.end() ? it->second : "region " + std::to_string(id);
  }
  std::string Bonfire(int32_t id) const {
    const auto& bonfires = BonfireNames();
    auto it = bonfires.find(id);
    return it != bonfires.end() ? it->second : "bonfire " + std::to_string(id);
  }
};

// Play region reads 0 between regions (riding an elevator, mid-load) --
// live-observed 2026-10-06. Displays keep the last real region instead.
struct AreaTracker {
  int32_t lastRegion = 0;
  void Update(const PlayerArea& area) {
    if (area.ok && area.playRegionId != 0) lastRegion = area.playRegionId;
  }
};

// ---- areas, session stats -------------------------------
//
// data/region_areas.tsv groups play regions into the 22 areas players
// talk about, each a place name from the game's own text (written by
// tools/extract_treasures.py --region-areas from the game's files and
// four calls made by hand).
struct AreaInfo {
  std::unordered_map<int32_t, std::string> regionArea;

  AreaInfo() {
    for (const auto& [id, area] : LoadIdNameTable(Paths().Data(L"region_areas.tsv"))) regionArea[id] = area;
  }
  // "" for regions with no area (PvP arenas) or unknown ids.
  std::string AreaOf(int32_t regionId) const {
    auto it = regionArea.find(regionId);
    return it != regionArea.end() ? it->second : "";
  }
};

// Death count and play time, from GameDataMan (= this project's BaseA
// cell). Offsets from The Grand Archives' table: "Death Num" +0x98,
// "True Death Num" +0x94, "Play Time" +0xA4. UNVERIFIED until checked
// live (docs/TECHNICAL.md).
constexpr uintptr_t kGameDataDeathNumOffset = 0x98;
constexpr uintptr_t kGameDataTrueDeathNumOffset = 0x94;
constexpr uintptr_t kGameDataPlayTimeOffset = 0xA4;

struct GameCounters {
  bool ok = false;
  int32_t deathNum = 0, trueDeathNum = 0;
  uint32_t playTime = 0;  // raw; units checked live (docs/TECHNICAL.md)
};

GameCounters ReadGameCounters(HANDLE hProcess, uintptr_t moduleBase, uintptr_t baseACellOffset) {
  GameCounters c;
  uintptr_t gameDataMan = 0;
  SIZE_T br = 0;
  if (baseACellOffset == 0 ||
      !ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(moduleBase + baseACellOffset),
                         &gameDataMan, sizeof(gameDataMan), &br) ||
      gameDataMan == 0)
    return c;
  c.ok = ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(gameDataMan + kGameDataDeathNumOffset),
                           &c.deathNum, sizeof(c.deathNum), &br) &&
         ReadProcessMemory(hProcess,
                           reinterpret_cast<LPCVOID>(gameDataMan + kGameDataTrueDeathNumOffset),
                           &c.trueDeathNum, sizeof(c.trueDeathNum), &br) &&
         ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(gameDataMan + kGameDataPlayTimeOffset),
                           &c.playTime, sizeof(c.playTime), &br);
  return c;
}

// Per-run session stats, fed once per polling tick. Counts only while
// the tool is polling and a character is loaded (F10 pause and menus
// don't count). Souls bookkeeping:
//   - a death (Death Num going up) loses the souls held just before
//     they dropped to 0 -- the drop and the counter can land on
//     different ticks, in either order, so both are handled;
//   - souls coming back by exactly the last loss = bloodstain recovered
//     (not counted as gained); dying again before that forfeits it;
//   - any other increase = souls gained; a decrease that isn't a death
//     (levelling up, buying) is spending, not a loss.
struct SessionStats {
  bool started = false;
  double seconds = 0.0;
  int32_t deaths = 0, lastDeathNum = 0, lastSouls = 0;
  int64_t gained = 0, lost = 0, recovered = 0;
  int64_t bloodstain = 0;        // souls on the ground, recoverable
  int64_t recentDrop = 0;        // souls held before the last drop to 0
  double recentDropAt = -1e9;    // when that drop happened (session seconds)
  bool deathAwaitingDrop = false;
  std::unordered_map<std::string, double> areaSeconds;
  std::string lastEvent;  // "Died: lost 1,234 souls" etc., for logs
  int64_t eventLost = -1, eventRecovered = -1;  // this tick's loss / recovery, -1 = none

  // Returns true when something worth logging happened (lastEvent set).
  bool Tick(double dt, int32_t souls, int32_t deathNum, const std::string& area) {
    lastEvent.clear();
    eventLost = eventRecovered = -1;
    if (!started) {
      started = true;
      lastDeathNum = deathNum;
      lastSouls = souls;
      return false;
    }
    seconds += dt;
    if (!area.empty()) areaSeconds[area] += dt;

    auto recordLoss = [&](int64_t amount) {
      if (bloodstain > 0) lastEvent = "Previous bloodstain forfeited. ";
      lost += amount;
      bloodstain = amount;
      eventLost = amount;
      lastEvent += "Died: lost " + std::to_string(amount) + " souls.";
    };
    if (souls != lastSouls) {
      int64_t delta = static_cast<int64_t>(souls) - lastSouls;
      if (souls == 0 && lastSouls > 0) {
        recentDrop = lastSouls;
        recentDropAt = seconds;
        if (deathAwaitingDrop) {
          recordLoss(recentDrop);
          deathAwaitingDrop = false;
          recentDrop = 0;
        }
      } else if (delta > 0) {
        if (bloodstain > 0 && delta == bloodstain) {
          recovered += delta;
          bloodstain = 0;
          eventRecovered = delta;
          lastEvent = "Bloodstain recovered: " + std::to_string(delta) + " souls.";
        } else {
          gained += delta;
        }
      }
    }
    if (deathNum > lastDeathNum) {
      deaths += deathNum - lastDeathNum;
      if (souls == 0 && recentDrop > 0 && seconds - recentDropAt < 15.0) {
        recordLoss(recentDrop);
        recentDrop = 0;
      } else if (souls > 0) {
        deathAwaitingDrop = true;  // the drop to 0 hasn't happened yet
      } else {
        recordLoss(0);
      }
    }
    lastDeathNum = deathNum;
    lastSouls = souls;
    return !lastEvent.empty();
  }
  double SoulsPerHour() const { return seconds >= 60.0 ? gained / (seconds / 3600.0) : 0.0; }
  double AreaSeconds(const std::string& area) const {
    auto it = areaSeconds.find(area);
    return it != areaSeconds.end() ? it->second : 0.0;
  }
};

std::string FormatDuration(double seconds) {
  int s = static_cast<int>(seconds);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
  return buf;
}

// ---- session recording ---------------------------------
//
// Every live session (stats --live, window, overlay) is saved as JSON
// Lines under sessions/<character>/<YYYYMMDD-HHMMSS>.jsonl, one event per
// line, flushed as written so a crash or kill loses nothing. Sessions
// shorter than kMinRecordedSessionSeconds aren't saved (quick checks
// shouldn't leave clutter): events are buffered until then. `report`
// turns every saved session into a results page. sessions/ is in
// .gitignore -- it's personal play data.
//
// Event types ("t" = session seconds, the same clock as SessionStats):
//   start    character, level, deathsTotal, playTimeMs, startedAt
//   enter    area                                   (area change)
//   death    area, x, y, z, soulsLost, deathsTotal
//   bloodstain souls                                (recovered)
//   level    from, to
//   sample   level, souls, deathsTotal, area, playTimeMs   (every 30s)
//   pos      x, y, z   -- the walked trail for the results page's 3D map:
//            written once the player has moved kTrailMinStep units since
//            the last pos, at most every kTrailMinSeconds (~100-200 KB/hour)
//   bonfire  name, x, y, z   (last rested bonfire changed)
//   end      seconds, deaths, gained, lost, recovered, level, deathsTotal, playTimeMs
constexpr double kMinRecordedSessionSeconds = 60.0;
constexpr double kSessionSampleSeconds = 30.0;
constexpr double kTrailMinStep = 1.0;      // game units (~1 m)
constexpr double kTrailMinSeconds = 0.5;

// Character name: PlayerGameData + 0x88, UTF-16 (The Grand Archives'
// "Character Name"); verified live 2026-10-06.
constexpr uintptr_t kPlayerGameDataNameOffset = 0x88;

std::string ReadCharacterName(HANDLE hProcess, uintptr_t playerGameData) {
  if (playerGameData == 0) return "";
  wchar_t buf[33] = {};
  SIZE_T br = 0;
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(playerGameData + kPlayerGameDataNameOffset),
                         buf, 32 * sizeof(wchar_t), &br))
    return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return "";
  std::string out(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), n, nullptr, nullptr);
  return out;
}

// ---- spoiler tiers -------------------------------------
//
// One tier for every hint, chosen by the user 2026-10-06:
//   Off      -- no guidance (counts only)
//   Vague    -- "something unfound, 40 m, 3 o'clock"; boss counts only
//   Category -- what kind of thing ("a ring"); defeated bosses named
//   Full     -- names everything
// Default Vague. F9 cycles it in the window/overlay and saves it to
// settings.ini (Paths().Settings(), git-ignored); commands follow it too, and
// `--full` shows everything for one run. Things you already have (found
// items, held keys, defeated bosses at Category+) are never hidden.
enum : int { kTierOff = 0, kTierVague = 1, kTierCategory = 2, kTierFull = 3 };
volatile LONG g_spoilerTier = kTierVague;

int SpoilerTier() { return static_cast<int>(g_spoilerTier); }

const char* SpoilerTierName(int tier) {
  static const char* names[] = {"Off", "Vague", "Category", "Full"};
  return names[(tier < 0 || tier > 3) ? kTierVague : tier];
}

std::wstring SettingsPath() { return Paths().Settings(); }

// Overlay settings, changed live from the live page (and F11 for perf).
// Written by the page server thread / F11, read by the UI thread: each is
// a whole LONG, so no lock is needed. Saved with the tier in settings.ini.
struct OverlaySettings {
  volatile LONG visible = 1;  // shown at all (F10 still hides it AND pauses polling)
  volatile LONG nearest = 1, bosses = 1, bossHint = 1, route = 1, missable = 1, perf = 0;  // which lines show
  volatile LONG corner = 0;    // of the game window: 0 top-right, 1 top-left, 2 bottom-right, 3 bottom-left
  volatile LONG opacity = 88;  // percent
  volatile LONG text = 15;     // font px at 96 DPI
  volatile LONG width = 340;   // panel px at 96 DPI
};
OverlaySettings g_ov;

// Live page settings (POST /api/page), saved with the tier: the theme and
// which panels are muted. The page owns no state of its own beyond these.
const char* const kPageThemes[3] = {"Bonfire", "Ledger", "Cartographer"};  // API ids a, b, c
const char* const kPagePanels[6] = {"char", "map", "boss", "gear", "items", "route"};
volatile LONG g_pageTheme = 0;  // index into kPageThemes
volatile LONG g_pageMuted = 0;  // bit i = kPagePanels[i] muted

// "map,items" -> bits; false if any name is unknown (nothing changed then).
bool SetPageMuted(const std::string& list) {
  LONG bits = 0;
  for (size_t pos = 0; pos <= list.size();) {
    size_t comma = list.find(',', pos);
    std::string name = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    if (!name.empty()) {
      int found = -1;
      for (int i = 0; i < 6; ++i)
        if (name == kPagePanels[i]) found = i;
      if (found < 0) return false;
      bits |= 1L << found;
    }
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  InterlockedExchange(&g_pageMuted, bits);
  return true;
}

std::string PageMutedList() {
  std::string out;
  for (int i = 0; i < 6; ++i)
    if (g_pageMuted & (1L << i)) out += (out.empty() ? "" : ",") + std::string(kPagePanels[i]);
  return out;
}

struct OverlaySettingDef {
  const char* key;  // in the API and (prefixed "overlay_") in settings.ini
  volatile LONG* value;
  LONG min, max;
};
const OverlaySettingDef kOverlaySettingDefs[] = {
    {"visible", &g_ov.visible, 0, 1}, {"nearest", &g_ov.nearest, 0, 1}, {"bosses", &g_ov.bosses, 0, 1},
    {"bossHint", &g_ov.bossHint, 0, 1}, {"route", &g_ov.route, 0, 1},   {"missable", &g_ov.missable, 0, 1},
    {"perf", &g_ov.perf, 0, 1},
    {"corner", &g_ov.corner, 0, 3},
    {"opacity", &g_ov.opacity, 30, 100}, {"text", &g_ov.text, 11, 24},  {"width", &g_ov.width, 240, 560},
};

// Validated set from text ("corner" also takes top-right / top-left /
// bottom-right / bottom-left). False if the key or value is invalid.
bool SetOverlaySetting(const std::string& key, const std::string& value) {
  static const char* corners[] = {"top-right", "top-left", "bottom-right", "bottom-left"};
  for (const auto& d : kOverlaySettingDefs) {
    if (key != d.key) continue;
    long v = -1;
    if (key == "corner")
      for (int c = 0; c < 4; ++c)
        if (value == corners[c]) v = c;
    char* end = nullptr;
    if (v < 0) v = std::strtol(value.c_str(), &end, 10);
    if (v < 0 && end == value.c_str()) return false;
    if (end && (end == value.c_str() || *end)) return false;
    if (v < d.min || v > d.max) return false;
    InterlockedExchange(d.value, v);
    return true;
  }
  return false;
}

// The DS3 "Game" folder the map data was read from (UTF-8; "" = unknown).
// Set by `extract` / the first-run step before any other thread starts.
std::string g_gameDir;

void LoadSettings() {
  std::ifstream in(SettingsPath());
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k == "spoiler_tier") {
      for (int t = 0; t <= 3; ++t)
        if (_stricmp(v.c_str(), SpoilerTierName(t)) == 0) InterlockedExchange(&g_spoilerTier, t);
    } else if (k.rfind("overlay_", 0) == 0) {
      SetOverlaySetting(k.substr(8), v);  // bad values are ignored, keeping the default
    } else if (k == "page_theme") {
      for (int t = 0; t < 3; ++t)
        if (_stricmp(v.c_str(), kPageThemes[t]) == 0) InterlockedExchange(&g_pageTheme, t);
    } else if (k == "page_muted") {
      SetPageMuted(v);
    } else if (k == "game_dir") {
      g_gameDir = v;
    }
  }
}

void SaveSettings() {
  static const char* corners[] = {"top-right", "top-left", "bottom-right", "bottom-left"};
  EnsureDir(Paths().user);
  std::ofstream out(SettingsPath(), std::ios::trunc);
  out << "# WASD settings (also changed from the live page).\n"
      << "# spoiler_tier: Off, Vague, Category or Full (F9 in game cycles it).\n"
      << "spoiler_tier=" << SpoilerTierName(SpoilerTier()) << "\n";
  for (const auto& d : kOverlaySettingDefs) {
    out << "overlay_" << d.key << "=";
    if (std::string(d.key) == "corner") out << corners[*d.value & 3] << "\n";
    else out << *d.value << "\n";
  }
  out << "# Live page: theme (Bonfire, Ledger or Cartographer) and muted panels.\n"
      << "page_theme=" << kPageThemes[g_pageTheme % 3] << "\n"
      << "page_muted=" << PageMutedList() << "\n";
  out << "# The DS3 Game folder map data is read from (wasd-cli.exe extract).\n"
      << "game_dir=" << g_gameDir << "\n";
}

int CycleSpoilerTier() {
  int next = (SpoilerTier() + 1) % 4;
  InterlockedExchange(&g_spoilerTier, next);
  SaveSettings();
  return next;
}

std::wstring WidenUtf8(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string JsonString(const std::string& s) {
  std::string out = "\"";
  for (unsigned char ch : s) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (ch < 0x20) {
          char esc[8];
          std::snprintf(esc, sizeof(esc), "\\u%04x", ch);
          out += esc;
        } else {
          out += static_cast<char>(ch);
        }
    }
  }
  return out + "\"";
}

// Folder-safe version of a character name (keeps letters, digits, space, - _).
std::wstring SafeFolderName(const std::string& name) {
  std::wstring w = WidenUtf8(name), out;
  for (wchar_t c : w) out += (iswalnum(c) || c == L' ' || c == L'-' || c == L'_') ? c : L'_';
  return out.empty() ? L"Unknown" : out;
}

struct SessionContext {
  int32_t level = 0, souls = 0, deathNum = 0;
  uint32_t playTimeMs = 0;
  std::string area, characterName;
  PlayerPose pose;
  int32_t lastBonfireId = -1;
  std::string lastBonfireName;
};

// Columns of sessions/<character>/<stamp>.perf.csv (see SessionLog::WritePerf):
// local time at the window's end, session seconds (the .jsonl's "t"), the
// window's length and poll count, poll rate, read latency (avg / max, of
// one tick's reads), this process's CPU (% of one core) and memory.
constexpr const char kPerfCsvHeader[] =
    "time,session_t,window_s,ticks,rate_hz,latency_avg_us,latency_max_us,cpu_pct_core,working_set_mb,private_mb";

class SessionLog {
 public:
  bool enabled = false;
  std::wstring path;  // set once the file is opened

  void Write(const std::string& line, double sessionSeconds) {
    if (!enabled) return;
    if (!file_) {
      pending_.push_back(line);
      if (sessionSeconds < kMinRecordedSessionSeconds) return;
      Open();
      if (!file_) return;
      for (const auto& l : pending_) std::fputs((l + "\n").c_str(), file_);
      pending_.clear();
    } else {
      std::fputs((line + "\n").c_str(), file_);
    }
    std::fflush(file_);
  }
  // Perf figures for the Milestone 5 stress test: one CSV row per perf
  // window (5s), in <stamp>.perf.csv beside the .jsonl. It opens with the
  // .jsonl (rows before that are buffered), so a session too short to be
  // saved leaves no CSV either. Rows come from PerfCsvRow.
  void WritePerf(const std::string& row) {
    if (!enabled) return;
    if (!perfFile_) {
      pendingPerf_.push_back(row);
      return;
    }
    std::fputs((row + "\n").c_str(), perfFile_);
    std::fflush(perfFile_);
  }
  void SetCharacter(const std::string& name) {
    if (characterName_.empty()) characterName_ = name;
  }
  void Close() {
    if (file_) std::fclose(file_);
    file_ = nullptr;
    if (perfFile_) std::fclose(perfFile_);
    perfFile_ = nullptr;
  }
  ~SessionLog() { Close(); }

 private:
  void Open() {
    std::wstring dir = Paths().Sessions() + L"\\" + SafeFolderName(characterName_);
    EnsureDir(dir);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[32];
    std::swprintf(stamp, 32, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond);
    path = dir + L"\\" + stamp + L".jsonl";
    // Shared for reading (not writing), so the session can be read -- or a
    // `report` built -- while the game is still being played.
    file_ = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
    if (file_) Log("Recording this session to %ls", path.c_str());
    if (!file_) return;
    std::wstring perfPath = dir + L"\\" + stamp + L".perf.csv";
    perfFile_ = _wfsopen(perfPath.c_str(), L"w", _SH_DENYWR);
    if (!perfFile_) return;
    std::fputs((std::string(kPerfCsvHeader) + "\n").c_str(), perfFile_);
    for (const auto& r : pendingPerf_) std::fputs((r + "\n").c_str(), perfFile_);
    pendingPerf_.clear();
    std::fflush(perfFile_);
  }
  FILE* file_ = nullptr;
  FILE* perfFile_ = nullptr;
  std::vector<std::string> pending_, pendingPerf_;
  std::string characterName_;
};

// SessionStats + recording. Live modes call Tick() once per polling tick
// and Finish() on exit.
struct SessionTracker {
  SessionStats stats;
  SessionLog log;
  int32_t lastLevel = -1;
  std::string lastArea;
  double nextSampleAt = 0.0;
  int32_t lastDeathNum = 0;
  uint32_t lastPlayTimeMs = 0;
  bool finished = false;
  bool haveTrail = false;
  float trailX = 0, trailY = 0, trailZ = 0;
  double trailT = -1e9;
  int32_t lastBonfireId = -1;

  bool Tick(double dt, const SessionContext& c) {
    char buf[512];
    bool firstTick = !stats.started;
    if (firstTick) {
      log.SetCharacter(c.characterName);
      SYSTEMTIME st;
      GetLocalTime(&st);
      char when[32];
      std::snprintf(when, sizeof(when), "%04u-%02u-%02uT%02u:%02u:%02u", st.wYear, st.wMonth,
                    st.wDay, st.wHour, st.wMinute, st.wSecond);
      std::snprintf(buf, sizeof(buf),
                    "{\"type\":\"start\",\"t\":0,\"character\":%s,\"level\":%d,\"deathsTotal\":%d,"
                    "\"playTimeMs\":%u,\"startedAt\":\"%s\"}",
                    JsonString(c.characterName).c_str(), c.level, c.deathNum, c.playTimeMs, when);
      log.Write(buf, 0.0);
      lastLevel = c.level;
    }
    bool event = stats.Tick(dt, c.souls, c.deathNum, c.area);
    double t = stats.seconds;
    if (!c.area.empty() && c.area != lastArea) {
      std::snprintf(buf, sizeof(buf), "{\"type\":\"enter\",\"t\":%.1f,\"area\":%s}", t,
                    JsonString(c.area).c_str());
      log.Write(buf, t);
      lastArea = c.area;
    }
    // Placeholder (0, 1, 0) is what an unloaded character reads; skip it.
    bool realPose = c.pose.ok && !(c.pose.x == 0.0f && c.pose.y == 1.0f && c.pose.z == 0.0f);
    if (realPose) {
      float dx = c.pose.x - trailX, dy = c.pose.y - trailY, dz = c.pose.z - trailZ;
      bool moved = !haveTrail || dx * dx + dy * dy + dz * dz >= kTrailMinStep * kTrailMinStep;
      if (moved && t - trailT >= kTrailMinSeconds) {
        std::snprintf(buf, sizeof(buf), "{\"type\":\"pos\",\"t\":%.1f,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}",
                      t, c.pose.x, c.pose.y, c.pose.z);
        log.Write(buf, t);
        trailX = c.pose.x;
        trailY = c.pose.y;
        trailZ = c.pose.z;
        trailT = t;
        haveTrail = true;
      }
      if (c.lastBonfireId > 0 && c.lastBonfireId != lastBonfireId) {
        if (lastBonfireId != -1) {  // not on the first tick: that's where the session started
          std::snprintf(buf, sizeof(buf),
                        "{\"type\":\"bonfire\",\"t\":%.1f,\"name\":%s,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}",
                        t, JsonString(c.lastBonfireName).c_str(), c.pose.x, c.pose.y, c.pose.z);
          log.Write(buf, t);
        }
        lastBonfireId = c.lastBonfireId;
      }
    }
    if (stats.eventLost >= 0) {
      std::snprintf(buf, sizeof(buf),
                    "{\"type\":\"death\",\"t\":%.1f,\"area\":%s,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,"
                    "\"soulsLost\":%lld,\"deathsTotal\":%d}",
                    t, JsonString(lastArea).c_str(), c.pose.x, c.pose.y, c.pose.z,
                    static_cast<long long>(stats.eventLost), c.deathNum);
      log.Write(buf, t);
    }
    if (stats.eventRecovered >= 0) {
      std::snprintf(buf, sizeof(buf), "{\"type\":\"bloodstain\",\"t\":%.1f,\"souls\":%lld}", t,
                    static_cast<long long>(stats.eventRecovered));
      log.Write(buf, t);
    }
    if (!firstTick && c.level != lastLevel && c.level > 0) {
      std::snprintf(buf, sizeof(buf), "{\"type\":\"level\",\"t\":%.1f,\"from\":%d,\"to\":%d}", t,
                    lastLevel, c.level);
      log.Write(buf, t);
      lastLevel = c.level;
    }
    if (t >= nextSampleAt) {
      std::snprintf(buf, sizeof(buf),
                    "{\"type\":\"sample\",\"t\":%.1f,\"level\":%d,\"souls\":%d,\"deathsTotal\":%d,"
                    "\"area\":%s,\"playTimeMs\":%u}",
                    t, c.level, c.souls, c.deathNum, JsonString(lastArea).c_str(), c.playTimeMs);
      log.Write(buf, t);
      nextSampleAt = t + kSessionSampleSeconds;
    }
    lastDeathNum = c.deathNum;
    lastPlayTimeMs = c.playTimeMs;
    return event;
  }

  void Finish() {
    if (finished || !stats.started) return;
    finished = true;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"type\":\"end\",\"t\":%.1f,\"seconds\":%.1f,\"deaths\":%d,\"gained\":%lld,"
                  "\"lost\":%lld,\"recovered\":%lld,\"level\":%d,\"deathsTotal\":%d,"
                  "\"playTimeMs\":%u}",
                  stats.seconds, stats.seconds, stats.deaths, static_cast<long long>(stats.gained),
                  static_cast<long long>(stats.lost), static_cast<long long>(stats.recovered),
                  lastLevel, lastDeathNum, lastPlayTimeMs);
    log.Write(buf, stats.seconds);
    log.Close();
  }
};



// ---- current equip load / item discovery (a THIRD, independent anchor --
// not WorldChrMan, not BaseA) ------------------------------------------
//
// Neither of these had any source lead in DS3RuntimeScripting,
// veeenu/darksoulsiii-practice-tool, gitDanilo/GitGud, or The Grand
// Archives' Cheat Engine table (all checked). Found live instead, the
// same way HP originally was: `fscan`/`frescan` against the actual
// status-screen value, narrowed to one address across three
// independent value changes (18.6 -> 14.6 -> 31.6, then a fresh scan
// -> 17.1 -> 14.6 again after a long gap -- all confirmed correct via
// [BitConverter]::ToSingle on the raw bits, not just eyeballing decimal
// digits). `findptr` then traced that address back to a static,
// module-embedded pointer cell (`DarkSoulsIII.exe+0x47F3B98`) -- the
// same category of anchor as WorldChrMan/BaseA (a fixed offset into the
// module's own data section, re-derived fresh each run, not a
// hardcoded absolute address), just found by pointer-tracing a scan hit
// instead of an AOB byte-signature scan. Landed in what looks like a
// menu/status-screen display cache rather than a live gameplay struct:
// current equip load sits at `+0x1E7C` off the dereferenced cell, and
// exactly 8 bytes later (`+0x1E84`) sits a plain int32 that matched the
// user's reported Item Discovery value (107) exactly on first look --
// a strong circumstantial match (right next to a verified real stat,
// right data type for a value DS3 always displays as a whole number),
// but unlike current equip load it hasn't been through its own
// independent live-change test yet (see docs/TECHNICAL.md).
//
// Max equip load was NOT found nearby (no plausible ~48.0 float turned
// up in a wide peek around this struct) and remains unresolved -- it's
// also less amenable to this technique anyway, since it only changes on
// a Vitality level-up, not something a quick gear swap can trigger for
// a scan/rescan narrowing pass.
constexpr uintptr_t kDisplayStatsCellOffset = 0x47F3B98;  // module-relative, found via findptr
constexpr uintptr_t kCurrentEquipLoadOffset = 0x1E7C;     // f32, live-verified
constexpr uintptr_t kItemDiscoveryOffset = 0x1E84;        // int32, circumstantial match only

struct ResolvedDisplayStats {
  uintptr_t currentEquipLoadAddress = 0;
  uintptr_t itemDiscoveryAddress = 0;
};

ResolvedDisplayStats ResolveDisplayStats(HANDLE hProcess, uintptr_t moduleBase) {
  ResolvedDisplayStats result;
  SIZE_T br = 0;
  uintptr_t cellAddr = moduleBase + kDisplayStatsCellOffset;
  uintptr_t m = 0;
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(cellAddr), &m, sizeof(m), &br) ||
      m == 0)
    return result;
  result.currentEquipLoadAddress = m + kCurrentEquipLoadOffset;
  result.itemDiscoveryAddress = m + kItemDiscoveryOffset;
  return result;
}

int RunWcm(bool haveTarget, uintptr_t target) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;

  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  if (moduleBase == 0) {
    Log("Could not resolve module base/size.");
    CloseHandle(hProcess);
    return 4;
  }
  Log("Module base: 0x%llx size: 0x%zx", static_cast<unsigned long long>(moduleBase), moduleSize);

  Log("Scanning module image for WorldChrMan/XA byte patterns...");
  auto resolved = ResolvePlayerHp(hProcess, moduleBase, moduleSize);

  if (resolved.worldChrManCellOffset != 0) {
    Log("WorldChrMan cell at DarkSoulsIII.exe+0x%llx",
        static_cast<unsigned long long>(resolved.worldChrManCellOffset));
  } else {
    Log("WorldChrMan pattern NOT found in this module image. Game build/patch may differ from "
        "what the pattern targets.");
  }
  if (resolved.xaConstant != 0) {
    Log("XA constant = 0x%llx", static_cast<unsigned long long>(resolved.xaConstant));
  }
  if (resolved.entityAddress != 0) {
    Log("Entity address (s1, world_chr_man+0x80 deref'd) = 0x%llx",
        static_cast<unsigned long long>(resolved.entityAddress));
  }
  if (resolved.structBase != 0) {
    Log("Resolved struct base = 0x%llx  <- world_chr_man -> +0x80 -> +xa -> +0x18",
        static_cast<unsigned long long>(resolved.structBase));
  }
  if (resolved.hpAddress != 0) {
    Log("HP address = 0x%llx (struct + 0x%llx)", static_cast<unsigned long long>(resolved.hpAddress),
        static_cast<unsigned long long>(kHpFieldOffset));
  }

  if (haveTarget && resolved.structBase != 0) {
    if (target >= resolved.structBase && (target - resolved.structBase) < 0x1000) {
      Log("target - struct = 0x%llx  <- candidate final offset for `resolve`",
          static_cast<unsigned long long>(target - resolved.structBase));
    } else {
      Log("target doesn't land near this struct (diff too large or negative) -- this chain "
          "doesn't reach it.");
    }
  }

  CloseHandle(hProcess);
  return resolved.hpAddress != 0 ? 0 : 6;
}

int RunHp(int iterations) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;

  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  if (moduleBase == 0) {
    Log("Could not resolve module base/size.");
    CloseHandle(hProcess);
    return 4;
  }

  Log("Resolving player HP pointer chain (WorldChrMan AOB scan, re-derived fresh)...");
  auto resolved = ResolvePlayerHp(hProcess, moduleBase, moduleSize);
  if (resolved.hpAddress == 0) {
    Log("FAILED to resolve HP address -- a pattern scan or a dereference along the chain came "
        "up empty. The game patch may have changed since this chain was verified. See "
        "docs/TECHNICAL.md, or re-derive with `wcm`.");
    CloseHandle(hProcess);
    return 6;
  }
  Log("Resolved: DarkSoulsIII.exe+0x%llx -> +0x80 -> +0x%llx -> +0x18 -> +0x%llx = 0x%llx",
      static_cast<unsigned long long>(resolved.worldChrManCellOffset),
      static_cast<unsigned long long>(resolved.xaConstant),
      static_cast<unsigned long long>(kHpFieldOffset),
      static_cast<unsigned long long>(resolved.hpAddress));

  Log("Polling HP at 10Hz for %d reads (Ctrl+C to stop early)...", iterations);

  double sumMicros = 0.0, maxMicros = 0.0;
  int okCount = 0;
  for (int i = 0; i < iterations; ++i) {
    int32_t v = 0;
    SIZE_T bytesRead = 0;

    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    BOOL ok = ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.hpAddress), &v,
                                 sizeof(v), &bytesRead);
    QueryPerformanceCounter(&end);
    double micros = (end.QuadPart - start.QuadPart) * 1000000.0 / freq.QuadPart;
    sumMicros += micros;
    if (micros > maxMicros) maxMicros = micros;

    if (ok && bytesRead == sizeof(v)) {
      ++okCount;
      Log("HP=%d latency=%.3fus", v, micros);
    } else {
      Log("read FAILED (error %lu) latency=%.3fus", GetLastError(), micros);
    }
    Sleep(100);
  }

  CloseHandle(hProcess);
  Log("Done. %d/%d reads ok, avg latency=%.3fus, max=%.3fus, ~10Hz poll rate.", okCount,
      iterations, sumMicros / iterations, maxMicros);
  return 0;
}

// ---- shared with equip/inventory (full definitions further down this
// file, near RunEquip) -- forward-declared here so RunStats can use
// them for live level-up / gear-change re-resolution. ItemNameTable
// needs its full definition this early since RunStats holds one by
// value; everything else is fine as a plain prototype.
struct ItemNameTable {
  bool loaded = false;
  std::unordered_map<std::string, std::string> weapon;
  std::unordered_map<std::string, std::string> protector;
  std::unordered_map<std::string, std::string> accessory;
  std::unordered_map<std::string, std::string> goods;
  std::unordered_map<std::string, std::string> magic;  // spells -- direct id lookup, no uniqueId
};
ItemNameTable LoadItemNameTable(const std::wstring& path);
std::wstring FindItemNameTablePath();
// The item names (generated), reloaded with the map data -- defined with
// the map-data tables below. For the live loop; commands load their own.
const ItemNameTable& ItemNames();

struct SlotInfo {
  const char* name;
  uintptr_t address;
};
std::vector<SlotInfo> BuildEquipSlotList(const ResolvedProfile& profile);
std::string FormatItemDescription(HANDLE hProcess, uintptr_t equipInventoryData,
                                   const ItemNameTable& nameTable, int32_t id);

struct ItemLookup {
  bool ok = false;
  bool plausible = false;  // uniqueId matches a known ItemUniqueIdPrefix
  int32_t uniqueId = 0;
  int32_t giveId = 0;
  uint32_t quantity = 0;
};
ItemLookup ResolveItem(HANDLE hProcess, uintptr_t equipInventoryData, int32_t inventoryItemId);
std::string LookupItemName(const ItemNameTable& table, int32_t uniqueId, int32_t giveId);
std::string LookupSpellName(const ItemNameTable& table, int32_t magicParamId);

// ---- equipped spells (attunement slots) --------------------------------
//
// Slot contents: PlayerGameData+0x470 is a POINTER (not a direct
// offset into PlayerGameData itself) to the actual spell-slot array;
// slot N's MagicParam id is an int32 at (that pointer) + 0x18 +
// (N-1)*8 -- sourced from AmySouls/DS3RuntimeScripting's
// PlayerGameData::getSpell() (player_game_data.cpp), whose
// "accessMultilevelPointer" helper is exactly this: one dereference,
// then the sub-offset applies to the pointed-to struct, not to
// PlayerGameData directly. Missing that initial dereference was a
// real bug caught live (2026-08-19): reading PlayerGameData+0x470+0x18
// directly (no dereference) gave garbage/pointer-looking values and
// the user confirmed all 3 of their slots were actually filled, not
// empty as that wrong version reported. Fixed by dumping the raw
// bytes at PlayerGameData+0x470 (a plausible heap pointer, not junk),
// dereferencing it once, and finding exactly the expected 8-byte-
// strided {int32 magicId, int32 padding/unknown=-1} slot layout --
// slot 1 read 2400000, slot 2 read 2402000, slot 3 read 2411000,
// which resolve via this project's own spell name table to Fireball/
// Fire Orb/Great Combustion, a coherent Pyromancer loadout the user
// confirmed matches what they actually have attuned.
// Slots run 1..14 (the game's own max), but only the first N -- the
// character's actual current Attunement Slot count, see
// ComputeBaseAttunementSlots/ComputeRingAttunementSlotBonus above --
// are meaningful; slots beyond that read stale/leftover ids the game
// doesn't display, so callers should bound the read by the live slot
// count, not read all 14 unconditionally.
constexpr uintptr_t kSpellSlotArrayPtrOffset = 0x470;
constexpr uintptr_t kSpellSlotBaseOffset = 0x18;
constexpr uintptr_t kSpellSlotStride = 8;
// Active (top-slot) spell: an i32 index into the slot array above,
// directly after its 14 entries (0x18 + 14*8 = 0x88). Found 2026-10-06
// with `memdiff pgd 470` while the user cycled spells: the only field
// in EquipMagicData that changed, once per press, in step with them.
// (PlayerGameData itself had been ruled out earlier by a 12KB diff --
// this lives behind its +0x470 pointer, which that diff never followed.)
constexpr uintptr_t kActiveSpellIndexOffset = 0x88;

int32_t ReadActiveSpellIndex(HANDLE hProcess, uintptr_t playerGameDataBase) {
  uintptr_t equipMagicData = 0;
  int32_t index = -1;
  SIZE_T br = 0;
  if (!ReadProcessMemory(hProcess,
                          reinterpret_cast<LPCVOID>(playerGameDataBase + kSpellSlotArrayPtrOffset),
                          &equipMagicData, sizeof(equipMagicData), &br) ||
      equipMagicData == 0)
    return -1;
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(equipMagicData + kActiveSpellIndexOffset),
                    &index, sizeof(index), &br);
  return index;
}

// slotNumber1Based: 1..14. Returns the raw MagicParam id (0/-1 = empty
// slot, not yet independently confirmed which sentinel DS3 uses --
// the user's 3 filled slots all read positive real ids, an empty slot
// hasn't been directly observed yet).
int32_t ReadSpellSlot(HANDLE hProcess, uintptr_t playerGameDataBase, int32_t slotNumber1Based) {
  uintptr_t arrayBase = 0;
  SIZE_T br = 0;
  if (!ReadProcessMemory(hProcess,
                          reinterpret_cast<LPCVOID>(playerGameDataBase + kSpellSlotArrayPtrOffset),
                          &arrayBase, sizeof(arrayBase), &br) ||
      arrayBase == 0)
    return 0;
  int32_t magicId = 0;
  uintptr_t addr = arrayBase + kSpellSlotBaseOffset +
                    static_cast<uintptr_t>(slotNumber1Based - 1) * kSpellSlotStride;
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), &magicId, sizeof(magicId), &br);
  return magicId;
}

// ---- current equip load (COMPUTED from item weights) -----------------
//
// Replaces the old findptr-derived read (DarkSoulsIII.exe+0x47F3B98 ->
// +0x1E7C), which read garbage in a later game session: that anchor
// doesn't survive a relaunch. The game doesn't need a stored total any
// more than it does for poise -- it's the sum of every equipped item's
// param-table `weight`, which this file already reads restart-proof:
//   EquipParamWeapon    +0x0C (weapons, shields, catalysts, ammo)
//   EquipParamProtector +0x20 (armor; live-verified 2026-08-19 against
//                              the wiki for 4 pieces, see armor poise)
//   EquipParamAccessory +0x08 (rings, covenants)
// Weapon and accessory offsets are from soulsmods/Paramdex's DS3 defs.
// Weapon weight is the same at every reinforcement level, so the row is
// the giveId with its +0..+10 digits stripped, like the AR lookup.
constexpr uintptr_t kEquipParamWeaponWeightOffset = 0x0C;
constexpr uintptr_t kEquipParamProtectorWeightOffset = 0x20;
constexpr uintptr_t kEquipParamAccessoryWeightOffset = 0x08;

enum class EquipWeightTable { kWeapon, kProtector, kAccessory };

struct EquippedWeightItem {
  EquipWeightTable table;
  int32_t giveId;
  std::string label;  // slot + name, for the breakdown
};

struct EquipLoadResult {
  bool ok = false;
  double total = 0.0;
  std::string breakdown;  // "R1 Club 6.0, Head Hat 1.6, ..." (nonzero items only)
};

EquipLoadResult ComputeCurrentEquipLoad(HANDLE hProcess, uintptr_t moduleBase,
                                        const std::vector<EquippedWeightItem>& items) {
  EquipLoadResult result;
  auto weaponTable = ResolveParamTable(hProcess, moduleBase, L"EquipParamWeapon");
  auto protectorTable = ResolveParamTable(hProcess, moduleBase, L"EquipParamProtector");
  auto accessoryTable = ResolveParamTable(hProcess, moduleBase, L"EquipParamAccessory");
  if (!weaponTable.ok || !protectorTable.ok || !accessoryTable.ok) return result;

  for (const auto& item : items) {
    const ParamTableRef* table = nullptr;
    int64_t rowId = 0;
    uintptr_t weightOffset = 0;
    switch (item.table) {
      case EquipWeightTable::kWeapon:
        table = &weaponTable;
        rowId = item.giveId - item.giveId % 100;
        weightOffset = kEquipParamWeaponWeightOffset;
        break;
      case EquipWeightTable::kProtector:
        table = &protectorTable;
        rowId = static_cast<int64_t>(item.giveId) - kProtectorGiveIdPrefix;
        weightOffset = kEquipParamProtectorWeightOffset;
        break;
      case EquipWeightTable::kAccessory:
        table = &accessoryTable;
        rowId = static_cast<int64_t>(item.giveId) - kAccessoryGiveIdPrefix;
        weightOffset = kEquipParamAccessoryWeightOffset;
        break;
    }
    uintptr_t rowAddr = FindParamRow(hProcess, *table, rowId);
    if (rowAddr == 0) continue;
    float weight = 0.0f;
    SIZE_T br = 0;
    if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(rowAddr + weightOffset), &weight,
                           sizeof(weight), &br))
      continue;
    result.total += weight;
    if (weight != 0.0f) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "%s%s %.1f", result.breakdown.empty() ? "" : ", ",
                    item.label.c_str(), weight);
      result.breakdown += buf;
    }
  }
  result.ok = true;
  return result;
}

// ---- live mode (Milestone 2) -----------------------------------------
//
// `stats --live` polls until closed instead of for a fixed count, with a
// global pause/resume hotkey (F10). Pausing is the brief's "zero reads,
// not just a hidden window": the single polling thread blocks in
// GetMessage until the hotkey (or Ctrl+C) arrives, so no
// ReadProcessMemory call can happen and it uses no CPU while paused.
//
// Why a low-level keyboard hook rather than RegisterHotKey: the first
// version used RegisterHotKey (Ctrl+Alt+G) and live-testing 2026-10-06
// showed it fires with the game unfocused but NEVER with DS3 focused --
// consistent with the game registering raw input with RIDEV_NOHOTKEYS,
// which suppresses every system hotkey while it's in the foreground. A
// WH_KEYBOARD_LL hook sees keys before that. It runs on its own thread
// that sleeps in GetMessage between key events, so it adds no polling
// and no measurable input lag; every key except the hotkey is passed
// straight through, and the hotkey itself is consumed so the game never
// sees it. F10 was picked by the user as unbound in their DS3 setup
// (Ctrl, the first choice's modifier, is a game control).
constexpr DWORD kLiveHotkeyVk = VK_F10;
constexpr UINT kMsgTogglePause = WM_APP + 1;  // hook thread -> poll thread
constexpr double kPollIntervalMs = 100.0;      // 10Hz, the brief's starting rate
constexpr double kPerfReportIntervalSeconds = 5.0;

// F11: show/hide the overlay's perf figures (brief Phase 2: "on-screen
// perf HUD toggle, separate from the main hotkey"). Only hooked in
// overlay mode -- elsewhere F11 passes through untouched.
constexpr DWORD kPerfHudHotkeyVk = VK_F11;
constexpr UINT kMsgTogglePerfHud = WM_APP + 3;

// F9: cycle the spoiler tier. Window/overlay modes only.
constexpr DWORD kSpoilerTierHotkeyVk = VK_F9;
constexpr UINT kMsgCycleSpoilerTier = WM_APP + 4;
bool g_spoilerHotkeyHeld = false;

DWORD g_pollThreadId = 0;
HWND g_hotkeyTargetWindow = nullptr;  // window/overlay modes: hotkeys are posted here instead
bool g_perfHudHotkeyEnabled = false;  // overlay mode only
volatile LONG g_stopRequested = 0;
bool g_hotkeyHeld = false;         // hook thread only: swallow auto-repeat
bool g_perfHudHotkeyHeld = false;  // same, for F11

LRESULT CALLBACK LiveKeyboardHook(int code, WPARAM wParam, LPARAM lParam) {
  if (code == HC_ACTION) {
    const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    if (key->vkCode == kLiveHotkeyVk) {
      bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;  // F10 arrives as SYSKEY
      if (down && !g_hotkeyHeld) {
        // A window's own queue survives modal loops (dragging, resizing)
        // that drop thread messages, so prefer it when there is one.
        if (g_hotkeyTargetWindow) {
          PostMessageW(g_hotkeyTargetWindow, kMsgTogglePause, 0, 0);
        } else {
          PostThreadMessageW(g_pollThreadId, kMsgTogglePause, 0, 0);
        }
      }
      g_hotkeyHeld = down;
      return 1;  // consumed: the game never sees the hotkey
    }
    if (key->vkCode == kSpoilerTierHotkeyVk && g_hotkeyTargetWindow) {
      bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
      if (down && !g_spoilerHotkeyHeld) PostMessageW(g_hotkeyTargetWindow, kMsgCycleSpoilerTier, 0, 0);
      g_spoilerHotkeyHeld = down;
      return 1;
    }
    if (key->vkCode == kPerfHudHotkeyVk && g_perfHudHotkeyEnabled && g_hotkeyTargetWindow) {
      bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
      if (down && !g_perfHudHotkeyHeld) {
        PostMessageW(g_hotkeyTargetWindow, kMsgTogglePerfHud, 0, 0);
      }
      g_perfHudHotkeyHeld = down;
      return 1;
    }
  }
  return CallNextHookEx(nullptr, code, wParam, lParam);
}

struct HotkeyThreadState {
  HANDLE ready = nullptr;
  bool hooked = false;
  DWORD error = 0;
};

DWORD WINAPI HotkeyThreadMain(LPVOID param) {
  auto* state = static_cast<HotkeyThreadState*>(param);
  HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, LiveKeyboardHook, GetModuleHandleW(nullptr), 0);
  state->hooked = hook != nullptr;
  state->error = hook ? 0 : GetLastError();
  SetEvent(state->ready);
  if (!hook) return 1;
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
  }  // hook callbacks run inside GetMessage; WM_QUIT ends the thread
  UnhookWindowsHookEx(hook);
  return 0;
}

// Ctrl+C / console close: ask the poll loop to stop cleanly (it prints
// a final summary) and wake it if it's blocked in a paused GetMessage.
BOOL WINAPI OnConsoleCtrl(DWORD ctrlType) {
  if (ctrlType != CTRL_C_EVENT && ctrlType != CTRL_BREAK_EVENT && ctrlType != CTRL_CLOSE_EVENT)
    return FALSE;
  InterlockedExchange(&g_stopRequested, 1);
  PostThreadMessageW(g_pollThreadId, WM_QUIT, 0, 0);
  return TRUE;
}

// The brief's "see our own performance metrics": measured poll rate,
// read latency, and this process's own CPU and memory, over a window.
struct PerfWindow {
  LARGE_INTEGER freq{}, start{};
  ULONGLONG cpuStart = 0;
  int ticks = 0;
  double sumMicros = 0.0, maxMicros = 0.0;

  static ULONGLONG ProcessCpu100ns() {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    auto toU64 = [](const FILETIME& ft) {
      return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    return toU64(kernel) + toU64(user);
  }
  void Reset() {
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    cpuStart = ProcessCpu100ns();
    ticks = 0;
    sumMicros = maxMicros = 0.0;
  }
  void AddTick(double micros) {
    ++ticks;
    sumMicros += micros;
    if (micros > maxMicros) maxMicros = micros;
  }
  double ElapsedSeconds() const {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - start.QuadPart) / freq.QuadPart;
  }
  struct Sample {
    bool valid = false;
    int ticks = 0;
    double seconds = 0.0, rateHz = 0.0, avgMicros = 0.0, maxMicros = 0.0;
    double cpuPercentOfCore = 0.0, workingSetMB = 0.0, privateMB = 0.0;
  };
  Sample Take() const {
    Sample out;
    double elapsed = ElapsedSeconds();
    if (elapsed <= 0.0 || ticks == 0) return out;
    double cpuSeconds = static_cast<double>(ProcessCpu100ns() - cpuStart) / 1e7;
    PROCESS_MEMORY_COUNTERS_EX mem{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem),
                         sizeof(mem));
    out.valid = true;
    out.ticks = ticks;
    out.seconds = elapsed;
    out.rateHz = ticks / elapsed;
    out.avgMicros = sumMicros / ticks;
    out.maxMicros = maxMicros;
    out.cpuPercentOfCore = cpuSeconds / elapsed * 100.0;
    out.workingSetMB = mem.WorkingSetSize / 1048576.0;
    out.privateMB = mem.PrivateUsage / 1048576.0;
    return out;
  }
  void Report() const {
    Sample s = Take();
    if (!s.valid) return;
    Log("perf: poll rate %.2f Hz (%d ticks / %.2fs), read latency avg %.1fus max %.1fus, "
        "CPU %.2f%% of one core, working set %.1f MB, private %.1f MB",
        s.rateHz, s.ticks, s.seconds, s.avgMicros, s.maxMicros, s.cpuPercentOfCore,
        s.workingSetMB, s.privateMB);
  }
};

// One perf.csv row (kPerfCsvHeader) from a finished perf window.
std::string PerfCsvRow(const PerfWindow::Sample& s, double sessionSeconds) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  char row[256];
  std::snprintf(row, sizeof(row), "%04u-%02u-%02uT%02u:%02u:%02u,%.1f,%.2f,%d,%.2f,%.1f,%.1f,%.3f,%.1f,%.1f",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, sessionSeconds, s.seconds,
                s.ticks, s.rateHz, s.avgMicros, s.maxMicros, s.cpuPercentOfCore, s.workingSetMB, s.privateMB);
  return row;
}

int RunStats(int iterations, bool live) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;

  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  if (moduleBase == 0) {
    Log("Could not resolve module base/size.");
    CloseHandle(hProcess);
    return 4;
  }

  Log("Resolving player HP/FP/Stamina pointer chain (WorldChrMan AOB scan, re-derived fresh)...");
  auto resolved = ResolvePlayerHp(hProcess, moduleBase, moduleSize);
  if (resolved.structBase == 0) {
    Log("FAILED to resolve the stat struct -- a pattern scan or a dereference along the chain "
        "came up empty. See docs/TECHNICAL.md, or re-derive with `wcm`.");
    CloseHandle(hProcess);
    return 6;
  }
  Log("Resolved struct base 0x%llx -- HP+0x%llx FP+0x%llx Stamina+0x%llx",
      static_cast<unsigned long long>(resolved.structBase),
      static_cast<unsigned long long>(kHpFieldOffset),
      static_cast<unsigned long long>(kFpFieldOffset),
      static_cast<unsigned long long>(kStaminaFieldOffset));

  Log("Resolving level/souls pointer chain (BaseA AOB scan, re-derived fresh)...");
  auto profile = ResolvePlayerProfile(hProcess, moduleBase, moduleSize);
  bool haveProfile = profile.characterStatsBase != 0;
  if (haveProfile) {
    Log("Resolved CharacterStats base 0x%llx -- Level+0x%llx Souls+0x%llx",
        static_cast<unsigned long long>(profile.characterStatsBase),
        static_cast<unsigned long long>(kLevelOffset - kCharacterStatsOffset),
        static_cast<unsigned long long>(kSoulsOffset - kCharacterStatsOffset));
  } else {
    Log("FAILED to resolve level/souls -- BaseA pattern scan or a dereference came up empty. "
        "Continuing with HP/FP/Stamina only.");
  }

  // Current equip load is computed from item weights now (see
  // ComputeCurrentEquipLoad); the old display-stats pointer
  // (ResolveDisplayStats) read garbage after a game relaunch. Item
  // Discovery came from that same pointer and has no replacement yet.
  Log("Item Discovery: not available -- its pointer anchor doesn't survive a game relaunch "
      "(see docs/TECHNICAL.md).");

  // Attributes and base-max only change on level-up. Rather than being
  // read once and going stale, they're wrapped in lambdas so the
  // polling loop below can re-invoke them the moment Level changes
  // mid-run, not just at startup.
  auto readAndPrintAttributes = [&]() {
    int32_t vig = 0, att = 0, end = 0, str = 0, dex = 0, intel = 0, fth = 0, lck = 0, vit = 0;
    SIZE_T brA = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.vigorAddress), &vig,
                       sizeof(vig), &brA);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.attunementAddress), &att,
                       sizeof(att), &brA);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.enduranceAddress), &end,
                       sizeof(end), &brA);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.strengthAddress), &str,
                       sizeof(str), &brA);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.dexterityAddress), &dex,
                       sizeof(dex), &brA);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.intelligenceAddress), &intel,
                       sizeof(intel), &brA);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.faithAddress), &fth,
                       sizeof(fth), &brA);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.luckAddress), &lck, sizeof(lck),
                       &brA);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.vitalityAddress), &vit,
                       sizeof(vit), &brA);
    Log("Attributes: Vig=%d Att=%d End=%d Str=%d Dex=%d Int=%d Fth=%d Lck=%d Vit=%d "
        "-- cross-check against your character status screen.",
        vig, att, end, str, dex, intel, fth, lck, vit);
  };

  auto readAndPrintBaseMax = [&]() {
    int32_t baseMaxHp = 0, baseMaxFp = 0, baseMaxStamina = 0;
    SIZE_T br0 = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.baseMaxHpAddress), &baseMaxHp,
                       sizeof(baseMaxHp), &br0);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.baseMaxFpAddress), &baseMaxFp,
                       sizeof(baseMaxFp), &br0);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.baseMaxStaminaAddress),
                       &baseMaxStamina, sizeof(baseMaxStamina), &br0);
    Log("Base max (no buffs): HP=%d FP=%d Stamina=%d -- cross-check against your character "
        "status screen.",
        baseMaxHp, baseMaxFp, baseMaxStamina);
  };

  // Equipped items (weapons/armor/rings), read via the disassembly-
  // verified chain (see docs/TECHNICAL.md). Unlike attributes/base-max above,
  // these ARE re-checked every tick below (cheap -- 14 extra int32
  // reads against a 100ms interval), since a gear swap can happen at
  // any time, not just on level-up. lastSlotIds is the baseline the
  // loop diffs against.
  uintptr_t equipInventoryData = 0;
  std::vector<SlotInfo> slots;
  ItemNameTable nameTable;
  std::vector<int32_t> lastSlotIds;
  std::vector<int32_t> lastSpellIds(14, 0);  // baseline for per-tick spell-slot diffing

  // Reads the player's current Attunement + equipped rings and returns
  // the live active-slot count (base from Attunement + ring bonuses --
  // see ComputeBaseAttunementSlots/ComputeRingAttunementSlotBonus
  // above). Small, self-contained ring-collection loop rather than
  // sharing state with readAndPrintEquipLoadAndPoise below, matching
  // this file's existing pattern of cheap, independent live re-reads.
  auto currentActiveSpellSlotCount = [&]() -> int32_t {
    int32_t attunement = 0;
    SIZE_T brA = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.attunementAddress), &attunement,
                       sizeof(attunement), &brA);
    std::vector<int32_t> ringGiveIds;
    for (const auto& slot : slots) {
      std::string sName = slot.name;
      if (sName != "Ring1" && sName != "Ring2" && sName != "Ring3" && sName != "Ring4") continue;
      int32_t id = -1;
      SIZE_T brN = 0;
      ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(slot.address), &id, sizeof(id), &brN);
      if (id < 0) continue;
      auto item = ResolveItem(hProcess, equipInventoryData, id);
      if (item.ok && item.plausible) ringGiveIds.push_back(item.giveId);
    }
    return ComputeBaseAttunementSlots(attunement) +
           ComputeRingAttunementSlotBonus(hProcess, moduleBase, ringGiveIds);
  };

  // Prints every currently-equipped spell (only the slots the character
  // actually has unlocked -- see currentActiveSpellSlotCount above) and
  // resets lastSpellIds to match, so the per-tick diff loop below has a
  // clean baseline. Live-verified exact match (2026-08-19): all 3 of
  // the user's equipped spells (Fireball/Fire Orb/Great Combustion)
  // read correctly and were confirmed by the user -- see
  // ReadSpellSlot's comment above for the pointer-dereference bug this
  // caught along the way.
  auto printEquippedSpells = [&]() {
    int32_t activeSlots = currentActiveSpellSlotCount();
    Log("Equipped spells (%d active attunement slot%s, live-verified exact match 2026-08-19 -- "
        "see docs/TECHNICAL.md):",
        activeSlots, activeSlots == 1 ? "" : "s");
    for (int32_t i = 1; i <= 14; ++i) {
      int32_t magicId = ReadSpellSlot(hProcess, profile.xBase, i);
      lastSpellIds[i - 1] = magicId;
      if (i > activeSlots) continue;
      if (magicId <= 0) {
        Log("  Slot %d: (empty)", i);
      } else {
        std::string name = LookupSpellName(nameTable, magicId);
        Log("  Slot %d: magicParamId=%d %s", i, magicId,
            name.empty() ? "(name not found)" : name.c_str());
      }
    }
  };

  // ---- active R/L hand + bottom (quick item) slot -----------------
  //
  // See kRightHandSlotOffset/kLeftHandSlotOffset/
  // kSelectedQuickItemSlotOffset above for how these were found/
  // verified. Top slot (active spell) is a known, documented gap --
  // ruled out of PlayerGameData entirely, not just unfound, see the
  // comment above kSelectedQuickItemSlotOffset.
  int32_t lastRightHandSlot = -999, lastLeftHandSlot = -999, lastQuickItemSlot = -999;
  int32_t lastActiveSpellIndex = -999;
  auto resolveActiveSpellName = [&](int32_t idx) -> std::string {
    if (idx < 0 || idx >= 14) return "(no spell slot)";
    int32_t magicId = ReadSpellSlot(hProcess, profile.xBase, idx + 1);
    std::string name = magicId > 0 ? LookupSpellName(nameTable, magicId) : "";
    return "slot " + std::to_string(idx + 1) + " " +
           (magicId <= 0 ? "(empty)" : (name.empty() ? "(name not found)" : name));
  };

  // Maps a hand-slot index (0-2) to its SlotInfo in `slots` and
  // resolves the item there the same way as everywhere else in this
  // file.
  auto resolveHandSlotName = [&](bool isRight, int32_t slotIdx) -> std::string {
    if (slotIdx < 0 || slotIdx > 2) return "(unknown slot index)";
    std::string wantedName = (isRight ? std::string("R") : std::string("L")) +
                              std::to_string(slotIdx + 1);
    for (const auto& slot : slots) {
      if (wantedName != slot.name) continue;
      int32_t id = -1;
      SIZE_T br = 0;
      ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(slot.address), &id, sizeof(id), &br);
      if (id < 0) return wantedName + " (empty)";
      auto item = ResolveItem(hProcess, equipInventoryData, id);
      if (!item.ok || !item.plausible) return wantedName + " (unresolved)";
      std::string name = LookupItemName(nameTable, item.uniqueId, item.giveId);
      return wantedName + " " + (name.empty() ? "(name not found)" : name);
    }
    return "(slot not found)";
  };

  auto resolveQuickItemName = [&](int32_t idx) -> std::string {
    if (idx < 0 || idx > 9) return "(unknown slot index)";
    int32_t id = -1;
    SIZE_T br = 0;
    ReadProcessMemory(hProcess,
                       reinterpret_cast<LPCVOID>(profile.xBase + kQuickItemArrayOffset +
                                                  static_cast<uintptr_t>(idx) * kQuickItemStride),
                       &id, sizeof(id), &br);
    if (id <= 0) return "(empty)";
    auto item = ResolveItem(hProcess, equipInventoryData, id);
    if (!item.ok || !item.plausible) return "(unresolved)";
    std::string name = LookupItemName(nameTable, item.uniqueId, item.giveId);
    return name.empty() ? "(name not found)" : name;
  };

  auto printActiveSlots = [&]() {
    int32_t rHand = 0, lHand = 0, qItem = 0;
    SIZE_T br = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.xBase + kRightHandSlotOffset),
                       &rHand, sizeof(rHand), &br);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.xBase + kLeftHandSlotOffset),
                       &lHand, sizeof(lHand), &br);
    ReadProcessMemory(hProcess,
                       reinterpret_cast<LPCVOID>(profile.xBase + kSelectedQuickItemSlotOffset),
                       &qItem, sizeof(qItem), &br);
    lastRightHandSlot = rHand;
    lastLeftHandSlot = lHand;
    lastQuickItemSlot = qItem;
    Log("Active right hand: %s -- live-verified correct 2026-08-19", resolveHandSlotName(true, rHand).c_str());
    Log("Active left hand: %s -- live-verified correct 2026-08-19", resolveHandSlotName(false, lHand).c_str());
    Log("Active bottom slot (quick item #%d): %s -- live-verified correct 2026-08-19, see "
        "docs/TECHNICAL.md",
        qItem + 1, resolveQuickItemName(qItem).c_str());
    lastActiveSpellIndex = ReadActiveSpellIndex(hProcess, profile.xBase);
    Log("Active top slot (spell): %s", resolveActiveSpellName(lastActiveSpellIndex).c_str());
  };

  // Physical Attack Rating for the 6 weapon slots -- see
  // ComputeWeaponPhysicalAr above for the full derivation/caveats.
  // Printed alongside a slot's item description, both at startup and
  // whenever that slot changes. Reads Str/Dex fresh every call (cheap)
  // so a level-up between gear checks isn't stale.
  auto isWeaponSlot = [](const char* slotName) {
    return std::string(slotName) == "R1" || std::string(slotName) == "R2" ||
           std::string(slotName) == "R3" || std::string(slotName) == "L1" ||
           std::string(slotName) == "L2" || std::string(slotName) == "L3";
  };
  auto printWeaponArIfApplicable = [&](const char* slotName, int32_t inventoryItemId) {
    if (!isWeaponSlot(slotName) || inventoryItemId < 0) return;
    auto item = ResolveItem(hProcess, equipInventoryData, inventoryItemId);
    if (!item.ok || !item.plausible) return;
    int32_t str = 0, dex = 0, intel = 0, faith = 0, luck = 0;
    SIZE_T brS = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.strengthAddress), &str,
                       sizeof(str), &brS);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.dexterityAddress), &dex,
                       sizeof(dex), &brS);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.intelligenceAddress), &intel,
                       sizeof(intel), &brS);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.faithAddress), &faith,
                       sizeof(faith), &brS);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.luckAddress), &luck,
                       sizeof(luck), &brS);
    auto ar = ComputeWeaponAr(hProcess, moduleBase, item.giveId, str, dex, intel, faith, luck);
    if (!ar.ok) return;

    // Live two-handing state -- see kWeaponSheathStateOffset above.
    // Only the currently-DRAWN slot on a given side can be two-handed
    // (RightHandSlot/LeftHandSlot pick which of that side's 3 weapon
    // slots is active); a slot that isn't the active one on its side is
    // always shown one-handed, since it isn't the weapon actually being
    // gripped right now regardless of the global sheath state.
    int32_t sheathState = 0, rightHandSlot = 0, leftHandSlot = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.xBase + kWeaponSheathStateOffset),
                       &sheathState, sizeof(sheathState), &brS);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.xBase + kRightHandSlotOffset),
                       &rightHandSlot, sizeof(rightHandSlot), &brS);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.xBase + kLeftHandSlotOffset),
                       &leftHandSlot, sizeof(leftHandSlot), &brS);
    std::string slot = slotName;
    bool isRightSlot = slot == "R1" || slot == "R2" || slot == "R3";
    bool isLeftSlot = slot == "L1" || slot == "L2" || slot == "L3";
    int32_t slotIndex = slot.empty() ? -1 : (slot[1] - '1');  // R1/L1->0, R2/L2->1, R3/L3->2
    bool isActiveSlot = (isRightSlot && rightHandSlot == slotIndex) ||
                         (isLeftSlot && leftHandSlot == slotIndex);
    bool isTwoHanded = isActiveSlot && ((isRightSlot && sheathState == 3) ||
                                         (isLeftSlot && sheathState == 2));

    std::string line = "    AR (computed, Str=" + std::to_string(str) +
                        " Dex=" + std::to_string(dex) + " Int=" + std::to_string(intel) +
                        " Fth=" + std::to_string(faith) + (isTwoHanded ? ", two-handed" : "") +
                        "):";
    line += FormatWeaponAr(ar, isTwoHanded);
    Log("%s -- see docs/TECHNICAL.md \"Weapon Attack Rating\" for what's live-verified.", line.c_str());
  };

  if (haveProfile) {
    equipInventoryData = profile.xBase + kEquipGameDataOffset + kEquipInventoryDataOffset;
    slots = BuildEquipSlotList(profile);
    nameTable = LoadItemNameTable(FindItemNameTablePath());

    readAndPrintAttributes();

    Log("Equipped items -- re-resolved automatically below whenever a slot changes:%s",
        nameTable.loaded ? "" : " (no item names yet -- run  wasd-cli.exe extract;  showing raw IDs only)");
    lastSlotIds.assign(slots.size(), -1);
    for (size_t i = 0; i < slots.size(); ++i) {
      int32_t id = -1;
      SIZE_T br = 0;
      ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(slots[i].address), &id, sizeof(id),
                         &br);
      lastSlotIds[i] = id;
      Log("  %-6s %s", slots[i].name,
          FormatItemDescription(hProcess, equipInventoryData, nameTable, id).c_str());
      printWeaponArIfApplicable(slots[i].name, id);
    }
    printEquippedSpells();
    printActiveSlots();
  }

  readAndPrintBaseMax();

  // Max equip load + armor poise both depend on the currently equipped
  // items, so both are wrapped in a lambda the same way
  // readAndPrintAttributes/readAndPrintBaseMax are -- re-invoked below
  // any time an equip slot actually changes, not just at startup.
  // Max equip load also depends on Vitality (re-read here every call,
  // cheap) so a Vitality level-up refreshes it too even without a gear
  // change.
  double currentEquipLoad = 0.0;  // computed; refreshed on gear change / level-up
  bool haveCurrentEquipLoad = false;
  auto readAndPrintEquipLoadAndPoise = [&]() {
    int32_t vitality = 0;
    SIZE_T brV = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.vitalityAddress), &vitality,
                       sizeof(vitality), &brV);
    double baseMaxLoad = ComputeBaseMaxEquipLoad(vitality);

    std::vector<std::string> equippedNames;
    std::vector<int32_t> armorGiveIds;
    std::vector<int32_t> ringGiveIds;
    std::vector<EquippedWeightItem> weightItems;
    for (const auto& slot : slots) {
      int32_t id = -1;
      SIZE_T brN = 0;
      ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(slot.address), &id, sizeof(id), &brN);
      if (id < 0) continue;
      auto item = ResolveItem(hProcess, equipInventoryData, id);
      if (item.ok && item.plausible) {
        std::string name = LookupItemName(nameTable, item.uniqueId, item.giveId);
        std::string slotName = slot.name;
        std::string label = slotName + " " + (name.empty() ? std::to_string(item.giveId) : name);
        if (slotName == "Head" || slotName == "Chest" || slotName == "Hands" ||
            slotName == "Legs") {
          armorGiveIds.push_back(item.giveId);
          weightItems.push_back({EquipWeightTable::kProtector, item.giveId, label});
        } else if (slotName == "Ring1" || slotName == "Ring2" || slotName == "Ring3" ||
                   slotName == "Ring4") {
          ringGiveIds.push_back(item.giveId);
          weightItems.push_back({EquipWeightTable::kAccessory, item.giveId, label});
        } else if (slotName == "Covenant") {
          weightItems.push_back({EquipWeightTable::kAccessory, item.giveId, label});
        } else {  // R1-3, L1-3, Arrow/Bolt slots
          weightItems.push_back({EquipWeightTable::kWeapon, item.giveId, label});
        }
        if (!name.empty()) equippedNames.push_back(std::move(name));
      }
    }
    // Ring bonus (Havel's Ring/Ring of Favor): computed live via
    // SpEffectParam, same mechanism as ring poise bonus below -- see
    // ComputeRingEquipLoadRateMultiplier above (UNVERIFIED against an
    // actual load-boosting ring, none on hand this session).
    double ringLoadMultiplier = ComputeRingEquipLoadRateMultiplier(hProcess, moduleBase, ringGiveIds);
    double finalMaxLoad = baseMaxLoad * ringLoadMultiplier;
    auto load = ComputeCurrentEquipLoad(hProcess, moduleBase, weightItems);
    haveCurrentEquipLoad = load.ok;
    if (load.ok) {
      currentEquipLoad = load.total;
      Log("Current equip load (computed from item weights): %.1f / %.1f = %.1f%% -- %s",
          load.total, finalMaxLoad, finalMaxLoad > 0 ? load.total / finalMaxLoad * 100.0 : 0.0,
          load.breakdown.c_str());
    }
    std::string ringWarning = EquipLoadRingBonusWarning(equippedNames);
    if (ringLoadMultiplier != 1.0) {
      Log("Max equip load: %.1f (base %.1f from Vitality=%d x ring multiplier %.3f -- ring "
          "formula UNVERIFIED, no load ring tested yet, see docs/TECHNICAL.md)",
          finalMaxLoad, baseMaxLoad, vitality, ringLoadMultiplier);
    } else if (!ringWarning.empty()) {
      Log("Max equip load (computed from Vitality=%d, BASE ONLY): %.1f -- %s", vitality,
          baseMaxLoad, ringWarning.c_str());
    } else {
      Log("Max equip load (computed from Vitality=%d, formula from Fextralife's Equipment_Load "
          "page): %.1f",
          vitality, baseMaxLoad);
    }

    // Attunement slots: computed from Attunement via the documented
    // breakpoint table (see ComputeBaseAttunementSlots above), plus a
    // live SpEffectParam read for ring bonuses (see
    // ComputeRingAttunementSlotBonus above -- generic, not name-based;
    // works for any attunement-slot ring, not just the one on hand).
    // Live-verified (2026-08-19): Attunement=14 -> base 2, +1 from
    // Saint's Ring (the ring actually granting the bonus, corrected
    // live after an initial wrong guess at Sage Ring) = 3, exactly
    // matching the user's own stated breakdown.
    int32_t attunement = 0;
    SIZE_T brAtt = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.attunementAddress), &attunement,
                       sizeof(attunement), &brAtt);
    int32_t baseSlots = ComputeBaseAttunementSlots(attunement);
    int32_t ringSlots = ComputeRingAttunementSlotBonus(hProcess, moduleBase, ringGiveIds);
    Log("Attunement Slots (computed from Attunement=%d via Fextralife's breakpoint table, ring "
        "bonus read live via SpEffectParam): %d (base %d + %d from rings)",
        attunement, baseSlots + ringSlots, baseSlots, ringSlots);

    // Armor poise: read live from EquipParamProtector, not scanned or
    // hardcoded per-item (see ComputeArmorPoise above and docs/TECHNICAL.md).
    // Ring poise bonuses (e.g. Wolf Ring) layer on top via SpEffectParam
    // -- see ComputeRingPoiseRateMultiplier above for the offset
    // evidence and the still-UNVERIFIED-against-an-actual-poise-ring
    // caveat.
    auto poiseResult = ComputeArmorPoise(hProcess, moduleBase, armorGiveIds);
    if (poiseResult.ok) {
      double ringMultiplier = ComputeRingPoiseRateMultiplier(hProcess, moduleBase, ringGiveIds);
      if (ringMultiplier != 1.0) {
        double finalPoise = poiseResult.total * ringMultiplier;
        Log("Total Poise: %.2f (armor %.2f x ring multiplier %.3f -- ring formula UNVERIFIED, "
            "no poise ring tested yet, see docs/TECHNICAL.md)",
            finalPoise, poiseResult.total, ringMultiplier);
      } else {
        Log("Total Poise (armor only, computed live from EquipParamProtector, diminishing-returns "
            "formula): %.2f -- cross-check against a poise calculator, vanilla UI doesn't show "
            "this number. No ring poise bonus detected (or none equipped) -- see docs/TECHNICAL.md.",
            poiseResult.total);
      }
    } else {
      Log("Armor poise NOT computed (EquipParamProtector table resolution failed).");
    }

    // Defense/Absorption (all 8 damage types): see ComputeArmorDefense
    // above -- same table, same live-verified diminishing-returns
    // combine as poise, applied to a different field cluster. Formula
    // itself is UNVERIFIED against the live status screen (first use
    // of these 8 fields), flagged in the log line below.
    auto defenseResult = ComputeArmorDefense(hProcess, moduleBase, armorGiveIds);
    if (defenseResult.ok) {
      Log("Absorption %% (armor only, computed live from EquipParamProtector, same "
          "diminishing-returns formula as Poise -- live-verified exact match to 3 decimal "
          "places against the in-game Stats screen, 2026-08-19, see docs/TECHNICAL.md): Phys=%.3f "
          "Strike=%.3f Slash=%.3f Thrust=%.3f Magic=%.3f Fire=%.3f Lightning=%.3f Dark=%.3f",
          defenseResult.phys, defenseResult.strike, defenseResult.slash, defenseResult.thrust,
          defenseResult.magic, defenseResult.fire, defenseResult.lightning, defenseResult.dark);
    } else {
      Log("Armor defense/absorption NOT computed (EquipParamProtector table resolution failed).");
    }

    // Status resistances (Bleed/Poison/Frost/Curse, +Toxic): see
    // ComputeArmorResistance above -- plain point sum, NOT the
    // diminishing-returns formula used for Poise/Absorption. UNVERIFIED
    // against the live status screen, flagged in the log line below.
    auto resistResult = ComputeArmorResistance(hProcess, moduleBase, armorGiveIds);
    if (resistResult.ok) {
      Log("Resistance (armor only, computed live from EquipParamProtector, plain sum -- "
          "Bleed/Poison/Frost/Curse live-verified correct against the in-game screen "
          "2026-08-19; Toxic not shown in vanilla UI, unverified, see docs/TECHNICAL.md): Bleed=%.0f "
          "Poison=%.0f Toxic=%.0f Frost=%.0f Curse=%.0f",
          resistResult.bleed, resistResult.poison, resistResult.toxic, resistResult.frost,
          resistResult.curse);
    } else {
      Log("Armor resistance NOT computed (EquipParamProtector table resolution failed).");
    }
  };

  if (haveProfile) {
    readAndPrintEquipLoadAndPoise();
  }

  HANDLE hotkeyThread = nullptr;
  DWORD hotkeyThreadId = 0;
  if (live) {
    g_pollThreadId = GetCurrentThreadId();
    MSG queueInit;  // make sure this thread has a queue before the hook can post to it
    PeekMessageW(&queueInit, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    HotkeyThreadState hotkeyState;
    hotkeyState.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    hotkeyThread = CreateThread(nullptr, 0, HotkeyThreadMain, &hotkeyState, 0, &hotkeyThreadId);
    if (hotkeyThread) WaitForSingleObject(hotkeyState.ready, INFINITE);
    CloseHandle(hotkeyState.ready);
    if (!hotkeyThread || !hotkeyState.hooked) {
      Log("WARNING: couldn't install the F10 keyboard hook (error %lu). Running without "
          "pause/resume; Ctrl+C still stops cleanly.",
          hotkeyThread ? hotkeyState.error : GetLastError());
    }
    Log("LIVE: polling at 10Hz until closed. F10 pauses/resumes, even with the game focused "
        "(zero reads while paused; the key is consumed, the game never sees it). Ctrl+C "
        "stops. The status line prints only when a value changes; a perf line prints every "
        "%.0fs.",
        kPerfReportIntervalSeconds);
  } else {
    Log("Polling HP/FP/Stamina (current/max) at 10Hz for %d reads (Ctrl+C to stop early)...",
        iterations);
  }

  int32_t lastLevel = 0;
  bool haveLastLevel = false;
  // Cached rather than recomputed every tick -- currentActiveSpellSlotCount
  // resolves rings + walks their SpEffect chains, noticeably heavier than
  // the other per-tick reads. Only recomputed when something that could
  // actually change it happens: a level-up (Attunement) or a gear change
  // (a ring swap). Initialized from printEquippedSpells's own call below.
  int32_t cachedActiveSpellSlots = haveProfile ? currentActiveSpellSlotCount() : 0;

  double sumMicros = 0.0, maxMicros = 0.0;
  int okCount = 0, tickCount = 0;
  std::string lastStatus;  // live mode: status line printed only when this changes
  bool playerUnloaded = false;  // quit to menu / loading: see ReadCurrentPlayerIns
  const uintptr_t gameManCellOffset = ResolveGameManCellOffset(hProcess, moduleBase, moduleSize);
  const AreaNames areaNames;
  AreaTracker areaTracker;
  const AreaInfo areaInfo;
  SessionTracker tracker;
  tracker.log.enabled = live;  // only live runs are saved
  SessionStats& session = tracker.stats;
  LARGE_INTEGER lastSessionTick{};
  bool loggedCounters = false;
  PerfWindow perf;
  perf.Reset();
  LARGE_INTEGER qpcFreq, nextTick;
  QueryPerformanceFrequency(&qpcFreq);
  QueryPerformanceCounter(&nextTick);
  const LONGLONG tickInterval =
      static_cast<LONGLONG>(qpcFreq.QuadPart * kPollIntervalMs / 1000.0);
  double pausedSeconds = 0.0;

  for (int i = 0; live || i < iterations; ++i) {
    if (live) {
      // Hotkey / quit handling. Everything here is message-queue work,
      // not a memory read.
      bool togglePause = false;
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == kMsgTogglePause) togglePause = true;
        if (msg.message == WM_QUIT) InterlockedExchange(&g_stopRequested, 1);
      }
      if (g_stopRequested) break;
      if (togglePause) {
        perf.Report();
        if (auto ps = perf.Take(); ps.valid) tracker.log.WritePerf(PerfCsvRow(ps, tracker.stats.seconds));
        Log("PAUSED (F10) -- polling stopped, zero reads until resumed.");
        LARGE_INTEGER pauseStart, pauseEnd;
        QueryPerformanceCounter(&pauseStart);
        bool resumed = false;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {  // blocks: no CPU, no reads
          if (msg.message == kMsgTogglePause) {
            resumed = true;
            break;
          }
        }
        QueryPerformanceCounter(&pauseEnd);
        double pausedFor =
            static_cast<double>(pauseEnd.QuadPart - pauseStart.QuadPart) / qpcFreq.QuadPart;
        pausedSeconds += pausedFor;
        if (!resumed) break;  // WM_QUIT (Ctrl+C) while paused
        Log("RESUMED after %.1fs paused.", pausedFor);
        perf.Reset();
        lastStatus.clear();  // reprint the current state straight away
        nextTick = pauseEnd;
      }
      // Game closed? A process-handle query, not a memory read.
      DWORD exitCode = 0;
      if (GetExitCodeProcess(hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
        Log("Game process exited (code %lu) -- stopping.", exitCode);
        break;
      }
    }
    ++tickCount;

    // Player unloaded or rebuilt (quit to menu, reload)? See
    // ReadCurrentPlayerIns. While unloaded, no player reads happen.
    {
      uintptr_t playerIns =
          ReadCurrentPlayerIns(hProcess, moduleBase, resolved.worldChrManCellOffset);
      if (playerIns != resolved.entityAddress) {
        bool reloaded = false;
        if (playerIns != 0) {
          auto fresh = ResolvePlayerHp(hProcess, moduleBase, moduleSize);
          if (fresh.structBase != 0 &&
              ReadCurrentPlayerIns(hProcess, moduleBase, fresh.worldChrManCellOffset) ==
                  fresh.entityAddress) {
            resolved = fresh;
            profile = ResolvePlayerProfile(hProcess, moduleBase, moduleSize);
            haveProfile = profile.characterStatsBase != 0;
            if (haveProfile) {
              equipInventoryData =
                  profile.xBase + kEquipGameDataOffset + kEquipInventoryDataOffset;
              slots = BuildEquipSlotList(profile);
              lastSlotIds.assign(slots.size(), INT32_MIN);  // every slot re-reports below
            }
            lastStatus.clear();
            playerUnloaded = false;
            reloaded = true;
            Log("Player loaded -- all addresses re-resolved.");
          }
        } else if (!playerUnloaded) {
          playerUnloaded = true;
          Log("Player unloaded (main menu or loading) -- reads paused until a character is "
              "loaded.");
        }
        if (!reloaded) {
          // Still unloaded or mid-load: wait a tick (re-resolving runs AOB
          // scans, so retry at most once a second) without reading.
          DWORD waitMs = playerIns != 0 ? 1000 : static_cast<DWORD>(kPollIntervalMs);
          if (live) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, waitMs, QS_ALLPOSTMESSAGE);
          } else {
            Sleep(waitMs);
          }
          QueryPerformanceCounter(&nextTick);
          continue;
        }
      }
    }

    int32_t hp = 0, maxHp = 0, fp = 0, maxFp = 0, stamina = 0, maxStamina = 0;
    int32_t level = 0, souls = 0;
    SIZE_T br = 0;

    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    bool ok =
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.hpAddress), &hp,
                           sizeof(hp), &br) &&
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.maxHpAddress), &maxHp,
                           sizeof(maxHp), &br) &&
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.fpAddress), &fp,
                           sizeof(fp), &br) &&
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.maxFpAddress), &maxFp,
                           sizeof(maxFp), &br) &&
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.staminaAddress), &stamina,
                           sizeof(stamina), &br) &&
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.maxStaminaAddress),
                           &maxStamina, sizeof(maxStamina), &br);
    if (haveProfile) {
      ok = ok &&
           ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.levelAddress), &level,
                              sizeof(level), &br) &&
           ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.soulsAddress), &souls,
                              sizeof(souls), &br);
    }
    QueryPerformanceCounter(&end);
    double micros = (end.QuadPart - start.QuadPart) * 1000000.0 / freq.QuadPart;
    sumMicros += micros;
    if (micros > maxMicros) maxMicros = micros;
    perf.AddTick(micros);

    if (ok) {
      ++okCount;
      if (haveProfile) {
        // Level-up detection: re-resolve attributes/base-max the
        // moment they'd actually be stale, not just at startup. Not
        // counted in the latency figure below -- that's meant to
        // reflect steady-state polling cost, and a level-up is a rare
        // event, not the common case being measured.
        if (!haveLastLevel) {
          lastLevel = level;
          haveLastLevel = true;
        } else if (level != lastLevel) {
          Log("Level changed %d -> %d -- refreshing attributes and base-max...", lastLevel,
              level);
          readAndPrintAttributes();
          readAndPrintBaseMax();
          readAndPrintEquipLoadAndPoise();  // max equip load depends on Vitality
          cachedActiveSpellSlots = currentActiveSpellSlotCount();  // Attunement may have changed
          // AR depends on Str/Dex/Int/Fth, so every weapon's figure is
          // stale after a level-up even though no slot changed.
          for (size_t si = 0; si < slots.size(); ++si) {
            if (!isWeaponSlot(slots[si].name) || lastSlotIds[si] < 0) continue;
            Log("  %s AR after level-up:", slots[si].name);
            printWeaponArIfApplicable(slots[si].name, lastSlotIds[si]);
          }
          lastLevel = level;
        }

        // Gear-change detection, all 14 slots (weapons/armor/rings),
        // every tick -- a swap can happen at any time, unlike
        // attributes which only change on level-up. Any change also
        // triggers a max-equip-load/armor-poise recompute (once per
        // tick, even if multiple slots changed at once), since both
        // depend on currently equipped items and would otherwise go
        // stale the moment gear is swapped mid-run.
        bool anySlotChanged = false;
        for (size_t si = 0; si < slots.size(); ++si) {
          int32_t curId = -1;
          SIZE_T brS = 0;
          ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(slots[si].address), &curId,
                             sizeof(curId), &brS);
          if (curId != lastSlotIds[si]) {
            Log("%s slot changed: %s", slots[si].name,
                FormatItemDescription(hProcess, equipInventoryData, nameTable, curId).c_str());
            printWeaponArIfApplicable(slots[si].name, curId);
            lastSlotIds[si] = curId;
            anySlotChanged = true;
          }
        }
        if (anySlotChanged) {
          readAndPrintEquipLoadAndPoise();
          cachedActiveSpellSlots = currentActiveSpellSlotCount();  // a ring swap may have changed it
        }

        // Spell-slot change detection -- spells can be re-attuned at a
        // bonfire without touching gear at all, so this needs its own
        // trigger, not just piggybacking on anySlotChanged above. Uses
        // the cached active-slot count (refreshed above on level-up/
        // gear-change only, not recomputed every tick -- resolving
        // rings + walking SpEffect chains every 100ms would be needless
        // overhead for a count that only ever changes on those two
        // events). Only the currently-active slots are diffed/printed;
        // slots beyond the live count are silently re-baselined so a
        // later increase in Attunement Slots doesn't spuriously report
        // stale content as a "change".
        for (int32_t spellSlot = 1; spellSlot <= 14; ++spellSlot) {
          int32_t magicId = ReadSpellSlot(hProcess, profile.xBase, spellSlot);
          if (magicId == lastSpellIds[spellSlot - 1]) continue;
          if (spellSlot <= cachedActiveSpellSlots) {
            std::string name = LookupSpellName(nameTable, magicId);
            Log("Spell slot %d changed: %s", spellSlot,
                magicId <= 0 ? "(empty)" : (name.empty() ? "(name not found)" : name.c_str()));
          }
          lastSpellIds[spellSlot - 1] = magicId;
        }

        // Active R/L hand + bottom (quick-item) slot change detection --
        // these can change independently of both gear swaps (switching
        // which already-equipped weapon is drawn) and level-ups, so
        // this is its own trigger too. Cheap: 3 int32 reads per tick.
        {
          int32_t rHand = 0, lHand = 0, qItem = 0;
          SIZE_T brActive = 0;
          ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.xBase +
                                                                  kRightHandSlotOffset),
                             &rHand, sizeof(rHand), &brActive);
          ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.xBase +
                                                                  kLeftHandSlotOffset),
                             &lHand, sizeof(lHand), &brActive);
          ReadProcessMemory(
              hProcess,
              reinterpret_cast<LPCVOID>(profile.xBase + kSelectedQuickItemSlotOffset), &qItem,
              sizeof(qItem), &brActive);
          if (rHand != lastRightHandSlot) {
            Log("Active right hand changed: %s", resolveHandSlotName(true, rHand).c_str());
            lastRightHandSlot = rHand;
          }
          if (lHand != lastLeftHandSlot) {
            Log("Active left hand changed: %s", resolveHandSlotName(false, lHand).c_str());
            lastLeftHandSlot = lHand;
          }
          int32_t spellIdx = ReadActiveSpellIndex(hProcess, profile.xBase);
          if (spellIdx != lastActiveSpellIndex) {
            Log("Active top slot changed (spell): %s", resolveActiveSpellName(spellIdx).c_str());
            lastActiveSpellIndex = spellIdx;
          }
          if (qItem != lastQuickItemSlot) {
            Log("Active bottom slot changed (quick item #%d): %s", qItem + 1,
                resolveQuickItemName(qItem).c_str());
            lastQuickItemSlot = qItem;
          }
        }

        // Session stats.
        std::string currentArea;
        {
          PlayerArea area = ReadPlayerArea(hProcess, moduleBase, resolved.entityAddress,
                                           gameManCellOffset);
          areaTracker.Update(area);
          currentArea = areaInfo.AreaOf(areaTracker.lastRegion);
          GameCounters counters = ReadGameCounters(hProcess, moduleBase, profile.baseACellOffset);
          if (counters.ok && !loggedCounters) {
            loggedCounters = true;
            Log("Game counters (UNVERIFIED offsets, see docs/TECHNICAL.md): DeathNum=%d TrueDeathNum=%d "
                "PlayTime=%u", counters.deathNum, counters.trueDeathNum, counters.playTime);
          }
          LARGE_INTEGER nowTick;
          QueryPerformanceCounter(&nowTick);
          double dt = lastSessionTick.QuadPart == 0
                          ? 0.0
                          : (std::min)(1.0, static_cast<double>(nowTick.QuadPart -
                                                                 lastSessionTick.QuadPart) /
                                                freq.QuadPart);
          lastSessionTick = nowTick;
          SessionContext ctx;
          ctx.level = level;
          ctx.souls = souls;
          ctx.deathNum = counters.deathNum;
          ctx.playTimeMs = counters.playTime;
          ctx.area = currentArea;
          ctx.characterName = ReadCharacterName(hProcess, profile.xBase);
          ctx.pose = ReadPlayerPose(hProcess, resolved);
          ctx.lastBonfireId = area.lastBonfireId;
          ctx.lastBonfireName = areaNames.Bonfire(area.lastBonfireId);
          if (counters.ok && tracker.Tick(dt, ctx)) {
            Log("%s (DeathNum=%d TrueDeathNum=%d)", session.lastEvent.c_str(), counters.deathNum,
                counters.trueDeathNum);
          }
        }
        int32_t reqSouls = RequiredSoulsForLevel(level + 1);
        char line[256];
        std::snprintf(line, sizeof(line), "HP=%d/%d FP=%d/%d Stamina=%d/%d", hp, maxHp, fp, maxFp,
                      stamina, maxStamina);
        std::string msg = line;
        if (haveCurrentEquipLoad) {
          std::snprintf(line, sizeof(line), " EquipLoad=%.1f", currentEquipLoad);
          msg += line;
        }
        if (auto pose = ReadPlayerPose(hProcess, resolved); pose.ok) {
          std::snprintf(line, sizeof(line), " Pos=(%.2f, %.2f, %.2f) Facing=%.0fdeg", pose.x,
                        pose.y, pose.z, pose.FacingDegrees());
          msg += line;
        }
        if (auto area = ReadPlayerArea(hProcess, moduleBase, resolved.entityAddress,
                                       gameManCellOffset);
            area.ok) {
          areaTracker.Update(area);
          std::snprintf(line, sizeof(line), " Area=\"%s\" (%s, region %d) LastBonfire=\"%s\"",
                        areaTracker.lastRegion ? areaNames.Region(areaTracker.lastRegion).c_str()
                                               : "?",
                        area.MapName().c_str(), area.playRegionId,
                        areaNames.Bonfire(area.lastBonfireId).c_str());
          msg += line;
        }
        if (session.started) {
          msg += " Deaths=" + std::to_string(session.deaths) + "/" +
                 std::to_string(session.lastDeathNum) + "total";
        }
        std::snprintf(line, sizeof(line), " Level=%d Souls=%d (next lvl %d costs %d, computed)",
                      level, souls, level + 1, reqSouls);
        msg += line;
        if (!live || msg != lastStatus) Log("%s latency=%.3fus", msg.c_str(), micros);
        lastStatus = msg;
      } else {
        char line[256];
        std::snprintf(line, sizeof(line), "HP=%d/%d FP=%d/%d Stamina=%d/%d", hp, maxHp, fp, maxFp,
                      stamina, maxStamina);
        std::string msg = line;
        if (haveCurrentEquipLoad) {
          std::snprintf(line, sizeof(line), " EquipLoad=%.1f", currentEquipLoad);
          msg += line;
        }
        if (auto pose = ReadPlayerPose(hProcess, resolved); pose.ok) {
          std::snprintf(line, sizeof(line), " Pos=(%.2f, %.2f, %.2f) Facing=%.0fdeg", pose.x,
                        pose.y, pose.z, pose.FacingDegrees());
          msg += line;
        }
        if (auto area = ReadPlayerArea(hProcess, moduleBase, resolved.entityAddress,
                                       gameManCellOffset);
            area.ok) {
          areaTracker.Update(area);
          std::snprintf(line, sizeof(line), " Area=\"%s\" (%s, region %d) LastBonfire=\"%s\"",
                        areaTracker.lastRegion ? areaNames.Region(areaTracker.lastRegion).c_str()
                                               : "?",
                        area.MapName().c_str(), area.playRegionId,
                        areaNames.Bonfire(area.lastBonfireId).c_str());
          msg += line;
        }
        if (!live || msg != lastStatus) Log("%s latency=%.3fus", msg.c_str(), micros);
        lastStatus = msg;
      }
    } else {
      Log("read FAILED (error %lu) latency=%.3fus", GetLastError(), micros);
    }

    if (live && perf.ElapsedSeconds() >= kPerfReportIntervalSeconds) {
      perf.Report();
      if (auto ps = perf.Take(); ps.valid) tracker.log.WritePerf(PerfCsvRow(ps, tracker.stats.seconds));
      perf.Reset();
    }

    // Fixed-deadline schedule: each tick is due exactly 100ms after the
    // previous DEADLINE, not 100ms after the work finished, so work time
    // and Sleep's ~15.6ms timer granularity jitter individual ticks but
    // don't drag the average rate below 10Hz. If we fall more than one
    // tick behind (machine stalled), resync rather than burst-catch-up.
    nextTick.QuadPart += tickInterval;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (now.QuadPart - nextTick.QuadPart > tickInterval) nextTick = now;
    LONGLONG waitTicks = nextTick.QuadPart - now.QuadPart;
    DWORD waitMs = waitTicks > 0 ? static_cast<DWORD>(waitTicks * 1000 / qpcFreq.QuadPart) : 0;
    if (live) {
      // Wakes early for a hotkey/quit message so pausing feels instant.
      MsgWaitForMultipleObjects(0, nullptr, FALSE, waitMs, QS_ALLPOSTMESSAGE);
    } else {
      Sleep(waitMs);
    }
  }

  if (hotkeyThread) {
    PostThreadMessageW(hotkeyThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(hotkeyThread, 2000);
    CloseHandle(hotkeyThread);
  }
  CloseHandle(hProcess);
  if (tickCount == 0) tickCount = 1;
  tracker.Finish();
  if (session.started) {
    Log("Session: %s polling, %d death(s), souls gained %lld (%.0f/h), lost %lld, recovered %lld.",
        FormatDuration(session.seconds).c_str(), session.deaths,
        static_cast<long long>(session.gained), session.SoulsPerHour(),
        static_cast<long long>(session.lost), static_cast<long long>(session.recovered));
    for (const auto& [areaName, secs] : session.areaSeconds) {
      Log("  %s in %s", FormatDuration(secs).c_str(), areaName.c_str());
    }
  }
  Log("Done. %d/%d reads ok, avg latency=%.3fus, max=%.3fus%s.", okCount, tickCount,
      sumMicros / tickCount, maxMicros,
      live ? (", " + std::to_string(static_cast<int>(pausedSeconds)) + "s spent paused").c_str()
           : "");
  return 0;
}

// ---- shared hex-dump printer (read-only diagnostic, used by dump/inv) --

void PrintHexDump(const unsigned char* buf, SIZE_T bytesRead) {
  for (SIZE_T i = 0; i < bytesRead; i += 16) {
    std::printf("  +%04llx: ", static_cast<unsigned long long>(i));
    for (SIZE_T j = 0; j < 16; ++j) {
      if (i + j < bytesRead)
        std::printf("%02X ", buf[i + j]);
      else
        std::printf("   ");
    }
    std::printf(" ");
    for (SIZE_T j = 0; j < 16 && i + j < bytesRead; ++j) {
      unsigned char c = buf[i + j];
      std::printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
    }
    std::printf("\n");
  }
}

// Writes a byte-exact binary capture to disk (still just the bytes
// already read via ReadProcessMemory above -- this never touches the
// target process itself). Used to feed captured code into an offline
// disassembler without retyping hex by hand.
bool SaveRawBytes(const std::wstring& path, const unsigned char* buf, SIZE_T length) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(buf), static_cast<std::streamsize>(length));
  return static_cast<bool>(out);
}

// ---- dump mode: raw hex dump of module bytes (read-only diagnostic) --

int RunDump(uintptr_t moduleOffset, SIZE_T length, const std::wstring& savePath) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;

  uintptr_t moduleBase = GetModuleBase(pid, kTargetProcessName);
  if (moduleBase == 0) {
    Log("Could not resolve module base address.");
    CloseHandle(hProcess);
    return 4;
  }

  uintptr_t addr = moduleBase + moduleOffset;
  std::vector<unsigned char> buf(length);
  SIZE_T bytesRead = 0;
  BOOL ok = ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), buf.data(), length,
                               &bytesRead);
  CloseHandle(hProcess);

  if (!ok) {
    Log("Read failed at DarkSoulsIII.exe+0x%llx (0x%llx) (error %lu).",
        static_cast<unsigned long long>(moduleOffset), static_cast<unsigned long long>(addr),
        GetLastError());
    return 5;
  }

  Log("Dumping %zu bytes at DarkSoulsIII.exe+0x%llx (0x%llx):",
      bytesRead, static_cast<unsigned long long>(moduleOffset),
      static_cast<unsigned long long>(addr));
  PrintHexDump(buf.data(), bytesRead);
  if (!savePath.empty()) {
    if (SaveRawBytes(savePath, buf.data(), bytesRead)) {
      std::wprintf(L"Saved %zu raw bytes to %ls\n", bytesRead, savePath.c_str());
    } else {
      Log("Failed to save bytes to the given path.");
    }
  }
  return 0;
}

// ---- dumpmodule: the whole game module to a file (read-only) ---------
//
// For finding our own byte patterns offline:
// tools/find_patterns.py reads the file. Read in 64 KB chunks; a chunk
// that fails is retried page by page, and pages that still can't be read
// (guard or no-access pages) are left as zeros and counted. Nothing is
// printed but a summary; the file is the game's own code, so it belongs
// in a private folder, never in the repository.
int RunDumpModule(const std::wstring& savePath) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  if (moduleBase == 0 || moduleSize == 0) {
    Log("Could not resolve the module base and size.");
    CloseHandle(hProcess);
    return 4;
  }
  constexpr SIZE_T kChunk = 0x10000, kPage = 0x1000;
  std::vector<unsigned char> image(moduleSize, 0);
  SIZE_T unreadable = 0;
  for (SIZE_T off = 0; off < moduleSize; off += kChunk) {
    SIZE_T len = (std::min)(kChunk, moduleSize - off);
    SIZE_T br = 0;
    if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(moduleBase + off), image.data() + off, len, &br) &&
        br == len)
      continue;
    for (SIZE_T p = off; p < off + len; p += kPage) {
      SIZE_T plen = (std::min)(kPage, off + len - p);
      if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(moduleBase + p), image.data() + p, plen, &br) ||
          br != plen) {
        std::fill(image.begin() + p, image.begin() + p + plen, 0);
        unreadable += plen;
      }
    }
  }
  CloseHandle(hProcess);
  if (!SaveRawBytes(savePath, image.data(), image.size())) {
    Log("Failed to save the module to the given path.");
    return 5;
  }
  Log("Saved DarkSoulsIII.exe's module (0x%llx bytes at 0x%llx; 0x%llx unreadable bytes left as zeros) to %ls",
      static_cast<unsigned long long>(moduleSize), static_cast<unsigned long long>(moduleBase),
      static_cast<unsigned long long>(unreadable), savePath.c_str());
  return 0;
}

// ---- peek mode: raw hex dump of an absolute address (read-only) --
//
// Same mechanism as `dump`, but takes an absolute address instead of a
// module-relative offset -- for inspecting scan/rescan hits and other
// heap addresses found live. Not restart-proof by itself (the address
// has to be re-derived each run); it's an investigative tool, not a
// permanent command.

int RunPeek(uintptr_t address, SIZE_T length, const std::wstring& savePath) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;

  std::vector<unsigned char> buf(length);
  SIZE_T bytesRead = 0;
  BOOL ok = ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(address), buf.data(), length,
                               &bytesRead);
  CloseHandle(hProcess);

  if (!ok) {
    Log("Read failed at 0x%llx (error %lu).", static_cast<unsigned long long>(address),
        GetLastError());
    return 5;
  }

  Log("Dumping %zu bytes at 0x%llx:", bytesRead, static_cast<unsigned long long>(address));
  PrintHexDump(buf.data(), bytesRead);
  if (!savePath.empty()) {
    if (SaveRawBytes(savePath, buf.data(), bytesRead)) {
      std::wprintf(L"Saved %zu raw bytes to %ls\n", bytesRead, savePath.c_str());
    } else {
      Log("Failed to save bytes to the given path.");
    }
  }
  return 0;
}

// ---- paramrow mode: hex-dump a live param row by table name + row id --
//
// General-purpose investigative counterpart to `equip`/ComputeArmorPoise/
// ComputeRingSpEffectRateMultiplier's live param-table reads -- lets any
// table/row be inspected directly (e.g. EquipParamWeapon, CalcCorrectGraph,
// AttackElementCorrectParam) without writing a one-off reader for every
// new field being investigated. Same ResolveParamTable/FindParamRow
// machinery, same read-only guarantee.
int RunParamRow(const std::wstring& tableName, int64_t rowId, SIZE_T length,
                 const std::wstring& savePath) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;

  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  (void)moduleSize;
  if (moduleBase == 0) {
    Log("Could not resolve module base/size.");
    CloseHandle(hProcess);
    return 4;
  }

  auto table = ResolveParamTable(hProcess, moduleBase, tableName.c_str());
  if (!table.ok) {
    Log("FAILED to resolve param table '%ls' (name not found in ParamMaster).", tableName.c_str());
    CloseHandle(hProcess);
    return 6;
  }
  Log("Resolved table '%ls': base=0x%llx rowCount=%u", tableName.c_str(),
      static_cast<unsigned long long>(table.tableBase), table.rowCount);

  uintptr_t rowAddr = FindParamRow(hProcess, table, rowId);
  if (rowAddr == 0) {
    Log("Row id %lld not found in '%ls' (%u rows scanned).", static_cast<long long>(rowId),
        tableName.c_str(), table.rowCount);
    CloseHandle(hProcess);
    return 6;
  }

  std::vector<unsigned char> buf(length);
  SIZE_T bytesRead = 0;
  BOOL ok = ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(rowAddr), buf.data(), length,
                               &bytesRead);
  CloseHandle(hProcess);
  if (!ok) {
    Log("Read failed at row 0x%llx (error %lu).", static_cast<unsigned long long>(rowAddr),
        GetLastError());
    return 5;
  }

  Log("Row id %lld -> 0x%llx. Dumping %zu bytes:", static_cast<long long>(rowId),
      static_cast<unsigned long long>(rowAddr), bytesRead);
  PrintHexDump(buf.data(), bytesRead);
  if (!savePath.empty()) {
    if (SaveRawBytes(savePath, buf.data(), bytesRead)) {
      std::wprintf(L"Saved %zu raw bytes to %ls\n", bytesRead, savePath.c_str());
    } else {
      Log("Failed to save bytes to the given path.");
    }
  }
  return 0;
}

// ---- inv mode: investigate the EquipInventoryData raw array layout --
//
// Goal: find the pointer field inside EquipInventoryData that holds the
// InventoryItem[] array, so an equip slot's `inventoryItemId` can be
// turned into {uniqueId, giveId, quantity, unknown1} with a plain
// pointer dereference -- no call into game code, unlike
// DS3RuntimeScripting's GetInventoryItem(). Source for the struct shape
// and the +0x88 count field: AmySouls/DS3RuntimeScripting's
// equip_inventory_data.h/.cpp (see docs/TECHNICAL.md). Read-only throughout:
// VirtualQueryEx/ReadProcessMemory only, same as every other command in
// this file -- this never calls into the game process.
// (kEquipInventoryDataOffset itself now lives up near kEquipGameDataOffset,
// since RunStats needs it too.)
constexpr uintptr_t kInventoryItemCountOffset = 0x88;
constexpr SIZE_T kInventoryItemStructSize = 16;  // {int32 uniqueId, int32 giveId, uint32 quantity, int32 unknown1}
constexpr SIZE_T kInvDumpLength = 0xB0;

int RunInv(bool haveSlotOverride, int32_t slotOverride) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;

  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  if (moduleBase == 0) {
    Log("Could not resolve module base/size.");
    CloseHandle(hProcess);
    return 4;
  }

  auto profile = ResolvePlayerProfile(hProcess, moduleBase, moduleSize);
  if (profile.xBase == 0) {
    Log("FAILED to resolve player base (BaseA scan). See docs/TECHNICAL.md, or re-derive with `wcm`.");
    CloseHandle(hProcess);
    return 6;
  }

  uintptr_t equipGameData = profile.xBase + kEquipGameDataOffset;
  uintptr_t equipInventoryData = equipGameData + kEquipInventoryDataOffset;
  Log("PlayerGameData base (x) = 0x%llx", static_cast<unsigned long long>(profile.xBase));
  Log("EquipGameData = 0x%llx  EquipInventoryData = 0x%llx (+0x%llx)",
      static_cast<unsigned long long>(equipGameData), static_cast<unsigned long long>(equipInventoryData),
      static_cast<unsigned long long>(kEquipInventoryDataOffset));

  SIZE_T br = 0;
  int32_t rawCount = 0;
  if (ReadProcessMemory(hProcess,
                         reinterpret_cast<LPCVOID>(equipInventoryData + kInventoryItemCountOffset),
                         &rawCount, sizeof(rawCount), &br)) {
    Log("Inventory item count (raw+0x88, +1 per DS3RuntimeScripting) = %d", rawCount + 1);
  }

  int32_t rw1 = -1;
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.rWeapon1Address), &rw1, sizeof(rw1),
                     &br);
  int32_t slotId = haveSlotOverride ? slotOverride : rw1;
  Log("Using inventoryItemId=%d (%s) as the index to test candidate array bases.", slotId,
      haveSlotOverride ? "--id override" : "current R1 weapon slot");

  Log("Raw bytes at EquipInventoryData, %zu bytes:", static_cast<size_t>(kInvDumpLength));
  std::vector<unsigned char> buf(kInvDumpLength);
  SIZE_T bytesRead = 0;
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(equipInventoryData), buf.data(),
                          kInvDumpLength, &bytesRead)) {
    Log("Read failed (error %lu).", GetLastError());
    CloseHandle(hProcess);
    return 5;
  }
  PrintHexDump(buf.data(), bytesRead);

  if (slotId < 0) {
    Log("No usable inventoryItemId (R1 slot empty, or pass --id <value>) -- skipping candidate "
        "probing.");
    CloseHandle(hProcess);
    return 0;
  }

  Log("Scanning the dump above for QWORD-aligned fields that look like a heap pointer, and "
      "test-indexing each as `candidate + %d * %zu`:",
      slotId, kInventoryItemStructSize);
  for (SIZE_T i = 0; i + 8 <= bytesRead; i += 8) {
    uint64_t v = 0;
    std::memcpy(&v, buf.data() + i, 8);
    // Cheap filter for "looks like a canonical Windows x64 user-mode
    // pointer" -- nonzero, below the canonical boundary. Every hit
    // below is a candidate to eyeball against the decoded item, not a
    // confirmed array base.
    if (v < 0x10000 || v > 0x00007FFFFFFFFFFFull) continue;

    uintptr_t candidate = static_cast<uintptr_t>(v);
    uintptr_t itemAddr = candidate + static_cast<uintptr_t>(slotId) * kInventoryItemStructSize;
    unsigned char item[kInventoryItemStructSize] = {};
    SIZE_T brI = 0;
    bool ok =
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(itemAddr), item, sizeof(item), &brI);
    if (ok) {
      int32_t uniqueId = 0, giveId = 0, unknown1 = 0;
      uint32_t quantity = 0;
      std::memcpy(&uniqueId, item + 0, 4);
      std::memcpy(&giveId, item + 4, 4);
      std::memcpy(&quantity, item + 8, 4);
      std::memcpy(&unknown1, item + 12, 4);
      Log("  +0x%02llx: ptr=0x%llx -> item@0x%llx = {uniqueId=%d giveId=%d quantity=%u "
          "unknown1=%d}",
          static_cast<unsigned long long>(i), static_cast<unsigned long long>(candidate),
          static_cast<unsigned long long>(itemAddr), uniqueId, giveId, quantity, unknown1);
    } else {
      Log("  +0x%02llx: ptr=0x%llx -> item@0x%llx UNREADABLE (error %lu)",
          static_cast<unsigned long long>(i), static_cast<unsigned long long>(candidate),
          static_cast<unsigned long long>(itemAddr), GetLastError());
    }
  }

  CloseHandle(hProcess);
  return 0;
}

// ---- equip mode: resolve equipped-item identity (read-only) --
//
// Turns each equip slot's `inventoryItemId` (see kEquipSlotBaseOffset
// above) into its real {uniqueId, giveId, quantity} record. An earlier
// attempt at this (single flat array, 8-byte entries, block+0xBF0+id*8)
// matched by pure coincidence for one item and failed for everything
// else -- see git history / docs/TECHNICAL.md for that dead end. This version
// is disassembly-verified, not guessed:
//
// A raw code capture (`dump ... build\captures\getinventoryitem.bin`,
// read-only via ReadProcessMemory, same as every other command in this
// file; the game's own code, so kept privately, not in the repository)
// was disassembled completely offline with a portable copy of
// radare2 (no live debugger attach, zero interaction with the running
// game beyond the read-only capture). That revealed the container is
// actually a TWO-SEGMENT array, not one flat array:
//
//   EquipInventoryData + 0x24  = threshold (int32, read live -- 128 in
//                                 the session this was found in, but
//                                 this field, not the number 128, is
//                                 what's trusted)
//   EquipInventoryData + 0x58  = pointer to the LOW segment
//                                 (used when inventoryItemId < threshold)
//   EquipInventoryData + 0x48  = pointer to the HIGH segment
//                                 (used when inventoryItemId >= threshold,
//                                 indexed from (id - threshold))
//   entry = low/high segment base + localIndex * 16, 16-byte struct:
//     +0x0 int32  uniqueId   (ItemUniqueIdPrefix-tagged: Weapon
//                              0x80800000, Armor 0x90800000, Accessory
//                              0xA0000000, Goods 0xB0000000)
//     +0x4 int32  giveId     (the real EquipParamWeapon/Protector/
//                              Accessory/Goods row ID -- Weapon has no
//                              prefix bits, Goods is OR'd with
//                              0x40000000, matching AmySouls/
//                              DS3RuntimeScripting's ItemParamIdPrefix)
//     +0x8 uint32 quantity
//     +0xC int32  unknown1
//
// Verified live against six real entries: R1's base-level Reinforced
// Club (uniqueId=0x8080021E, giveId=8030000 exactly -- matches
// param_names.json's EquipParamWeapon row, quantity=1), L1's starter
// weapon (properly Weapon-prefixed uniqueId, quantity=1), and four
// low-index Goods entries (uniqueId/giveId both correctly
// Goods-prefixed with matching row IDs, quantity=1 each) -- both
// segments, both prefix families, all internally consistent. This is
// the real container layout, not another coincidence.
constexpr uintptr_t kInventoryThresholdOffset = 0x24;  // from EquipInventoryData
constexpr uintptr_t kInventoryLowSegmentPtrOffset = 0x58;  // from EquipInventoryData
constexpr uintptr_t kInventoryHighSegmentPtrOffset = 0x48;  // from EquipInventoryData
constexpr SIZE_T kInventoryEntryStride = 16;  // {int32 uniqueId, int32 giveId, uint32 qty, int32 unk1}

// Recognized ItemUniqueIdPrefix high bytes (AmySouls/DS3RuntimeScripting's
// equip_game_data.h), used only as a sanity check that a resolved entry
// looks like a real item and not a stale/garbage read.
constexpr int32_t kUniqueIdPrefixWeapon = static_cast<int32_t>(0x80800000);
constexpr int32_t kUniqueIdPrefixArmor = static_cast<int32_t>(0x90800000);
constexpr int32_t kUniqueIdPrefixAccessory = static_cast<int32_t>(0xA0000000);
constexpr int32_t kUniqueIdPrefixGoods = static_cast<int32_t>(0xB0000000);

bool LooksLikeRealUniqueId(int32_t uniqueId) {
  int32_t prefix = uniqueId & static_cast<int32_t>(0xFFF00000);
  return prefix == kUniqueIdPrefixWeapon || prefix == kUniqueIdPrefixArmor ||
         prefix == kUniqueIdPrefixAccessory || prefix == kUniqueIdPrefixGoods;
}

const char* CategoryForUniqueId(int32_t uniqueId) {
  int32_t prefix = uniqueId & static_cast<int32_t>(0xFFF00000);
  if (prefix == kUniqueIdPrefixWeapon) return "Weapon";
  if (prefix == kUniqueIdPrefixArmor) return "Armor";
  if (prefix == kUniqueIdPrefixAccessory) return "Accessory";
  if (prefix == kUniqueIdPrefixGoods) return "Goods";
  return "Unknown";
}

// (ItemLookup itself is defined earlier in this file, just above
// RunStats, since RunStats needs the full type -- same reasoning as
// ItemNameTable above it.)

// The container header (threshold + both segment base pointers). Read
// once and reused across many lookups -- see ResolveInventoryLayout /
// ResolveItemFast below, used by the full-inventory walk so it doesn't
// re-read these three fields on every one of a few thousand slots.
struct InventoryLayout {
  bool ok = false;
  int32_t threshold = 0;
  uintptr_t lowSegmentBase = 0;
  uintptr_t highSegmentBase = 0;
  int32_t capacity = 0;  // the container's own loop bound, from disassembly (EquipInventoryData+0x10)
};

constexpr uintptr_t kInventoryCapacityOffset = 0x10;  // from EquipInventoryData -- the exact
                                                       // bound the game's own iteration code uses
                                                       // for this array (see docs/TECHNICAL.md)

InventoryLayout ResolveInventoryLayout(HANDLE hProcess, uintptr_t equipInventoryData) {
  InventoryLayout layout;
  SIZE_T br = 0;
  if (!ReadProcessMemory(hProcess,
                          reinterpret_cast<LPCVOID>(equipInventoryData + kInventoryCapacityOffset),
                          &layout.capacity, sizeof(layout.capacity), &br))
    return layout;
  if (!ReadProcessMemory(hProcess,
                          reinterpret_cast<LPCVOID>(equipInventoryData + kInventoryThresholdOffset),
                          &layout.threshold, sizeof(layout.threshold), &br))
    return layout;
  if (!ReadProcessMemory(hProcess,
                          reinterpret_cast<LPCVOID>(equipInventoryData + kInventoryLowSegmentPtrOffset),
                          &layout.lowSegmentBase, sizeof(layout.lowSegmentBase), &br))
    return layout;
  if (!ReadProcessMemory(hProcess,
                          reinterpret_cast<LPCVOID>(equipInventoryData + kInventoryHighSegmentPtrOffset),
                          &layout.highSegmentBase, sizeof(layout.highSegmentBase), &br))
    return layout;
  layout.ok = layout.lowSegmentBase != 0 && layout.highSegmentBase != 0;
  return layout;
}

ItemLookup ReadItemEntry(HANDLE hProcess, uintptr_t entryAddr) {
  ItemLookup result;
  unsigned char entry[kInventoryEntryStride] = {};
  SIZE_T br = 0;
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(entryAddr), entry, sizeof(entry), &br))
    return result;

  int32_t uniqueId = 0, giveId = 0;
  uint32_t quantity = 0;
  std::memcpy(&uniqueId, entry + 0, 4);
  std::memcpy(&giveId, entry + 4, 4);
  std::memcpy(&quantity, entry + 8, 4);
  result.ok = true;
  result.uniqueId = uniqueId;
  result.giveId = giveId;
  result.quantity = quantity;
  result.plausible = LooksLikeRealUniqueId(uniqueId);
  return result;
}

// Given an already-resolved layout (see ResolveInventoryLayout), resolves
// one inventoryItemId with no extra header reads -- just the segment
// pick and the one entry read.
ItemLookup ResolveItemFast(HANDLE hProcess, const InventoryLayout& layout, int32_t inventoryItemId) {
  if (inventoryItemId < 0 || !layout.ok) return ItemLookup{};
  bool useLow = inventoryItemId < layout.threshold;
  uintptr_t segmentBase = useLow ? layout.lowSegmentBase : layout.highSegmentBase;
  int32_t localIndex = useLow ? inventoryItemId : (inventoryItemId - layout.threshold);
  uintptr_t entryAddr = segmentBase + static_cast<uintptr_t>(localIndex) * kInventoryEntryStride;
  return ReadItemEntry(hProcess, entryAddr);
}

ItemLookup ResolveItem(HANDLE hProcess, uintptr_t equipInventoryData, int32_t inventoryItemId) {
  if (inventoryItemId < 0) return ItemLookup{};  // empty slot
  InventoryLayout layout = ResolveInventoryLayout(hProcess, equipInventoryData);
  if (!layout.ok) return ItemLookup{};
  return ResolveItemFast(hProcess, layout, inventoryItemId);
}

// ---- item name lookup table (optional) -----------------------------------
//
// Loads item_names.tsv: category, param row id, English name, one per line.
// It's generated from the game's own text in the user's install by
// tools/extract_treasures.py, like treasures.tsv.
// Loading it is entirely optional -- if the file is missing,
// giveId/uniqueId output still works exactly as before, just without a
// resolved name. (ItemNameTable itself is defined earlier in this file,
// just above RunStats, since RunStats needs the full type.)

// generated\item_names.tsv with the other generated tables (Paths()).
std::wstring FindItemNameTablePath() { return Paths().Generated(L"item_names.tsv"); }

ItemNameTable LoadItemNameTable(const std::wstring& path) {
  ItemNameTable table;
  std::ifstream in(path);
  if (!in) return table;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    size_t tab1 = line.find('\t');
    if (tab1 == std::string::npos) continue;
    size_t tab2 = line.find('\t', tab1 + 1);
    if (tab2 == std::string::npos) continue;
    std::string category = line.substr(0, tab1);
    std::string id = line.substr(tab1 + 1, tab2 - tab1 - 1);
    std::string name = line.substr(tab2 + 1);
    if (category == "Weapon") table.weapon.emplace(std::move(id), std::move(name));
    else if (category == "Protector") table.protector.emplace(std::move(id), std::move(name));
    else if (category == "Accessory") table.accessory.emplace(std::move(id), std::move(name));
    else if (category == "Goods") table.goods.emplace(std::move(id), std::move(name));
    else if (category == "Magic") table.magic.emplace(std::move(id), std::move(name));
  }
  table.loaded = true;
  return table;
}

// Spells are looked up directly by their MagicParam id -- no uniqueId
// prefix indirection like LookupItemName's other categories, since
// getSpell() (see ResolveEquippedSpells below) returns that id
// straight from the game's own attunement-slot array.
std::string LookupSpellName(const ItemNameTable& table, int32_t magicParamId) {
  if (!table.loaded || magicParamId <= 0) return "";
  auto it = table.magic.find(std::to_string(magicParamId));
  return it == table.magic.end() ? "" : it->second;
}

// ItemParamIdPrefix (AmySouls/DS3RuntimeScripting's equip_game_data.h) --
// the prefix baked into giveId itself, distinct from the
// kUniqueIdPrefix* family above which tags uniqueId instead.
constexpr int32_t kGiveIdPrefixProtector = static_cast<int32_t>(0x10000000);
constexpr int32_t kGiveIdPrefixAccessory = static_cast<int32_t>(0x20000000);
constexpr int32_t kGiveIdPrefixGoods = static_cast<int32_t>(0x40000000);

// Turns {uniqueId, giveId} into a name-table lookup key. Weapons are
// the one case that needs adjusting first: giveId encodes reinforcement
// level as +0..+10 within each 100-wide infusion block (verified live
// this session -- L1's Pyromancy Flame +4 was giveId 13400004, base row
// 13400000), and the name table only has entries at the unreinforced
// (…00) row, so the low two digits are stripped before lookup.
std::string LookupItemName(const ItemNameTable& table, int32_t uniqueId, int32_t giveId) {
  if (!table.loaded) return "";
  int32_t prefix = uniqueId & static_cast<int32_t>(0xFFF00000);
  const std::unordered_map<std::string, std::string>* map = nullptr;
  int32_t nameRowId = giveId;
  if (prefix == kUniqueIdPrefixWeapon) {
    nameRowId = giveId - (giveId % 100);
    map = &table.weapon;
  } else if (prefix == kUniqueIdPrefixArmor) {
    nameRowId = giveId - kGiveIdPrefixProtector;
    map = &table.protector;
  } else if (prefix == kUniqueIdPrefixAccessory) {
    nameRowId = giveId - kGiveIdPrefixAccessory;
    map = &table.accessory;
  } else if (prefix == kUniqueIdPrefixGoods) {
    nameRowId = giveId - kGiveIdPrefixGoods;
    map = &table.goods;
  } else {
    return "";
  }
  auto it = map->find(std::to_string(nameRowId));
  return it == map->end() ? "" : it->second;
}

// The 14 equipped-item slots (weapons, armor, rings), all sourced from a
// ResolvedProfile. Shared by `equip` and `stats`'s live change-detection.
std::vector<SlotInfo> BuildEquipSlotList(const ResolvedProfile& profile) {
  return {
      {"R1", profile.rWeapon1Address}, {"R2", profile.rWeapon2Address},
      {"R3", profile.rWeapon3Address}, {"L1", profile.lWeapon1Address},
      {"L2", profile.lWeapon2Address}, {"L3", profile.lWeapon3Address},
      {"Head", profile.headAddress},   {"Chest", profile.chestAddress},
      {"Hands", profile.handsAddress}, {"Legs", profile.legsAddress},
      {"Ring1", profile.ring1Address}, {"Ring2", profile.ring2Address},
      {"Ring3", profile.ring3Address}, {"Ring4", profile.ring4Address},
      {"Arrow1", profile.primaryArrowAddress}, {"Bolt1", profile.primaryBoltAddress},
      {"Arrow2", profile.secondaryArrowAddress}, {"Bolt2", profile.secondaryBoltAddress},
      {"Covenant", profile.covenantAddress},
  };
}

// Resolves one inventoryItemId to a printable description -- "(empty)"
// for -1, a failure/unrecognized-prefix message, or the full
// [Category] uniqueId/giveId/qty/name line. Shared by `equip` and
// `stats`'s live change-detection so both print identically.
std::string FormatItemDescription(HANDLE hProcess, uintptr_t equipInventoryData,
                                   const ItemNameTable& nameTable, int32_t id) {
  if (id < 0) return "(empty)";
  auto item = ResolveItem(hProcess, equipInventoryData, id);
  char buf[256];
  if (!item.ok) {
    std::snprintf(buf, sizeof(buf), "inventoryItemId=%d -> lookup FAILED (unreadable)", id);
  } else if (!item.plausible) {
    std::snprintf(buf, sizeof(buf),
                  "inventoryItemId=%d -> uniqueId=0x%08X giveId=%d qty=%u  ** unrecognized "
                  "uniqueId prefix -- treat as unverified **",
                  id, static_cast<uint32_t>(item.uniqueId), item.giveId, item.quantity);
  } else {
    std::string itemName = LookupItemName(nameTable, item.uniqueId, item.giveId);
    std::snprintf(buf, sizeof(buf), "inventoryItemId=%d -> [%s] uniqueId=0x%08X giveId=%-10d qty=%-3u %s",
                  id, CategoryForUniqueId(item.uniqueId), static_cast<uint32_t>(item.uniqueId),
                  item.giveId, item.quantity, itemName.empty() ? "(name not found)" : itemName.c_str());
  }
  return std::string(buf);
}

int RunEquip() {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;

  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  if (moduleBase == 0) {
    Log("Could not resolve module base/size.");
    CloseHandle(hProcess);
    return 4;
  }

  auto profile = ResolvePlayerProfile(hProcess, moduleBase, moduleSize);
  if (profile.xBase == 0) {
    Log("FAILED to resolve player base (BaseA scan). See docs/TECHNICAL.md.");
    CloseHandle(hProcess);
    return 6;
  }

  uintptr_t equipInventoryData = profile.xBase + kEquipGameDataOffset + kEquipInventoryDataOffset;
  auto slots = BuildEquipSlotList(profile);

  ItemNameTable nameTable = LoadItemNameTable(FindItemNameTablePath());
  Log("Resolving equipped-item identity (two-segment array, disassembly-verified -- see "
      "docs/TECHNICAL.md).%s",
      nameTable.loaded ? "" : " (no item names yet -- run  wasd-cli.exe extract;  showing raw IDs only)");

  for (const auto& slot : slots) {
    int32_t id = -1;
    SIZE_T br = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(slot.address), &id, sizeof(id), &br);
    Log("%-6s %s", slot.name,
        FormatItemDescription(hProcess, equipInventoryData, nameTable, id).c_str());
  }

  CloseHandle(hProcess);
  return 0;
}

// ---- inventory mode: enumerate the full player inventory (read-only) --
//
// Walks every possible inventoryItemId, not just the 14 equipped slots
// `equip` covers -- every item ever acquired, worn or not. Uses the
// same disassembly-verified two-segment chain as `equip` (see above and
// docs/TECHNICAL.md), reading the container's header once
// (ResolveInventoryLayout) rather than per-slot. The upper bound comes
// from EquipInventoryData+0x10 -- not a guess, this is the exact loop
// bound the game's own code uses when it walks this same array (seen
// directly in the disassembly that found this chain). Freed/never-used
// slots read back with an unrecognized uniqueId prefix and are skipped
// silently; only entries that pass the same sanity check `equip` uses
// are printed.

int RunInventory() {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;

  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  if (moduleBase == 0) {
    Log("Could not resolve module base/size.");
    CloseHandle(hProcess);
    return 4;
  }

  auto profile = ResolvePlayerProfile(hProcess, moduleBase, moduleSize);
  if (profile.xBase == 0) {
    Log("FAILED to resolve player base (BaseA scan). See docs/TECHNICAL.md.");
    CloseHandle(hProcess);
    return 6;
  }

  uintptr_t equipInventoryData = profile.xBase + kEquipGameDataOffset + kEquipInventoryDataOffset;
  InventoryLayout layout = ResolveInventoryLayout(hProcess, equipInventoryData);
  if (!layout.ok) {
    Log("FAILED to resolve the inventory container layout. See docs/TECHNICAL.md.");
    CloseHandle(hProcess);
    return 6;
  }

  ItemNameTable nameTable = LoadItemNameTable(FindItemNameTablePath());
  Log("Walking inventoryItemId 0..%d (container capacity, from EquipInventoryData+0x%llx -- the "
      "game's own loop bound for this array). threshold=%d, low segment=0x%llx, high segment="
      "0x%llx.%s",
      layout.capacity - 1, static_cast<unsigned long long>(kInventoryCapacityOffset),
      layout.threshold, static_cast<unsigned long long>(layout.lowSegmentBase),
      static_cast<unsigned long long>(layout.highSegmentBase),
      nameTable.loaded ? "" : " (no item names yet -- run  wasd-cli.exe extract;  showing raw IDs only)");

  LARGE_INTEGER freq, start, end;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);

  int found = 0;
  for (int32_t id = 0; id < layout.capacity; ++id) {
    auto item = ResolveItemFast(hProcess, layout, id);
    if (!item.ok || !item.plausible) continue;
    ++found;
    std::string itemName = LookupItemName(nameTable, item.uniqueId, item.giveId);
    Log("  id=%-5d [%-9s] uniqueId=0x%08X giveId=%-10d qty=%-3u %s", id,
        CategoryForUniqueId(item.uniqueId), static_cast<uint32_t>(item.uniqueId), item.giveId,
        item.quantity, itemName.empty() ? "(name not found)" : itemName.c_str());
  }

  QueryPerformanceCounter(&end);
  double millis = (end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
  CloseHandle(hProcess);
  Log("Done. %d item(s) found across %d slot(s) scanned, %.2fms total.", found, layout.capacity,
      millis);
  return 0;
}

// ---- ar mode: Attack Rating for every weapon owned (or any param row) --
//
// Verification tool: the in-game inventory screen shows every weapon's
// Attack Power (as base+bonus) at your current stats without equipping
// it -- two-handed figures while you're two-handing -- so one `ar` run
// gives many independent checks instead of one per equip. The
// blacksmith's infusion menu previews each infusion the same way, which
// is what `--infusions` is for.
//
//   ar [--verbose]           every Weapon-category item in the inventory
//   ar --row <giveId>        one EquipParamWeapon row (owned or not)
//   ar --infusions <giveId>  all 16 infusion rows of that weapon/level
int RunAr(const std::vector<int32_t>& explicitRows) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  auto profile = moduleBase ? ResolvePlayerProfile(hProcess, moduleBase, moduleSize)
                            : ResolvedProfile{};
  if (profile.xBase == 0) {
    Log("FAILED to resolve player base (BaseA scan). See docs/TECHNICAL.md.");
    CloseHandle(hProcess);
    return 6;
  }

  int32_t str = 0, dex = 0, intel = 0, faith = 0, luck = 0;
  SIZE_T br = 0;
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.strengthAddress), &str,
                    sizeof(str), &br);
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.dexterityAddress), &dex,
                    sizeof(dex), &br);
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.intelligenceAddress), &intel,
                    sizeof(intel), &br);
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.faithAddress), &faith,
                    sizeof(faith), &br);
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.luckAddress), &luck, sizeof(luck),
                    &br);
  Log("Stats: Str=%d Dex=%d Int=%d Fth=%d Lck=%d. Shown as the game's own base+bonus "
      "(base-penalty when a requirement is unmet). NOTE: the inventory screen shows two-handed "
      "figures while you're two-handing, so compare against 1H or 2H accordingly.",
      str, dex, intel, faith, luck);

  ItemNameTable nameTable = LoadItemNameTable(FindItemNameTablePath());
  auto printRow = [&](int32_t giveId, const char* tag) {
    auto ar = ComputeWeaponAr(hProcess, moduleBase, giveId, str, dex, intel, faith, luck);
    std::string name = LookupItemName(nameTable, static_cast<int32_t>(0x80800000), giveId);
    if (!ar.ok) {
      Log("  %s giveId=%-9d %-32s (no EquipParamWeapon row)", tag, giveId, name.c_str());
      return;
    }
    std::string oneH = FormatWeaponAr(ar, false), twoH = FormatWeaponAr(ar, true);
    Log("  %s giveId=%-9d %-32s%s%s 1H:%s%s  [aec %d: %s]", tag, giveId,
        name.empty() ? "(name not found)" : name.c_str(), ar.isCatalyst ? " [" : "",
        ar.isCatalyst ? (std::string(ar.catalystKind) + "]").c_str() : "", oneH.c_str(),
        twoH != oneH ? ("  2H:" + twoH).c_str() : "", ar.attackElementCorrectId,
        DescribeAttackElementCorrect(ar.aec).c_str());
  };

  if (!explicitRows.empty()) {
    for (int32_t row : explicitRows) printRow(row, "row");
  } else {
    uintptr_t equipInventoryData = profile.xBase + kEquipGameDataOffset + kEquipInventoryDataOffset;
    InventoryLayout layout = ResolveInventoryLayout(hProcess, equipInventoryData);
    if (!layout.ok) {
      Log("FAILED to resolve the inventory container layout.");
      CloseHandle(hProcess);
      return 6;
    }
    for (int32_t id = 0; id < layout.capacity; ++id) {
      auto item = ResolveItemFast(hProcess, layout, id);
      if (!item.ok || !item.plausible || item.uniqueId >> 24 != kUniqueIdPrefixWeapon >> 24)
        continue;
      if (item.giveId == 110000) continue;  // Fists placeholder, one line is enough via --row
      char tag[16];
      std::snprintf(tag, sizeof(tag), "id=%-4d", id);
      printRow(item.giveId, tag);
    }
  }
  Log("Done.");
  CloseHandle(hProcess);
  return 0;
}

// ---- memdiff mode: live byte-diff of a struct, with noise learning ----
//
// Built for the active-spell (top slot) hunt: PlayerGameData itself was
// ruled out by a 12KB diff, so the remaining candidates are objects
// reached through pointers (EquipMagicData at PlayerGameData+0x470, the
// ChrIns/WorldChrMan side). Snapshots `len` bytes every 100ms and prints
// every aligned int32 that changes. For the first `learn` ticks the user
// touches nothing; anything that changes anyway (timers, animation
// state) is marked noise and suppressed for the rest of the run.
//
//   memdiff <base> [hex deref offset ...] [--len hex] [--learn n] [--iter n]
//   base: pgd (PlayerGameData) | chrins (WorldChrMan player, "s1") |
//         chrmods ("s2") | chrdata (HP/FP struct, "s3") | <hex address>
//   Each deref offset: read the pointer at current+offset, continue there.
// --summary: instead of a line per change, print one line per changed
// int32 at the end -- change count plus first/last/min/max read as a
// float -- for actions that change too much to read live (walking).
// Only cells that look like ordinary floats are listed (finite, not
// denormal, magnitude under 1e6), most-changed first.
int RunMemDiff(const std::wstring& baseName, const std::vector<uintptr_t>& derefs, SIZE_T len,
               int learnTicks, int iterations, bool summary) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  if (moduleBase == 0) {
    CloseHandle(hProcess);
    return 4;
  }

  uintptr_t addr = 0;
  if (baseName == L"pgd") {
    addr = ResolvePlayerProfile(hProcess, moduleBase, moduleSize).xBase;
  } else if (baseName == L"chrins" || baseName == L"chrmods" || baseName == L"chrdata") {
    auto resolved = ResolvePlayerHp(hProcess, moduleBase, moduleSize);
    addr = baseName == L"chrins"    ? resolved.entityAddress
           : baseName == L"chrmods" ? resolved.chrModulesAddress
                                    : resolved.structBase;
  } else {
    addr = static_cast<uintptr_t>(std::wcstoull(baseName.c_str(), nullptr, 16));
  }
  SIZE_T br = 0;
  for (uintptr_t off : derefs) {
    uintptr_t next = 0;
    if (addr == 0 || !ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr + off), &next,
                                        sizeof(next), &br)) {
      addr = 0;
      break;
    }
    addr = next;
  }
  if (addr == 0) {
    Log("FAILED to resolve the base/deref chain.");
    CloseHandle(hProcess);
    return 6;
  }
  len &= ~static_cast<SIZE_T>(3);
  Log("memdiff 0x%llx, 0x%llx bytes. Learning noise for %d ticks (~%.1fs) -- DON'T TOUCH "
      "ANYTHING yet.",
      static_cast<unsigned long long>(addr), static_cast<unsigned long long>(len), learnTicks,
      learnTicks / 10.0);

  std::vector<unsigned char> prev(len), cur(len);
  std::vector<bool> noisy(len / 4, false);
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), prev.data(), len, &br)) {
    Log("Initial read FAILED (error %lu).", GetLastError());
    CloseHandle(hProcess);
    return 6;
  }
  struct CellStats {
    int changes = 0;
    float first = 0.0f, last = 0.0f, lo = 0.0f, hi = 0.0f;
  };
  std::vector<CellStats> stats(summary ? len / 4 : 0);
  for (int i = 0; i < iterations; ++i) {
    Sleep(100);
    if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), cur.data(), len, &br))
      continue;
    bool learning = i < learnTicks;
    if (i == learnTicks) {
      size_t n = 0;
      for (bool b : noisy) n += b;
      Log("Learning done: %zu noisy int32(s) suppressed. NOW do the action.", n);
    }
    for (size_t w = 0; w < len / 4; ++w) {
      int32_t a = 0, b = 0;
      std::memcpy(&a, prev.data() + w * 4, 4);
      std::memcpy(&b, cur.data() + w * 4, 4);
      if (a == b) continue;
      if (learning) {
        noisy[w] = true;
      } else if (!noisy[w]) {
        float fa = 0.0f, fb = 0.0f;
        std::memcpy(&fa, &a, 4);
        std::memcpy(&fb, &b, 4);
        if (summary) {
          auto& c = stats[w];
          if (c.changes == 0) c.first = c.lo = c.hi = fa;
          ++c.changes;
          c.last = fb;
          c.lo = (std::min)(c.lo, fb);  // parenthesised: windows.h defines min/max macros
          c.hi = (std::max)(c.hi, fb);
        } else {
          Log("  +0x%04zx  %d -> %d   (0x%08X, as float %g)", w * 4, a, b,
              static_cast<uint32_t>(b), fb);
        }
      }
    }
    prev.swap(cur);
  }
  if (summary) {
    auto plausible = [](float f) {
      return std::isfinite(f) && (f == 0.0f || std::fabs(f) >= 1e-6f) && std::fabs(f) < 1e6f;
    };
    std::vector<size_t> order;
    for (size_t w = 0; w < stats.size(); ++w) {
      const auto& c = stats[w];
      if (c.changes > 0 && plausible(c.first) && plausible(c.last) && plausible(c.lo) &&
          plausible(c.hi))
        order.push_back(w);
    }
    std::sort(order.begin(), order.end(),
              [&](size_t x, size_t y) { return stats[x].changes > stats[y].changes; });
    Log("Summary: %zu float-looking cell(s) changed after learning (most-changed first):",
        order.size());
    for (size_t w : order) {
      const auto& c = stats[w];
      Log("  +0x%05zx  changes=%-4d first=%-12g last=%-12g min=%-12g max=%-12g delta=%g", w * 4,
          c.changes, c.first, c.last, c.lo, c.hi, c.last - c.first);
    }
  }
  Log("Done.");
  CloseHandle(hProcess);
  return 0;
}

// Defined further down (event flags, map data); the window worker uses them.
int CountFoundPickups(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize,
                      uintptr_t baseACellOffset, int mapArea, int* total);
// Bosses in `area` ("" if none), shard and key-item counts, for the window.
void WindowProgress(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize,
                    uintptr_t baseACellOffset, const std::string& area, std::string* bosses,
                    int* bossesDefeated, int* bossesTotal, int* bossesLater, std::string* shards);
std::string WindowKeyItems(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize,
                           const ResolvedProfile& profile);
std::string WindowUpgrade(HANDLE hProcess, uintptr_t moduleBase, const ResolvedProfile& profile,
                          int32_t inventoryItemId);
std::unordered_map<int32_t, int> HeldGoodsQty(HANDLE hProcess, const ResolvedProfile& profile);
void WindowNearest(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
                   const PlayerPose& pose, const PlayerArea& where, std::string* windowLine,
                   std::string* overlayLine);
int CountFoundInArea(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
                     const std::string& area, int* total);
std::string WindowBossHint(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, const std::string& area,
                           const PlayerPose& pose);

// ---- window mode (Milestone 3) ---------------------------------------
//
// A small, separate, always-on-top companion window -- not drawn over
// the game (that's Phase 2) -- showing live values plus the brief's
// metrics: last-read time, read latency, measured poll rate, and this
// process's own CPU and memory.
//
// Two threads, so neither can stall the other:
//   - a polling worker does every ReadProcessMemory call on the same
//     fixed-deadline 10Hz schedule as `stats --live`, fills a shared
//     snapshot under a lock, and posts kMsgSnapshot to the window;
//   - the UI thread only draws the latest snapshot. Dragging the window
//     (a modal loop) therefore never pauses the readings.
// F10 (the same keyboard hook as `stats --live`) posts kMsgTogglePause
// to the window; while paused the worker blocks on an event, so it
// makes zero reads and uses no CPU.
constexpr UINT kMsgSnapshot = WM_APP + 2;  // worker -> window: new snapshot ready
constexpr wchar_t kWindowClassName[] = L"WasdWindow";

// ---- live page data (overlay mode) -------------------------------------
//
// Structured copies of what the live page shows, filled by the polling
// worker and served as JSON by the page server (see LiveServerMain).
struct LiveNearby {
  std::string label;     // unfound: at the spoiler tier ("" at Off and Vague: counts only); found: the name
  std::string category;  // unfound at Full: the item's kind, shown beside its name
  bool found = false;
  std::string clock;
  float dist = 0, dy = 0, x = 0, z = 0;
  double angle = 0;  // bearing from straight ahead, clockwise, radians
  bool showHeight = false;
};
struct LiveBoss {
  std::string name;  // "" when hidden by the tier
  bool defeated = false, optional = false, nearest = false, humanType = false;
  bool later = false;  // its route step isn't open yet (BossIsLater)
  std::string opens;   // when later: what opens it at the tier
  int statsLevel = 0;  // 0 none, 1 weakness only, 2 full numbers
  float dist = -1;     // metres to an undefeated boss with a known position, else -1
  std::string weak;
  float rate[8] = {};      // physical, slash, strike, thrust, magic, fire, lightning, dark
  int16_t resist[5] = {};  // poison, toxic, bleed, curse, frost
};
struct LiveRouteStep {
  std::string label, kind, from, wayIn;  // label/from/wayIn at the tier (wayIn only at Full)
  int status = 0;                         // 0 locked, 1 open, 2 done
  int requiredLeft = 0, optionalLeft = 0;
  bool unnamed = false;                   // "A new area beyond ..." (tier Category)
};
struct LiveMissable {
  std::string id, kind, title, text, lose, area;  // at the tier
  int status = 0;     // 0 pending, 1 missed (lockout happened), 2 done, 3 don't care
  bool here = false;  // its area is the current area
  bool automatic = false;  // done because the game shows it (not a mark)
};
struct LiveWeapon {
  std::string slot, name, upgrade;
  bool active = false, twoHanded = false, catalyst = false, haveAr = false;
  bool present[5] = {};
  int base[5] = {}, bonus1h[5] = {}, bonus2h[5] = {};  // Phys, Magic, Fire, Lightning, Dark
  int spellBuff = 0;
  // Level planner: total AR change (1H / 2H), spell buff change, and
  // whether a requirement penalty goes away, for Str, Dex, Int, Fth, Lck +1.
  int plan1h[5] = {}, plan2h[5] = {}, planBuff[5] = {};
  bool planFix[5] = {};
  // Infusions at current stats: total AR (1H / 2H; spell buff for catalysts)
  // of each of the 16 infusion rows; -1 where the row doesn't exist.
  double inf1h[16] = {}, inf2h[16] = {};
  int infCurrent = -1;          // this weapon's infusion index, -1 if it can't be infused
  std::string infusion;         // "Heavy: +18 AR (gem held)" -- the best other infusion, at the grip
  std::string infusionDetail;   // top three
};

// This session's walked trail for the live page's map: a point every
// 1 unit moved, plus deaths and bonfire rests. mapKey = map number * 2 +
// 1 in Untended Graves (map coordinate spaces overlap, so the page only
// draws points from the player's current map and world).
struct TrailPoint {
  float x, y, z;
  int16_t mapKey;
  uint8_t kind;  // 0 walk, 1 death, 2 bonfire rest
};
struct LiveTrail {
  CRITICAL_SECTION lock;
  std::vector<TrailPoint> points;
};
LiveTrail g_trail;
constexpr size_t kTrailMaxPoints = 50000;

void TrailAdd(const TrailPoint& p) {
  EnterCriticalSection(&g_trail.lock);
  if (g_trail.points.size() < kTrailMaxPoints) g_trail.points.push_back(p);
  LeaveCriticalSection(&g_trail.lock);
}

void LiveNearbyList(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
                    const PlayerPose& pose, const PlayerArea& where, const std::string& area,
                    std::vector<LiveNearby>* out);
void LiveBossList(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, const std::string& area,
                  const PlayerPose& pose, std::vector<LiveBoss>* out, int counts[4]);
void LiveRoute(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
               const std::string& character, const std::string& area, std::vector<LiveRouteStep>* steps,
               std::string* hint, int* hidden);
void LiveMissables(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
                   const std::string& character, const std::string& area, std::vector<LiveMissable>* out,
                   std::string* alert, int* hidden);

struct WindowSnapshot {
  std::string status;  // non-empty: shown instead of values (not attached, game closed...)
  bool paused = false;
  bool haveValues = false;
  int32_t hp = 0, maxHp = 0, fp = 0, maxFp = 0, stamina = 0, maxStamina = 0;
  int32_t level = 0, souls = 0, nextLevelCost = 0;
  bool haveEquipLoad = false;
  double equipLoad = 0.0, maxEquipLoad = 0.0;
  std::string rightHand, leftHand, spell, quickItem;
  PlayerPose pose;
  std::string areaName, lastBonfireName, currentArea;
  double sessionSeconds = 0.0, areaSeconds = 0.0, soulsPerHour = 0.0;
  int32_t sessionDeaths = 0, deathsTotal = 0;
  int itemsFound = -1, itemsTotal = 0;  // world pickups in this map section
  std::string bossesHere, shards;          // bosses in this area, shard counts
  std::string keyItems;                    // "3 / 39 obtained (2 held)"
  std::string upgrade;                     // active right-hand weapon's next upgrade
  std::string nearest;                     // closest unfound pickup, at the spoiler tier
  std::string nearestShort;                // same, overlay width
  std::string itemsScope;                  // "in Firelink Shrine" / "in this map section"
  std::string bossHint;                    // closest undefeated boss's weaknesses
  int bossesDefeated = 0, bossesTotal = 0, bossesLater = 0;  // later: not open yet (BossIsLater)
  int64_t soulsLost = 0, soulsRecovered = 0;
  std::string characterName;
  std::string lastReadTime;
  double lastLatencyMicros = 0.0;
  PerfWindow::Sample perf;
  DWORD pid = 0;
  // Live page only:
  std::vector<LiveNearby> nearby;
  std::vector<LiveBoss> bosses;
  std::vector<LiveWeapon> weapons;
  std::vector<LiveRouteStep> route;  // route hints, at the tier
  std::string routeHint;
  int routeHidden = 0;
  std::vector<LiveMissable> missables;  // missable-content warnings, at the tier
  std::string missableAlert;            // overlay line: a pending warning in this area
  int missablesHidden = 0;
  int bossCounts[4] = {};  // base defeated, base total, DLC defeated, DLC total
  int32_t attrs[9] = {};   // vigor, attunement, endurance, vitality, str, dex, int, faith, luck
  std::string planGain[9];  // level planner: what +1 in each attribute gives (short)
  std::string planDetail[9];
  std::string planBest;     // the attribute that helps the weapon in hand most
  bool gemHeld[16] = {};    // infusion gems in the inventory (index = infusion; 0 = Normal, none needed)
  int mapNumber = 0;
  bool untendedWorld = false;
};

struct WindowShared {
  CRITICAL_SECTION lock;
  WindowSnapshot snapshot;
  HWND hwnd = nullptr;
  HANDLE wakeEvent = nullptr;  // signalled on pause toggle and on shutdown
  volatile LONG paused = 0;
  volatile LONG stop = 0;
  HFONT font = nullptr;  // overlay font, remade only when the text size setting changes
  double scale = 1.0;                        // DPI / 96
};
WindowShared g_window;

void PublishSnapshot(const WindowSnapshot& snap) {
  EnterCriticalSection(&g_window.lock);
  g_window.snapshot = snap;
  LeaveCriticalSection(&g_window.lock);
  PostMessageW(g_window.hwnd, kMsgSnapshot, 0, 0);
}

// DS3's roll tiers by equip-load percentage (Fextralife, Equipment_Load).
const char* RollTypeForLoad(double percent) {
  if (percent < 30.0) return "light roll";
  if (percent < 70.0) return "medium roll";
  if (percent <= 100.0) return "heavy roll";
  return "overloaded";
}

// ---- what to level next -----------------------------------------------
//
// What +1 in each attribute would give, from the character as it is:
//   Vigor / Attunement -- the game's own curves, CalcCorrectGraph rows 100
//     (HP: 1/15/27/50/99 -> 300/550/1000/1300/1400) and 101 (FP: 1/15/35/
//     60/99 -> 50/120/280/350/450), found with `graphs` 2026-10-06 and
//     matching the wiki's tables. Scaled by current max / curve value, so
//     an Ember's +30% (live HP 590 / 627 = curve 454 / 483 x 1.30) and
//     rings carry over.
//   Endurance -- no curve found in the params: interpolated from the
//     Fextralife Endurance table's points, so marked "≈".
//   Vitality -- equip load +1 x the ring multiplier, and the roll tier.
//   Attunement slots -- ComputeBaseAttunementSlots.
//   Str / Dex / Int / Fth / Luck -- ComputeWeaponAr again with the stat +1
//     for every equipped weapon (computed on gear/level change).
// "Best" = the biggest gain for the weapon in hand (right hand first; a
// catalyst's spell buff counts), a requirement penalty removed first.
double EnduranceStaminaApprox(double endurance) {
  static const double pts[][2] = {{9, 92},   {11, 95},  {12, 97},  {13, 98},  {14, 100}, {15, 102}, {16, 104},
                                  {17, 106}, {18, 108}, {19, 110}, {20, 112}, {21, 114}, {22, 116}, {23, 118},
                                  {24, 120}, {25, 122}, {30, 134}, {35, 146}, {40, 160}, {50, 161}, {99, 170}};
  const int n = static_cast<int>(sizeof(pts) / sizeof(pts[0]));
  if (endurance <= pts[0][0]) return pts[0][1] - (pts[0][0] - endurance) * 1.5;
  for (int i = 1; i < n; ++i)
    if (endurance <= pts[i][0])
      return pts[i - 1][1] + (pts[i][1] - pts[i - 1][1]) * (endurance - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]);
  return pts[n - 1][1];
}

void BuildLevelPlan(HANDLE hProcess, uintptr_t moduleBase, WindowSnapshot* s) {
  static CalcCorrectGraphRow hpCurve, fpCurve;
  if (!hpCurve.ok) hpCurve = ReadCalcCorrectGraphRow(hProcess, moduleBase, 100);
  if (!fpCurve.ok) fpCurve = ReadCalcCorrectGraphRow(hProcess, moduleBase, 101);
  const int32_t* a = s->attrs;
  for (auto& g : s->planGain) g.clear();
  for (auto& d : s->planDetail) d.clear();
  if (a[0] <= 0) return;
  char buf[160];
  auto curveGain = [](const CalcCorrectGraphRow& c, int v, int current) -> int {
    if (!c.ok) return -1;
    double now = std::floor(EvaluateCalcCorrectGraph(c, v)), next = std::floor(EvaluateCalcCorrectGraph(c, v + 1));
    double scale = now > 0 && current > 0 ? current / now : 1.0;
    return static_cast<int>(std::lround((next - now) * scale));
  };
  int hp = curveGain(hpCurve, a[0], s->maxHp);
  if (hp >= 0) s->planGain[0] = "+" + std::to_string(hp) + " HP";
  int fp = curveGain(fpCurve, a[1], s->maxFp);
  if (fp >= 0) s->planGain[1] = "+" + std::to_string(fp) + " FP";
  if (ComputeBaseAttunementSlots(a[1] + 1) > ComputeBaseAttunementSlots(a[1])) s->planGain[1] += ", +1 slot";
  {
    double now = EnduranceStaminaApprox(a[2]), next = EnduranceStaminaApprox(a[2] + 1);
    double scale = now > 0 && s->maxStamina > 0 ? s->maxStamina / now : 1.0;
    s->planGain[2] = "≈+" + std::to_string(static_cast<int>(std::lround((next - now) * scale))) + " stamina";
  }
  if (s->haveEquipLoad && s->maxEquipLoad > 0) {
    double mult = s->maxEquipLoad / ComputeBaseMaxEquipLoad(a[3]);
    double newMax = s->maxEquipLoad + mult;
    std::string now = RollTypeForLoad(s->equipLoad / s->maxEquipLoad * 100.0);
    std::string then = RollTypeForLoad(s->equipLoad / newMax * 100.0);
    std::snprintf(buf, sizeof(buf), "+%.1f load", mult);
    s->planGain[3] = buf;
    if (then != now) s->planGain[3] += " (" + then + ")";
    std::snprintf(buf, sizeof(buf), "Equip load %.1f / %.1f -> %.1f (%s)", s->equipLoad, s->maxEquipLoad, newMax,
                  then.c_str());
    s->planDetail[3] = buf;
  }
  // Offensive stats: the weapon in hand first.
  const LiveWeapon* inHand = nullptr;
  for (const auto& w : s->weapons)
    if (w.active && (!inHand || w.slot[0] == 'R')) inHand = &w;
  int bestK = -1;
  double bestScore = 0;
  for (int k = 0; k < 5; ++k) {
    std::string detail;
    for (const auto& w : s->weapons) {
      int ar = w.twoHanded ? w.plan2h[k] : w.plan1h[k];
      std::string part;
      if (w.planFix[k]) part = "meets a requirement";
      if (ar) part += (part.empty() ? "" : ", ") + std::string(ar > 0 ? "+" : "") + std::to_string(ar) + " AR";
      if (w.catalyst && w.planBuff[k]) part += (part.empty() ? "" : ", ") + std::string("+") +
                                              std::to_string(w.planBuff[k]) + " spell buff";
      if (!part.empty()) detail += (detail.empty() ? "" : "; ") + w.name + ": " + part;
    }
    s->planDetail[4 + k] = detail;
    std::string gain;
    if (inHand) {
      int ar = inHand->twoHanded ? inHand->plan2h[k] : inHand->plan1h[k];
      if (inHand->planFix[k]) gain = "meets req.";
      else if (inHand->catalyst && inHand->planBuff[k]) gain = "+" + std::to_string(inHand->planBuff[k]) + " buff";
      else if (ar) gain = "+" + std::to_string(ar) + " AR";
      double score = (inHand->planFix[k] ? 1000 : 0) + ar + (inHand->catalyst ? inHand->planBuff[k] : 0);
      if (score > bestScore) { bestScore = score; bestK = k; }
    }
    if (gain.empty() && !detail.empty()) gain = "other gear";
    s->planGain[4 + k] = gain.empty() ? "–" : gain;
  }
  // Best other infusion per weapon, at its current grip.
  static const char* kInfusions[16] = {"Normal", "Heavy",     "Sharp", "Refined", "Simple", "Crystal",
                                       "Fire",   "Chaos",     "Lightning", "Deep", "Dark",  "Poison",
                                       "Blood",  "Raw",       "Blessed", "Hollow"};
  for (auto& w : s->weapons) {
    w.infusion.clear();
    w.infusionDetail.clear();
    if (w.infCurrent < 0) continue;
    const double* v = w.twoHanded ? w.inf2h : w.inf1h;
    const char* unit = w.catalyst ? " spell buff" : " AR";
    std::vector<int> order;
    for (int k = 0; k < 16; ++k)
      if (v[k] >= 0 && k != w.infCurrent) order.push_back(k);
    std::sort(order.begin(), order.end(), [&](int x, int y) { return v[x] > v[y]; });
    if (order.empty()) continue;
    double cur = v[w.infCurrent];
    int best = order[0];
    auto delta = [&](int k) {
      int d = static_cast<int>(std::lround(v[k] - cur));
      return (d >= 0 ? "+" : "") + std::to_string(d) + unit;
    };
    if (v[best] <= cur) {
      w.infusion = std::string(kInfusions[w.infCurrent]) + " is already best at your stats";
    } else {
      w.infusion = std::string(kInfusions[best]) + ": " + delta(best) +
                   (best == 0 ? "" : s->gemHeld[best] ? " (gem held)" : " (no gem yet)");
    }
    for (size_t i = 0; i < order.size() && i < 3; ++i)
      w.infusionDetail += (i ? ", " : "") + std::string(kInfusions[order[i]]) + " " + delta(order[i]) +
                          (order[i] == 0 || s->gemHeld[order[i]] ? "" : " (no gem)");
    w.infusionDetail = "vs " + std::string(kInfusions[w.infCurrent]) + " (total" + unit + ", " +
                       (w.twoHanded ? "two-handed" : "one-handed") + "): " + w.infusionDetail +
                       ". Totals add split damage (physical + element) together, which defences cut harder than one type, and leave out status build-up.";
  }

  static const char* kNames[5] = {"Strength", "Dexterity", "Intelligence", "Faith", "Luck"};
  if (!inHand) s->planBest = "";
  else if (bestK < 0)
    s->planBest = "Nothing in hand gains from Str/Dex/Int/Fth/Luck: " + s->planGain[0] + " from Vigor.";
  else
    s->planBest = std::string(kNames[bestK]) + " for " + inHand->name + " (" + s->planGain[4 + bestK] + ")";
}

// One attached game run: polls until the game exits or the guide stops
// (WindowPollWorker below waits for the game and calls this per run).
void PollGameRun(HANDLE hProcess, DWORD pid) {
  WindowSnapshot snap;
  snap.pid = pid;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  if (!moduleBase) {  // module list not ready yet (game still starting): retry later
    CloseHandle(hProcess);
    return;
  }
  EnterCriticalSection(&g_trail.lock);  // the map shows this run's path only
  g_trail.points.clear();
  LeaveCriticalSection(&g_trail.lock);
  const uintptr_t gameManCellOffset =
      moduleBase ? ResolveGameManCellOffset(hProcess, moduleBase, moduleSize) : 0;
  const AreaNames areaNames;
  AreaTracker areaTracker;
  const AreaInfo areaInfo;
  SessionTracker tracker;
  tracker.log.enabled = true;  // window/overlay sessions are always saved
  SessionStats& session = tracker.stats;
  LARGE_INTEGER lastSessionTick{};

  // Everything below is (re)built by resolveAll() whenever the game's
  // current player object changes -- see ReadCurrentPlayerIns.
  ResolvedPointers resolved;
  ResolvedProfile profile;
  uintptr_t wcmCellOffset = 0;
  uintptr_t equipInventoryData = 0;
  std::vector<SlotInfo> slots;
  std::vector<int32_t> lastSlotIds;
  int32_t lastLevel = INT32_MIN;
  int32_t lastHandIdx[3] = {INT32_MIN, INT32_MIN, INT32_MIN};  // right, left, quick item
  int32_t lastSpellIdx = INT32_MIN;
  bool inGame = false;
  ULONGLONG lastResolveAttempt = 0;
  auto resolveAll = [&]() -> bool {
    resolved = moduleBase ? ResolvePlayerHp(hProcess, moduleBase, moduleSize) : ResolvedPointers{};
    if (resolved.worldChrManCellOffset) wcmCellOffset = resolved.worldChrManCellOffset;
    profile = moduleBase ? ResolvePlayerProfile(hProcess, moduleBase, moduleSize) : ResolvedProfile{};
    if (resolved.structBase == 0 || profile.xBase == 0) return false;
    equipInventoryData = profile.xBase + kEquipGameDataOffset + kEquipInventoryDataOffset;
    slots = BuildEquipSlotList(profile);
    lastSlotIds.assign(slots.size(), INT32_MIN);  // forces names + equip load to re-resolve
    lastLevel = INT32_MIN;
    lastHandIdx[0] = lastHandIdx[1] = lastHandIdx[2] = INT32_MIN;
    lastSpellIdx = INT32_MIN;
    return true;
  };

  auto readI32 = [&](uintptr_t addr) {
    int32_t v = 0;
    SIZE_T br = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), &v, sizeof(v), &br);
    return v;
  };
  auto itemName = [&](int32_t inventoryItemId) -> std::string {
    if (inventoryItemId < 0) return "(empty)";
    auto item = ResolveItem(hProcess, equipInventoryData, inventoryItemId);
    if (!item.ok || !item.plausible) return "(unresolved)";
    std::string name = LookupItemName(ItemNames(), item.uniqueId, item.giveId);
    return name.empty() ? "(name not found)" : name;
  };
  auto slotItemId = [&](const char* slotName) -> int32_t {
    for (size_t i = 0; i < slots.size(); ++i) {
      if (std::string(slots[i].name) == slotName) return lastSlotIds[i];
    }
    return -1;
  };
  // Same categorisation as stats' readAndPrintEquipLoadAndPoise.
  auto recomputeEquipLoad = [&]() {
    std::vector<EquippedWeightItem> items;
    std::vector<int32_t> ringGiveIds;
    for (size_t i = 0; i < slots.size(); ++i) {
      if (lastSlotIds[i] < 0) continue;
      auto item = ResolveItem(hProcess, equipInventoryData, lastSlotIds[i]);
      if (!item.ok || !item.plausible) continue;
      std::string slotName = slots[i].name;
      EquipWeightTable table = EquipWeightTable::kWeapon;
      if (slotName == "Head" || slotName == "Chest" || slotName == "Hands" || slotName == "Legs") {
        table = EquipWeightTable::kProtector;
      } else if (slotName.rfind("Ring", 0) == 0) {
        table = EquipWeightTable::kAccessory;
        ringGiveIds.push_back(item.giveId);
      } else if (slotName == "Covenant") {
        table = EquipWeightTable::kAccessory;
      }
      items.push_back({table, item.giveId, slotName});
    }
    auto load = ComputeCurrentEquipLoad(hProcess, moduleBase, items);
    snap.haveEquipLoad = load.ok;
    snap.equipLoad = load.total;
    snap.maxEquipLoad = ComputeBaseMaxEquipLoad(readI32(profile.vitalityAddress)) *
                        ComputeRingEquipLoadRateMultiplier(hProcess, moduleBase, ringGiveIds);
  };

  PerfWindow perf;
  perf.Reset();
  LARGE_INTEGER qpcFreq, nextTick;
  QueryPerformanceFrequency(&qpcFreq);
  QueryPerformanceCounter(&nextTick);
  const LONGLONG tickInterval =
      static_cast<LONGLONG>(qpcFreq.QuadPart * kPollIntervalMs / 1000.0);
  int ticksSincePerfSample = 0;
  int keyItemTicks = 0;
  int lastSpoilerTier = -1;
  TrailPoint lastTrail{0, 0, 0, -1, 0};
  int32_t lastTrailDeaths = -1, lastTrailBonfire = INT32_MIN;

  while (!g_window.stop) {
    AdoptNewMapData();  // safe point: no reader holds the tables between ticks
    if (g_window.paused) {
      perf.Report();
      if (auto ps = perf.Take(); ps.valid) tracker.log.WritePerf(PerfCsvRow(ps, tracker.stats.seconds));
      Log("PAUSED (F10) -- polling stopped, zero reads until resumed.");
      snap.paused = true;
      PublishSnapshot(snap);
      while (g_window.paused && !g_window.stop) {
        WaitForSingleObject(g_window.wakeEvent, INFINITE);  // blocks: no reads, no CPU
      }
      if (g_window.stop) break;
      Log("RESUMED.");
      snap.paused = false;
      lastSessionTick.QuadPart = 0;  // the paused gap isn't session time
      perf.Reset();
      QueryPerformanceCounter(&nextTick);
    }

    DWORD exitCode = 0;
    if (GetExitCodeProcess(hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
      Log("Game process exited (code %lu) -- polling stopped.", exitCode);
      snap.status = "Game closed -- polling stopped.";
      PublishSnapshot(snap);
      break;
    }

    // Player loaded / unloaded / rebuilt? Re-resolving runs AOB scans,
    // so while it keeps failing (mid-load) it's retried once a second.
    uintptr_t playerIns = ReadCurrentPlayerIns(hProcess, moduleBase, wcmCellOffset);
    if (!inGame || playerIns != resolved.entityAddress) {
      bool wasInGame = inGame;
      inGame = false;
      if ((playerIns != 0 || wcmCellOffset == 0) && GetTickCount64() - lastResolveAttempt >= 1000) {
        lastResolveAttempt = GetTickCount64();
        inGame = resolveAll() &&
                 ReadCurrentPlayerIns(hProcess, moduleBase, wcmCellOffset) == resolved.entityAddress;
        if (inGame) Log("Player loaded -- all addresses (re-)resolved.");
      }
      if (!inGame) {
        if (wasInGame) Log("Player unloaded (main menu or loading) -- reads paused.");
        snap.status = "Not in game (main menu or loading) -- waiting.";
        snap.haveValues = false;
        lastSessionTick.QuadPart = 0;  // menus and loading aren't session time
        PublishSnapshot(snap);
        WaitForSingleObject(g_window.wakeEvent, static_cast<DWORD>(kPollIntervalMs));
        QueryPerformanceCounter(&nextTick);
        continue;
      }
      snap.status.clear();
    }

    // The timed core reads -- same set `stats` reports latency for.
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    snap.hp = readI32(resolved.hpAddress);
    snap.maxHp = readI32(resolved.maxHpAddress);
    snap.fp = readI32(resolved.fpAddress);
    snap.maxFp = readI32(resolved.maxFpAddress);
    snap.stamina = readI32(resolved.staminaAddress);
    snap.maxStamina = readI32(resolved.maxStaminaAddress);
    snap.level = readI32(profile.levelAddress);
    snap.souls = readI32(profile.soulsAddress);
    QueryPerformanceCounter(&t1);
    snap.lastLatencyMicros =
        static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 / qpcFreq.QuadPart;
    perf.AddTick(snap.lastLatencyMicros);
    snap.lastReadTime = Timestamp();
    snap.pose = ReadPlayerPose(hProcess, resolved);
    {
      PlayerArea area = ReadPlayerArea(hProcess, moduleBase, resolved.entityAddress, gameManCellOffset);
      areaTracker.Update(area);
      if (area.ok) snap.mapNumber = static_cast<int>(area.mapId >> 24);
      snap.untendedWorld = areaTracker.lastRegion >= 400000 && areaTracker.lastRegion < 400100;
      snap.areaName = areaTracker.lastRegion ? areaNames.Region(areaTracker.lastRegion) : "";
      snap.lastBonfireName = area.ok ? areaNames.Bonfire(area.lastBonfireId) : "";
      snap.currentArea = areaInfo.AreaOf(areaTracker.lastRegion);
      GameCounters counters = ReadGameCounters(hProcess, moduleBase, profile.baseACellOffset);
      LARGE_INTEGER nowTick;
      QueryPerformanceCounter(&nowTick);
      double dt = lastSessionTick.QuadPart == 0
                      ? 0.0
                      : (std::min)(1.0, static_cast<double>(nowTick.QuadPart -
                                                             lastSessionTick.QuadPart) /
                                            qpcFreq.QuadPart);
      lastSessionTick = nowTick;
      SessionContext ctx;
      ctx.level = snap.level;
      ctx.souls = snap.souls;
      ctx.deathNum = counters.deathNum;
      ctx.playTimeMs = counters.playTime;
      ctx.area = snap.currentArea;
      ctx.characterName = ReadCharacterName(hProcess, profile.xBase);
      ctx.pose = snap.pose;
      ctx.lastBonfireId = area.lastBonfireId;
      ctx.lastBonfireName = snap.lastBonfireName;
      snap.characterName = ctx.characterName;
      if (counters.ok && tracker.Tick(dt, ctx)) {
        Log("%s", session.lastEvent.c_str());
      }
      snap.deathsTotal = counters.deathNum;
      snap.sessionSeconds = session.seconds;
      snap.sessionDeaths = session.deaths;
      snap.soulsLost = session.lost;
      snap.soulsRecovered = session.recovered;
      snap.soulsPerHour = session.SoulsPerHour();
      snap.areaSeconds = session.AreaSeconds(snap.currentArea);
      // Live page trail: a point per unit moved, plus deaths and rests.
      if (snap.pose.ok) {
        TrailPoint pt{snap.pose.x, snap.pose.y, snap.pose.z,
                      static_cast<int16_t>(snap.mapNumber * 2 + (snap.untendedWorld ? 1 : 0)), 0};
        float dx = pt.x - lastTrail.x, dy = pt.y - lastTrail.y, dz = pt.z - lastTrail.z;
        if (pt.mapKey != lastTrail.mapKey || dx * dx + dy * dy + dz * dz >= 1.0f) {
          TrailAdd(pt);
          lastTrail = pt;
        }
        if (lastTrailDeaths >= 0 && session.deaths > lastTrailDeaths) TrailAdd({pt.x, pt.y, pt.z, pt.mapKey, 1});
        lastTrailDeaths = session.deaths;
        if (area.ok && lastTrailBonfire != INT32_MIN && area.lastBonfireId != lastTrailBonfire)
          TrailAdd({pt.x, pt.y, pt.z, pt.mapKey, 2});
        if (area.ok) lastTrailBonfire = area.lastBonfireId;
      }
    }
    snap.nextLevelCost = RequiredSoulsForLevel(snap.level + 1);
    snap.haveValues = true;

    // Gear, level (Vitality -> max load), and active-slot changes: cheap
    // int32 reads every tick, heavier name/weight resolution only on change.
    bool gearChanged = snap.level != lastLevel;
    lastLevel = snap.level;
    for (size_t i = 0; i < slots.size(); ++i) {
      int32_t id = readI32(slots[i].address);
      if (id != lastSlotIds[i]) gearChanged = true;
      lastSlotIds[i] = id;
    }
    if (gearChanged) recomputeEquipLoad();

    int32_t rightIdx = readI32(profile.xBase + kRightHandSlotOffset);
    int32_t leftIdx = readI32(profile.xBase + kLeftHandSlotOffset);
    int32_t quickIdx = readI32(profile.xBase + kSelectedQuickItemSlotOffset);
    int32_t spellIdx = ReadActiveSpellIndex(hProcess, profile.xBase);
    if (gearChanged || rightIdx != lastHandIdx[0]) {
      std::string slot = "R" + std::to_string(rightIdx + 1);
      snap.rightHand = (rightIdx >= 0 && rightIdx <= 2) ? itemName(slotItemId(slot.c_str())) : "?";
    }
    if (gearChanged || leftIdx != lastHandIdx[1]) {
      std::string slot = "L" + std::to_string(leftIdx + 1);
      snap.leftHand = (leftIdx >= 0 && leftIdx <= 2) ? itemName(slotItemId(slot.c_str())) : "?";
    }
    if (gearChanged || quickIdx != lastHandIdx[2]) {
      int32_t id = (quickIdx >= 0 && quickIdx <= 9)
                       ? readI32(profile.xBase + kQuickItemArrayOffset +
                                 static_cast<uintptr_t>(quickIdx) * kQuickItemStride)
                       : -1;
      snap.quickItem = id > 0 ? itemName(id) : "(empty)";
    }
    // Spells can be re-attuned without a gear change, so re-resolve the
    // name on a timer too (once a second) rather than only on index change.
    if (gearChanged || spellIdx != lastSpellIdx || ticksSincePerfSample == 0) {
      int32_t magicId =
          (spellIdx >= 0 && spellIdx < 14) ? ReadSpellSlot(hProcess, profile.xBase, spellIdx + 1) : 0;
      std::string name = magicId > 0 ? LookupSpellName(ItemNames(), magicId) : "";
      snap.spell = magicId <= 0 ? "(none)" : (name.empty() ? "(name not found)" : name);
    }
    lastHandIdx[0] = rightIdx;
    lastHandIdx[1] = leftIdx;
    lastHandIdx[2] = quickIdx;
    lastSpellIdx = spellIdx;

    // Live page: attributes and the six weapon slots (name, attack rating,
    // next upgrade). AR reads several param rows, so only on a gear or
    // level change; which weapon is in hand is cheap and checked per tick.
    if (gearChanged) {
      const uintptr_t attrAddr[9] = {profile.vigorAddress,    profile.attunementAddress,
                                     profile.enduranceAddress, profile.vitalityAddress,
                                     profile.strengthAddress,  profile.dexterityAddress,
                                     profile.intelligenceAddress, profile.faithAddress,
                                     profile.luckAddress};
      for (int i = 0; i < 9; ++i) snap.attrs[i] = readI32(attrAddr[i]);
      snap.weapons.clear();
      for (const char* slotName : {"R1", "R2", "R3", "L1", "L2", "L3"}) {
        int32_t id = slotItemId(slotName);
        if (id < 0) continue;
        auto item = ResolveItem(hProcess, equipInventoryData, id);
        if (!item.ok || !item.plausible || item.giveId == 110000) continue;  // empty / Fists
        LiveWeapon w;
        w.slot = slotName;
        w.name = itemName(id);
        auto ar = ComputeWeaponAr(hProcess, moduleBase, item.giveId, snap.attrs[4], snap.attrs[5], snap.attrs[6],
                                  snap.attrs[7], snap.attrs[8]);
        if (ar.ok) {
          w.haveAr = true;
          w.catalyst = ar.isCatalyst;
          w.spellBuff = ar.spellBuff;
          for (int t = 0; t < 5 && t < kAecTypeCount; ++t) {
            w.present[t] = ar.types[t].present;
            w.base[t] = ar.types[t].base;
            w.bonus1h[t] = ar.types[t].bonus1H;
            w.bonus2h[t] = ar.types[t].bonus2H;
          }
        }
        // Level planner: the same AR with each offensive stat one higher.
        for (int k = 0; k < 5 && ar.ok; ++k) {
          int32_t a[5] = {snap.attrs[4], snap.attrs[5], snap.attrs[6], snap.attrs[7], snap.attrs[8]};
          a[k] += 1;
          auto up = ComputeWeaponAr(hProcess, moduleBase, item.giveId, a[0], a[1], a[2], a[3], a[4]);
          if (!up.ok) continue;
          for (int t = 0; t < kAecTypeCount; ++t) {
            if (!ar.types[t].present) continue;
            w.plan1h[k] += up.types[t].bonus1H - ar.types[t].bonus1H;
            w.plan2h[k] += up.types[t].bonus2H - ar.types[t].bonus2H;
            w.planFix[k] |= ar.types[t].penalized1H && !up.types[t].penalized1H;
          }
          w.planBuff[k] = up.spellBuff - ar.spellBuff;
        }
        // Every infusion of this weapon at the same level and current stats.
        // Rows: base + 100 x infusion (0 Normal ... 15 Hollow); weapons that
        // can't be infused have only the +0 row.
        {
          const int32_t level = item.giveId % 100, weaponBase = item.giveId - item.giveId % 10000;
          int rows = 0;
          for (int k = 0; k < 16; ++k) {
            w.inf1h[k] = w.inf2h[k] = -1;
            auto r = ComputeWeaponAr(hProcess, moduleBase, weaponBase + 100 * k + level, snap.attrs[4], snap.attrs[5],
                                     snap.attrs[6], snap.attrs[7], snap.attrs[8]);
            if (!r.ok) continue;
            ++rows;
            if (r.isCatalyst) {
              w.inf1h[k] = w.inf2h[k] = r.spellBuff;
              continue;
            }
            w.inf1h[k] = w.inf2h[k] = 0;
            for (int t = 0; t < kAecTypeCount; ++t) {
              if (!r.types[t].present) continue;
              w.inf1h[k] += r.types[t].base + r.types[t].bonus1H;
              w.inf2h[k] += r.types[t].base + r.types[t].bonus2H;
            }
          }
          w.infCurrent = rows > 1 ? (item.giveId % 10000) / 100 : -1;
        }
        w.upgrade = WindowUpgrade(hProcess, moduleBase, profile, id);
        snap.weapons.push_back(w);
      }
    }
    {
      int32_t sheath = readI32(profile.xBase + kWeaponSheathStateOffset);
      for (auto& w : snap.weapons) {
        bool right = w.slot[0] == 'R';
        int idx = w.slot[1] - '1';
        w.active = right ? idx == rightIdx : idx == leftIdx;
        w.twoHanded = w.active && ((right && sheath == 3) || (!right && sheath == 2));
      }
    }

    // World pickups found in this map section: once a second (~30-40
    // flag reads). The pickup list itself is cached per map section.
    bool tierChanged = SpoilerTier() != lastSpoilerTier;
    lastSpoilerTier = SpoilerTier();
    if (ticksSincePerfSample == 0 || tierChanged) {
      PlayerArea a = ReadPlayerArea(hProcess, moduleBase, resolved.entityAddress, 0);
      if (a.ok) {
        // Per named area (pickup positions + play regions); falls back to
        // the map section when the area has no positioned pickups.
        int total = 0;
        int found = CountFoundInArea(hProcess, moduleBase, moduleSize, profile.baseACellOffset,
                                     snap.currentArea, &total);
        snap.itemsScope = found >= 0 ? "in " + snap.currentArea : "in this map section";
        if (found < 0)
          found = CountFoundPickups(hProcess, moduleBase, moduleSize, profile.baseACellOffset,
                                    static_cast<int>(a.mapId >> 24), &total);
        snap.itemsFound = found;
        snap.itemsTotal = total;
      }
      WindowProgress(hProcess, moduleBase, moduleSize, profile.baseACellOffset, snap.currentArea,
                     &snap.bossesHere, &snap.bossesDefeated, &snap.bossesTotal, &snap.bossesLater, &snap.shards);
      if (snap.pose.ok) {
        PlayerArea where = ReadPlayerArea(hProcess, moduleBase, resolved.entityAddress, 0);
        where.playRegionId = areaTracker.lastRegion;  // keep the last region between regions
        WindowNearest(hProcess, moduleBase, moduleSize, profile.baseACellOffset, snap.pose, where,
                      &snap.nearest, &snap.nearestShort);
        LiveNearbyList(hProcess, moduleBase, moduleSize, profile.baseACellOffset, snap.pose, where,
                       snap.currentArea, &snap.nearby);
      }
      LiveBossList(hProcess, moduleBase, moduleSize, snap.currentArea, snap.pose, &snap.bosses, snap.bossCounts);
      LiveRoute(hProcess, moduleBase, moduleSize, profile.baseACellOffset, snap.characterName, snap.currentArea,
                &snap.route, &snap.routeHint, &snap.routeHidden);
      LiveMissables(hProcess, moduleBase, moduleSize, profile.baseACellOffset, snap.characterName, snap.currentArea,
                    &snap.missables, &snap.missableAlert, &snap.missablesHidden);
      BuildLevelPlan(hProcess, moduleBase, &snap);
      snap.bossHint = WindowBossHint(hProcess, moduleBase, moduleSize, snap.currentArea, snap.pose);
      // Key items walk the inventory, so every 5s rather than every second.
      if (++keyItemTicks % 5 == 1) {
        snap.keyItems = WindowKeyItems(hProcess, moduleBase, moduleSize, profile);
        {
          // Infusion gems held: Heavy Gem 1100, Sharp 1110 ... Hollow 1240 (goods).
          auto held = HeldGoodsQty(hProcess, profile);
          snap.gemHeld[0] = true;
          for (int k = 1; k < 16; ++k) snap.gemHeld[k] = held.count(1100 + (k - 1) * 10) > 0;
        }
        int32_t rIdx = readI32(profile.xBase + kRightHandSlotOffset);
        std::string slot = "R" + std::to_string(rIdx + 1);
        int32_t invId = (rIdx >= 0 && rIdx <= 2) ? slotItemId(slot.c_str()) : -1;
        snap.upgrade = invId >= 0 ? WindowUpgrade(hProcess, moduleBase, profile, invId) : "";
      }
    }
    // Perf figures: refreshed once a second over a rolling 5s window.
    if (++ticksSincePerfSample >= 10) {
      ticksSincePerfSample = 0;
      snap.perf = perf.Take();
      if (perf.ElapsedSeconds() >= kPerfReportIntervalSeconds) {
        if (snap.perf.valid) tracker.log.WritePerf(PerfCsvRow(snap.perf, tracker.stats.seconds));
        perf.Reset();
      }
    }
    PublishSnapshot(snap);

    // Fixed-deadline 10Hz, same as stats --live; the wait wakes early on
    // a pause toggle or shutdown.
    nextTick.QuadPart += tickInterval;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (now.QuadPart - nextTick.QuadPart > tickInterval) nextTick = now;
    LONGLONG waitTicks = nextTick.QuadPart - now.QuadPart;
    DWORD waitMs = waitTicks > 0 ? static_cast<DWORD>(waitTicks * 1000 / qpcFreq.QuadPart) : 0;
    WaitForSingleObject(g_window.wakeEvent, waitMs);
  }
  tracker.Finish();
  CloseHandle(hProcess);
  if (session.seconds > 0) {
    Log("Session: %s, %d death(s), %.0f souls/h, lost %lld, recovered %lld.", FormatDuration(session.seconds).c_str(),
        session.deaths, session.SoulsPerHour(), static_cast<long long>(session.lost),
        static_cast<long long>(session.recovered));
  }
}

// Waits for the game, attaches, polls until it closes, then waits again --
// so the overlay and live page can be started before the game and keep
// running across game restarts. While waiting it only lists processes
// (every 2s); it attaches through AttachReadOnly, which still refuses any
// DS3 without the Seamless Co-op module (never vanilla online).
DWORD WINAPI WindowPollWorker(LPVOID) {
  bool announced = false;
  DWORD refusedPid = 0;
  while (!g_window.stop) {
    DWORD pid = FindProcessIdByName(kTargetProcessName);
    bool coop = pid != 0 && HasModule(ListModuleNames(pid), kSeamlessCoopModuleMarker);
    if (pid != 0 && !coop && pid != refusedPid) {
      Log("Found Dark Souls III (PID %lu) without Seamless Co-op loaded -- not attaching (vanilla online is "
          "never touched); waiting.", pid);
      refusedPid = pid;
    }
    HANDLE hProcess = nullptr;
    if (coop) {
      DWORD attachedPid = 0;
      hProcess = AttachReadOnly(&attachedPid);
      if (hProcess) pid = attachedPid;
    }
    if (!hProcess) {
      if (!announced) Log("Waiting for Dark Souls III (Seamless Co-op) to start...");
      announced = true;
      WindowSnapshot waiting;
      waiting.status = "Waiting for Dark Souls III (Seamless Co-op) to start...";
      PublishSnapshot(waiting);
      WaitForSingleObject(g_window.wakeEvent, 2000);
      continue;
    }
    announced = false;
    refusedPid = 0;
    PollGameRun(hProcess, pid);
  }
  return 0;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

struct WindowTheme {
  COLORREF background = RGB(22, 22, 26);
  COLORREF text = RGB(225, 225, 230);
  COLORREF dim = RGB(140, 140, 152);
  COLORREF barTrack = RGB(48, 48, 56);
  COLORREF hp = RGB(178, 42, 42);
  COLORREF fp = RGB(52, 96, 196);
  COLORREF stamina = RGB(58, 150, 72);
  COLORREF paused = RGB(214, 150, 40);
  COLORREF warn = RGB(220, 90, 70);
};

// ---- overlay (Milestone 4) -------------------------------------------
//
// A compact HUD panel drawn over the top-right of the game's client area
// (empty in DS3's own HUD), tracking the game window as it moves or
// resizes. Requires the game windowed or borderless: in exclusive
// fullscreen no other window can appear over it (see docs/TECHNICAL.md).
//
//   - Click-through and never focused: WS_EX_LAYERED | WS_EX_TRANSPARENT
//     | WS_EX_NOACTIVATE, so mouse and keyboard always reach the game.
//   - Panel-sized, not a transparent sheet over the whole game: DWM only
//     composites a few hundred pixels square at 10Hz. Rounded corners
//     use a colour key; the panel itself is drawn at the opacity setting.
//   - Tracking is event-driven: an out-of-context WinEvent hook (no
//     injection -- events are delivered to this thread's message loop)
//     for the game window's location changes and for foreground
//     changes, plus a cheap recheck on every snapshot as a fallback.
//   - Shown only while the game is the foreground window and not
//     minimized, so it doesn't float over other apps after alt-tab.
//   - F10 hides it AND pauses polling (brief: "same hotkey toggles
//     overlay visibility + polling together"); F11 toggles the perf
//     figures, hidden by default for actual play sessions.
constexpr wchar_t kOverlayClassName[] = L"WasdOverlay";
constexpr COLORREF kOverlayColorKey = RGB(255, 0, 255);  // fully transparent
constexpr double kOverlayMargin = 16.0;                   // from the game's client edge
// Opacity, width, text size and corner are live settings (g_ov).

struct OverlayState {
  HWND hwnd = nullptr;
  HWND game = nullptr;
  DWORD gamePid = 0;
  HWINEVENTHOOK foregroundHook = nullptr, locationHook = nullptr;
  int lineCount = 1;      // of the current content (the panel's height follows it; 0 = hidden)
  int fontPx = 0;         // size g_window.font was made at
  int charsPerLine = 38;  // measured from the font at start
};
OverlayState g_overlay;

int OverlayLineCount() { return g_overlay.lineCount; }

// Wraps at " | " breaks first (the boss hint's sections), then at spaces.
std::vector<std::wstring> WrapOverlayText(const std::wstring& text, size_t maxChars) {
  std::vector<std::wstring> segments;
  for (size_t pos = 0;;) {
    size_t sep = text.find(L" | ", pos);
    segments.push_back(text.substr(pos, sep == std::wstring::npos ? std::wstring::npos : sep - pos));
    if (sep == std::wstring::npos) break;
    pos = sep + 3;
  }
  std::vector<std::wstring> lines;
  for (const auto& seg : segments) {
    if (!lines.empty() && lines.back().size() + 3 + seg.size() <= maxChars) {
      lines.back() += L" | " + seg;
      continue;
    }
    std::wstring rest = seg;
    while (rest.size() > maxChars) {
      size_t cut = rest.rfind(L' ', maxChars);
      if (cut == std::wstring::npos || cut == 0) cut = maxChars;
      lines.push_back(rest.substr(0, cut));
      rest = rest.substr(cut + (rest[cut] == L' ' ? 1 : 0));
    }
    lines.push_back(rest);
  }
  return lines;
}

struct OverlayLine {
  std::wstring text;
  COLORREF color;
};

// What the overlay shows -- only what you'd act on mid-fight (user's
// choice, 2026-10-06): the spoiler tier with the nearest unfound item,
// bosses done / total here, and the nearest boss's weaknesses and
// resistances as far as the tier allows. Everything else is on the live
// page.
std::vector<OverlayLine> OverlayLines(const WindowSnapshot& snap, const WindowTheme& theme) {
  std::vector<OverlayLine> out;
  const size_t width = static_cast<size_t>((std::max)(16, g_overlay.charsPerLine));
  auto add = [&](const std::string& text, COLORREF color) {
    for (const auto& l : WrapOverlayText(Widen(text), width)) out.push_back({l, color});
  };
  if (!snap.status.empty() && !snap.haveValues) {
    add(snap.status, theme.warn);
  } else {
    const int tier = SpoilerTier();
    if (g_ov.nearest) {
      // Vague: only how many are left -- no direction or distance, like the page.
      std::string nearText = tier == kTierOff ? "hints off (F9)"
                         : tier == kTierVague
                             ? (snap.itemsFound >= 0 ? std::to_string(snap.itemsTotal - snap.itemsFound) +
                                                           " unfound items here"
                                                     : std::string("--"))
                             : snap.nearestShort.empty() ? std::string("--") : snap.nearestShort;
      add("[" + std::string(SpoilerTierName(tier)) + "] " + nearText, theme.text);
    }
    if (g_ov.bosses)
      add((snap.bossesTotal > 0 ? "Bosses " + std::to_string(snap.bossesDefeated) + " / " +
                                      std::to_string(snap.bossesTotal) + " defeated here"
                                : std::string(snap.bossesLater ? "No bosses here yet" : "No bosses here")) +
              (snap.bossesLater ? " (+" + std::to_string(snap.bossesLater) + " later)" : ""),
          theme.dim);
    if (g_ov.bossHint && !snap.bossHint.empty()) add(snap.bossHint, theme.text);
    if (g_ov.missable && !snap.missableAlert.empty()) add("! " + snap.missableAlert, theme.warn);
    if (g_ov.route && !snap.routeHint.empty()) add("Route: " + snap.routeHint, theme.dim);
  }
  if (g_ov.perf) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "read %s  %.1f us  %.2f Hz", snap.lastReadTime.c_str(), snap.lastLatencyMicros,
                  snap.perf.valid ? snap.perf.rateHz : 0.0);
    add(buf, theme.dim);
    std::snprintf(buf, sizeof(buf), "CPU %.2f%%   mem %.1f MB", snap.perf.valid ? snap.perf.cpuPercentOfCore : 0.0,
                  snap.perf.valid ? snap.perf.workingSetMB : 0.0);
    add(buf, theme.dim);
  }
  return out;
}

double OverlayLineHeight() { return g_ov.text * 4.0 / 3.0; }  // 20 px at the default 15

int OverlayHeightPx() {
  const double scale = g_window.scale;
  return static_cast<int>((14.0 + OverlayLineCount() * OverlayLineHeight() + 10.0) * scale + 0.5);
}

struct FindGameWindowCtx {
  DWORD pid;
  HWND best;
  LONG bestArea;
};

BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM param) {
  auto* ctx = reinterpret_cast<FindGameWindowCtx*>(param);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != ctx->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr)
    return TRUE;
  RECT r;
  GetClientRect(hwnd, &r);
  LONG area = r.right * r.bottom;
  if (area > ctx->bestArea) {
    ctx->best = hwnd;
    ctx->bestArea = area;
  }
  return TRUE;
}

// The game's main window: its largest visible, unowned top-level window.
HWND FindGameWindow(DWORD pid) {
  FindGameWindowCtx ctx{pid, nullptr, 0};
  EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&ctx));
  return ctx.best;
}

void CALLBACK OnOverlayWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG,
                                DWORD, DWORD);
bool OverlayPreviewActive();

// Show/hide/move the overlay to match the game window. Cheap enough to
// call on every event and every snapshot.
void RepositionOverlay() {
  if (!g_overlay.hwnd) return;
  if (g_overlay.gamePid != 0 && (!g_overlay.game || !IsWindow(g_overlay.game))) {
    g_overlay.game = FindGameWindow(g_overlay.gamePid);
  }
  // Normally only while the game is focused; while the live page's overlay
  // settings are open (preview), also when it isn't, so changes show live.
  bool visible = g_overlay.game && !g_window.paused && g_ov.visible && g_overlay.lineCount > 0 &&
                 !IsIconic(g_overlay.game) &&
                 (GetForegroundWindow() == g_overlay.game || OverlayPreviewActive());
  if (!visible) {
    if (IsWindowVisible(g_overlay.hwnd)) ShowWindow(g_overlay.hwnd, SW_HIDE);
    return;
  }
  RECT client;
  GetClientRect(g_overlay.game, &client);
  POINT topLeft{client.left, client.top}, bottomRight{client.right, client.bottom};
  ClientToScreen(g_overlay.game, &topLeft);
  ClientToScreen(g_overlay.game, &bottomRight);
  const double scale = g_window.scale;
  int w = static_cast<int>(g_ov.width * scale + 0.5);
  int h = OverlayHeightPx();
  int margin = static_cast<int>(kOverlayMargin * scale + 0.5);
  bool left = g_ov.corner == 1 || g_ov.corner == 3, bottom = g_ov.corner >= 2;
  int x = left ? topLeft.x + margin : bottomRight.x - w - margin;
  int y = bottom ? bottomRight.y - h - margin : topLeft.y + margin;
  RECT want{x, y, x + w, y + h};
  RECT have;
  GetWindowRect(g_overlay.hwnd, &have);
  if (IsWindowVisible(g_overlay.hwnd) && EqualRect(&want, &have)) return;  // nothing to do
  SetWindowPos(g_overlay.hwnd, HWND_TOPMOST, want.left, want.top, w, h,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void CALLBACK OnOverlayWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG,
                                DWORD, DWORD) {
  if (event == EVENT_OBJECT_LOCATIONCHANGE && (hwnd != g_overlay.game || idObject != OBJID_WINDOW))
    return;  // the game also reports cursor/caret moves; only its window matters
  RepositionOverlay();
}

// Location changes are hooked per game process, so they can only be
// installed once the worker has attached and published a PID.
void EnsureOverlayGameHooks(DWORD pid) {
  if (pid == 0 || g_overlay.gamePid == pid) return;
  g_overlay.gamePid = pid;
  g_overlay.game = FindGameWindow(pid);
  if (g_overlay.locationHook) UnhookWinEvent(g_overlay.locationHook);
  g_overlay.locationHook =
      SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                      OnOverlayWinEvent, pid, 0, WINEVENT_OUTOFCONTEXT);
  if (!g_overlay.game) Log("Overlay: game window not found yet -- will keep looking.");
}

void PaintOverlay(HWND hwnd, HDC target) {
  WindowSnapshot snap;
  EnterCriticalSection(&g_window.lock);
  snap = g_window.snapshot;
  LeaveCriticalSection(&g_window.lock);

  RECT client;
  GetClientRect(hwnd, &client);
  int width = client.right, height = client.bottom;
  HDC dc = CreateCompatibleDC(target);
  HBITMAP bmp = CreateCompatibleBitmap(target, width, height);
  HGDIOBJ oldBmp = SelectObject(dc, bmp);
  const WindowTheme theme;
  const double scale = g_window.scale;
  auto px = [&](double v) { return static_cast<int>(v * scale + 0.5); };

  // Colour-keyed background, then a rounded dark panel.
  HBRUSH key = CreateSolidBrush(kOverlayColorKey);
  FillRect(dc, &client, key);
  DeleteObject(key);
  HBRUSH panel = CreateSolidBrush(theme.background);
  HPEN border = CreatePen(PS_SOLID, 1, theme.barTrack);
  HGDIOBJ oldBrush = SelectObject(dc, panel);
  HGDIOBJ oldPen = SelectObject(dc, border);
  RoundRect(dc, 0, 0, width, height, px(10), px(10));
  SelectObject(dc, oldBrush);
  SelectObject(dc, oldPen);
  DeleteObject(panel);
  DeleteObject(border);

  HGDIOBJ oldFont = SelectObject(dc, g_window.font);
  SetBkMode(dc, TRANSPARENT);
  const int margin = px(12), lineH = px(OverlayLineHeight());
  int y = px(10);
  auto line = [&](const std::wstring& s, COLORREF color) {
    SetTextColor(dc, color);
    TextOutW(dc, margin, y, s.c_str(), static_cast<int>(s.size()));
    y += lineH;
  };
  for (const auto& l : OverlayLines(snap, theme)) line(l.text, l.color);

  BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
  SelectObject(dc, oldFont);
  SelectObject(dc, oldBmp);
  DeleteObject(bmp);
  DeleteDC(dc);
}

LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_NCHITTEST:
      return HTTRANSPARENT;  // belt and braces alongside WS_EX_TRANSPARENT
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      PaintOverlay(hwnd, dc);
      EndPaint(hwnd, &ps);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Everything the overlay draws, as one string: it only repaints (and
// resizes) when this changes -- most ticks, nothing does.
std::string OverlayContentKey(const WindowSnapshot& snap) {
  std::string key = snap.status + "|" + (snap.haveValues ? "1" : "0") + "|" + std::to_string(SpoilerTier()) + "|" +
                    snap.nearestShort + "|" + std::to_string(snap.bossesDefeated) + "/" +
                    std::to_string(snap.bossesTotal) + "+" + std::to_string(snap.bossesLater) + "|" + snap.bossHint + "|" + snap.routeHint + "|" + snap.missableAlert + "|" +
                    std::to_string(snap.itemsFound) +
                    "/" + std::to_string(snap.itemsTotal);
  for (const auto& d : kOverlaySettingDefs) key += "|" + std::to_string(*d.value);
  if (g_ov.perf) key += snap.lastReadTime;  // perf lines change every tick
  return key;
}
std::string g_lastOverlayContentKey;

// Re-lays out the overlay from the latest snapshot: line count (height),
// position, repaint -- only when its content changed (or force).
void RefreshOverlay(bool force) {
  if (!g_overlay.hwnd) return;
  EnterCriticalSection(&g_window.lock);
  DWORD pid = g_window.snapshot.pid;
  std::string key = OverlayContentKey(g_window.snapshot);
  int lines = static_cast<int>(OverlayLines(g_window.snapshot, WindowTheme()).size());
  LeaveCriticalSection(&g_window.lock);
  EnsureOverlayGameHooks(pid);
  g_overlay.lineCount = lines;  // 0 (every line switched off) hides the panel
  RepositionOverlay();
  if (force || key != g_lastOverlayContentKey) {
    g_lastOverlayContentKey = key;
    InvalidateRect(g_overlay.hwnd, nullptr, FALSE);
  }
}

// Tier set from the live page (POST /api/tier): page server -> host window.
constexpr UINT kMsgSpoilerTierSet = WM_APP + 5;
// Overlay settings changed from the live page (POST /api/overlay).
constexpr UINT kMsgOverlaySettings = WM_APP + 6;

// Font size, wrap length and opacity from the settings (UI thread).
void ApplyOverlayLook() {
  const double scale = g_window.scale;
  if (g_overlay.fontPx != g_ov.text || !g_window.font) {
    if (g_window.font) DeleteObject(g_window.font);
    g_window.font = CreateFontW(-static_cast<int>(g_ov.text * scale + 0.5), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                FIXED_PITCH | FF_MODERN, L"Consolas");
    g_overlay.fontPx = g_ov.text;
  }
  // Consolas is fixed-pitch: one character's width sets the wrap length.
  HDC dc = GetDC(nullptr);
  HGDIOBJ old = SelectObject(dc, g_window.font);
  SIZE em{};
  GetTextExtentPoint32W(dc, L"M", 1, &em);
  SelectObject(dc, old);
  ReleaseDC(nullptr, dc);
  if (em.cx > 0) g_overlay.charsPerLine = static_cast<int>((g_ov.width - 24.0) * scale / em.cx);
  if (g_overlay.hwnd)
    SetLayeredWindowAttributes(g_overlay.hwnd, kOverlayColorKey, static_cast<BYTE>(g_ov.opacity * 255 / 100),
                               LWA_COLORKEY | LWA_ALPHA);
}

// Messages for the hidden host window (hotkeys, snapshots, the page).
void RefreshMainWindow();  // the app window (WASD.exe), defined after the map-data section

bool HandleSharedMessage(HWND, UINT msg) {
  switch (msg) {
    case kMsgSnapshot:
      RefreshOverlay(false);
      RefreshMainWindow();
      return true;
    case kMsgTogglePause:
      InterlockedExchange(&g_window.paused, g_window.paused ? 0 : 1);
      SetEvent(g_window.wakeEvent);
      RepositionOverlay();  // hides it while paused
      return true;
    case kMsgCycleSpoilerTier:
      Log("Spoiler tier: %s (F9).", SpoilerTierName(CycleSpoilerTier()));
      SetEvent(g_window.wakeEvent);  // worker refreshes the hint lines now
      RefreshOverlay(true);
      return true;
    case kMsgSpoilerTierSet:
      Log("Spoiler tier: %s (live page).", SpoilerTierName(SpoilerTier()));
      SetEvent(g_window.wakeEvent);
      RefreshOverlay(true);
      return true;
    case kMsgTogglePerfHud:
      InterlockedExchange(&g_ov.perf, g_ov.perf ? 0 : 1);
      SaveSettings();
      Log("Overlay perf figures %s (F11).", g_ov.perf ? "shown" : "hidden");
      RefreshOverlay(true);  // height changes
      return true;
    case kMsgOverlaySettings:
      ApplyOverlayLook();
      RefreshOverlay(true);
      return true;
  }
  return false;
}

bool StartLiveServer(int* portOut);
void StopLiveServer();
bool LivePageRecentlyPolled();
wchar_t g_liveUrl[64] = L"";
constexpr UINT_PTR kOpenPageTimerId = 1;

// The overlay's owner is a hidden message target (there's no visible
// window to close); Ctrl+C reaches it via the console handler (WM_CLOSE
// posted to it).
constexpr UINT kMsgMapData = WM_APP + 7;       // map-data thread -> host window: finished
constexpr UINT kMsgShowYourself = WM_APP + 8;  // a second WASD.exe -> the app window: come to the front

LRESULT CALLBACK OverlayHostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (HandleSharedMessage(hwnd, msg)) return 0;
  if (msg == kMsgMapData) {
    RefreshMainWindow();
    return 0;
  }
  if (msg == WM_TIMER && wParam == kOpenPageTimerId) {
    KillTimer(hwnd, kOpenPageTimerId);
    if (EnvVar(L"WASD_NO_BROWSER") == L"1") {  // tests
      Log("Not opening the live page (WASD_NO_BROWSER).");
    } else if (LivePageRecentlyPolled()) {
      Log("Live page already open in a browser -- not opening another tab.");
    } else {
      Log("Opening the live page in your browser.");
      ShellExecuteW(nullptr, L"open", g_liveUrl, nullptr, nullptr, SW_SHOWNOACTIVATE);
    }
    return 0;
  }
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

BOOL WINAPI OnWindowModeConsoleCtrl(DWORD ctrlType) {
  if (ctrlType != CTRL_C_EVENT && ctrlType != CTRL_BREAK_EVENT && ctrlType != CTRL_CLOSE_EVENT)
    return FALSE;
  PostMessageW(g_window.hwnd, WM_CLOSE, 0, 0);
  return TRUE;
}


// The Milestone 4 HUD over the game plus the live page server, sharing
// one polling worker and one hotkey hook. (The Milestone 3 companion
// window was replaced by the live page, 2026-10-06.)
void EnsureMapDataAtStart();  // defined with the map-data section
void CreateMainWindow(HINSTANCE instance);
void StartFirstRunMapData();
HWND g_mainWindow = nullptr;  // the app window (WASD.exe only)

// app: WASD.exe -- adds the app window, reads first-run map data in the
// background instead of before starting, and has no console to handle.
int RunOverlay(bool app = false) {
  if (!app) EnsureMapDataAtStart();  // first run: read the map data
  SetProcessDPIAware();
  InitializeCriticalSection(&g_window.lock);
  g_window.wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HINSTANCE instance = GetModuleHandleW(nullptr);

  HDC screen = GetDC(nullptr);
  double scale = GetDeviceCaps(screen, LOGPIXELSY) / 96.0;
  ReleaseDC(nullptr, screen);
  g_window.scale = scale;
  ApplyOverlayLook();  // font + wrap length from the saved settings
  InitializeCriticalSection(&g_trail.lock);
  {
    WindowSnapshot starting;
    starting.status = "Attaching to Dark Souls III...";
    g_window.snapshot = starting;
  }

  {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = OverlayHostProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"WasdOverlayHost";
    RegisterClassExW(&wc);
    g_window.hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                    nullptr, instance, nullptr);
  }
  if (!g_window.hwnd) {
    Log("Couldn't create the window (error %lu).", GetLastError());
    return 4;
  }

  {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = OverlayWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kOverlayClassName;
    RegisterClassExW(&wc);
    g_overlay.hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kOverlayClassName, L"WASD overlay", WS_POPUP, 0, 0,
        static_cast<int>(g_ov.width * scale + 0.5), OverlayHeightPx(), nullptr, nullptr,
        instance, nullptr);
    if (!g_overlay.hwnd) {
      Log("Couldn't create the overlay (error %lu).", GetLastError());
      return 4;
    }
    ApplyOverlayLook();  // opacity, now that the window exists
    g_overlay.foregroundHook =
        SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, nullptr,
                        OnOverlayWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_perfHudHotkeyEnabled = true;
  }
  if (app) {
    CreateMainWindow(instance);
    if (!g_mainWindow) {
      Log("Couldn't create the app window (error %lu).", GetLastError());
      return 4;
    }
    StartFirstRunMapData();
  } else {
    SetConsoleCtrlHandler(OnWindowModeConsoleCtrl, TRUE);
  }

  // F10 (and F11 in overlay mode) hook, delivering to the target window.
  g_hotkeyTargetWindow = g_window.hwnd;
  HotkeyThreadState hotkeyState;
  hotkeyState.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  DWORD hotkeyThreadId = 0;
  HANDLE hotkeyThread =
      CreateThread(nullptr, 0, HotkeyThreadMain, &hotkeyState, 0, &hotkeyThreadId);
  if (hotkeyThread) WaitForSingleObject(hotkeyState.ready, INFINITE);
  CloseHandle(hotkeyState.ready);
  if (!hotkeyThread || !hotkeyState.hooked) {
    Log("WARNING: couldn't install the keyboard hook -- no F10/F11 this run.");
  }

  HANDLE worker = CreateThread(nullptr, 0, WindowPollWorker, nullptr, 0, nullptr);
  Log("%s %s", WASD_APP_NAME, WASD_VERSION_STRING);
  Log("Your data: %ls (%ls)", Paths().user.c_str(), Paths().userSource.c_str());
  Log("Overlay running over the game window's top-right (game must be windowed or borderless). "
      "F10 hides it + pauses polling, F11 shows/hides perf figures, F9 cycles the spoiler tier. %s",
      app ? "Close the WASD window to exit." : "Ctrl+C to exit.");
  int port = 0;
  if (StartLiveServer(&port)) {
    std::swprintf(g_liveUrl, 64, L"http://localhost:%d/", port);
    Log("Live page: %ls (this PC only).", g_liveUrl);
    RefreshMainWindow();
    // A page left open from an earlier run reconnects within a second or so
    // (Chrome slows timers in background tabs to once a second);
    // only open a new tab if none has asked for data by then.
    SetTimer(g_window.hwnd, kOpenPageTimerId, 2500, nullptr);
  }

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  InterlockedExchange(&g_window.stop, 1);
  SetEvent(g_window.wakeEvent);
  if (worker) {
    WaitForSingleObject(worker, 3000);
    CloseHandle(worker);
  }
  if (hotkeyThread) {
    PostThreadMessageW(hotkeyThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(hotkeyThread, 2000);
    CloseHandle(hotkeyThread);
  }
  StopLiveServer();
  if (g_overlay.foregroundHook) UnhookWinEvent(g_overlay.foregroundHook);
  if (g_overlay.locationHook) UnhookWinEvent(g_overlay.locationHook);
  if (g_overlay.hwnd) DestroyWindow(g_overlay.hwnd);
  if (g_mainWindow) DestroyWindow(g_mainWindow);
  g_mainWindow = nullptr;
  CloseHandle(g_window.wakeEvent);
  DeleteObject(g_window.font);
  // (Each game run logs its own session summary when it ends.)
  DeleteCriticalSection(&g_window.lock);
  DeleteCriticalSection(&g_trail.lock);
  Log("Closed.");
  return 0;
}


// ---- report mode: results page from saved sessions -----
//
// Collects every sessions/<character>/*.jsonl, injects them into
// templates/results_template.html in place of __SESSIONS_JSON__ as
// [{"file": ..., "events": [<each line>]}, ...], writes
// reports/results.html and opens it in the default browser. The page is
// self-contained (inline JS/SVG, no network). Lines that aren't a whole
// {...} object (e.g. a line cut off by a crash mid-write) are skipped.
std::string ReadWholeFile(const std::wstring& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// ---- map data: finding the game, running the extractor
//
// treasures.tsv, enemies.tsv and the name tables (item, bonfire and place
// names) come from the user's own DS3 install, through
// tools\extract_treasures.py: with the system Python in a dev checkout, with
// the bundled embeddable Python (python\) in an install.
// `wasd-cli.exe extract [--game <folder>]` runs it; `overlay` runs it before
// starting when a table is missing or older than the game's files (the game
// was patched): kGeneratedTables, kGameSourceFiles.

// Steam's steamapps\libraryfolders.vdf, a KeyValues text file. Minimal
// reader: quoted strings (with \\ \" escapes), braces, // comments.
namespace vdf {
struct Node {
  bool object = false;
  std::string text;
  std::vector<std::pair<std::string, Node>> kids;
  const Node* Get(const std::string& key) const {
    for (const auto& kv : kids)
      if (_stricmp(kv.first.c_str(), key.c_str()) == 0) return &kv.second;
    return nullptr;
  }
};

struct Reader {
  const std::string& s;
  size_t i = 0;
  bool ok = true;
  // '{', '}', '"' (a string, in *tok) or 0 at the end.
  char Next(std::string* tok) {
    for (;;) {
      while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
      if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
        while (i < s.size() && s[i] != '\n') ++i;
        continue;
      }
      break;
    }
    if (i >= s.size()) return 0;
    char c = s[i++];
    if (c == '{' || c == '}') return c;
    tok->clear();
    if (c != '"') {  // unquoted token
      --i;
      while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '{' && s[i] != '}') *tok += s[i++];
      return '"';
    }
    while (i < s.size() && s[i] != '"') {
      if (s[i] == '\\' && i + 1 < s.size()) {
        char e = s[++i];
        *tok += e == 'n' ? '\n' : e == 't' ? '\t' : e;  // \\ and \" (and anything else) as the character
        ++i;
      } else {
        *tok += s[i++];
      }
    }
    if (i >= s.size()) ok = false;  // unterminated string
    ++i;
    return '"';
  }
  void ReadPairs(Node* into, bool braced) {
    into->object = true;
    for (;;) {
      std::string key, value;
      char t = Next(&key);
      if (t == 0) {
        if (braced) ok = false;
        return;
      }
      if (t == '}') {
        if (!braced) ok = false;
        return;
      }
      if (t != '"') { ok = false; return; }
      char u = Next(&value);
      if (u == '{') {
        Node child;
        ReadPairs(&child, true);
        into->kids.emplace_back(key, std::move(child));
        if (!ok) return;
      } else if (u == '"') {
        Node leaf;
        leaf.text = value;
        into->kids.emplace_back(key, std::move(leaf));
      } else {
        ok = false;
        return;
      }
    }
  }
};
}  // namespace vdf

struct SteamLibrary {
  std::wstring path;
  std::vector<std::string> apps;  // installed app ids; empty in the pre-2021 format
};

// Libraries listed in libraryfolders.vdf. Current format: "N" { "path" ...
// "apps" { "<id>" ... } }; pre-2021: "N" "<path>". Broken file: empty.
std::vector<SteamLibrary> ParseLibraryFolders(const std::string& text) {
  std::vector<SteamLibrary> out;
  vdf::Node root;
  vdf::Reader r{text};
  r.ReadPairs(&root, false);
  const vdf::Node* lf = root.Get("libraryfolders");
  if (!r.ok || !lf || !lf->object) return out;
  for (const auto& [key, node] : lf->kids) {
    if (key.empty() || key.find_first_not_of("0123456789") != std::string::npos) continue;
    SteamLibrary lib;
    if (node.object) {
      const vdf::Node* path = node.Get("path");
      if (!path || path->object) continue;
      lib.path = WidenUtf8(path->text);
      if (const vdf::Node* apps = node.Get("apps"))
        for (const auto& app : apps->kids) lib.apps.push_back(app.first);
    } else {
      lib.path = WidenUtf8(node.text);
    }
    if (!lib.path.empty()) out.push_back(std::move(lib));
  }
  return out;
}

// Backslashes, no trailing separator (Steam's registry value uses "c:/...").
std::wstring NormalizeDir(std::wstring d) {
  for (auto& c : d)
    if (c == L'/') c = L'\\';
  while (d.size() > 3 && d.back() == L'\\') d.pop_back();
  return d;
}

constexpr char kDs3SteamAppId[] = "374320";

struct GameDirSearch {
  std::wstring explicitDir;     // --game
  std::wstring runningGameExe;  // the running DS3's exe ("" when not running, or not Seamless Co-op)
  std::wstring savedDir;        // game_dir from settings.ini
  std::wstring steamPath;       // HKCU\Software\Valve\Steam\SteamPath ("" without Steam)
  std::function<bool(const std::wstring&)> exists;
  std::function<std::string(const std::wstring&)> readFile;
};
struct GameDirFound {
  std::wstring dir;  // "" = not found
  std::string source;
  std::vector<std::wstring> tried;  // every folder looked at, for the "not found" message
};

// Where the DS3 "Game" folder (the one holding Data5.bhd) is, in this
// order: --game (no fallback when given), the running game, the saved
// folder, then every Steam library (those listing app 374320 first).
GameDirFound FindDs3GameDir(const GameDirSearch& s) {
  GameDirFound f;
  auto check = [&](std::wstring dir, const char* source) {
    dir = NormalizeDir(dir);
    if (dir.empty()) return false;
    f.tried.push_back(dir);
    if (!s.exists(dir + L"\\Data5.bhd")) return false;
    f.dir = dir;
    f.source = source;
    return true;
  };
  if (!s.explicitDir.empty()) {
    check(s.explicitDir, "--game");
    return f;
  }
  if (!s.runningGameExe.empty() && check(ParentDir(s.runningGameExe), "the running game")) return f;
  if (!s.savedDir.empty() && check(s.savedDir, "settings.ini")) return f;
  if (s.steamPath.empty()) return f;
  std::wstring steam = NormalizeDir(s.steamPath);
  auto libs = ParseLibraryFolders(s.readFile(steam + L"\\steamapps\\libraryfolders.vdf"));
  if (std::none_of(libs.begin(), libs.end(), [&](const SteamLibrary& l) { return _wcsicmp(NormalizeDir(l.path).c_str(), steam.c_str()) == 0; }))
    libs.push_back({steam, {}});
  std::stable_partition(libs.begin(), libs.end(), [](const SteamLibrary& l) {
    return std::find(l.apps.begin(), l.apps.end(), kDs3SteamAppId) != l.apps.end();
  });
  for (const auto& lib : libs)
    if (check(lib.path + L"\\steamapps\\common\\DARK SOULS III\\Game", "Steam")) return f;
  return f;
}

// One argument for a Windows command line, quoted the way CommandLineToArgvW
// and the C runtime split it back (backslashes only double before a quote).
std::wstring QuoteArg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
  std::wstring out = L"\"";
  for (size_t i = 0;; ++i) {
    size_t slashes = 0;
    while (i < a.size() && a[i] == L'\\') {
      ++i;
      ++slashes;
    }
    if (i == a.size()) {
      out.append(slashes * 2, L'\\');
      break;
    }
    if (a[i] == L'"') {
      out.append(slashes * 2 + 1, L'\\');
    } else {
      out.append(slashes, L'\\');
    }
    out += a[i];
  }
  return out + L"\"";
}

std::wstring ExtractorCommandLine(const std::wstring& python, const std::wstring& script, const std::wstring& gameDir,
                                  const std::wstring& outDir) {
  // -I: ignore PYTHON* variables and user site-packages; -B: write no
  // __pycache__ beside the scripts (that would be the program folder).
  // -u: unbuffered, so each progress line reaches the log as it's printed.
  return QuoteArg(python) + L" -I -B -u " + QuoteArg(script) + L" " + QuoteArg(gameDir) + L" --out " + QuoteArg(outDir);
}

// Re-read the map data? Yes when it's missing, or older than the game's
// files it's read from (a game patch rewrites them). Times: FILETIME as
// 64-bit ticks.
bool MapDataStale(bool haveData, uint64_t dataTime, uint64_t gameDataTime) {
  return !haveData || gameDataTime > dataTime;
}

// The generated tables the app reads, and the game files they come from:
// maps from Data5 (and the DLCs), text from Data1, the regulation Data0.bdt.
// All tables must be there; the oldest of them is
// compared with the newest game file.
const wchar_t* const kGeneratedTables[] = {L"treasures.tsv", L"enemies.tsv", L"item_names.tsv", L"bonfire_names.tsv",
                                           L"regions.tsv", L"boss_names.tsv"};
const wchar_t* const kGameSourceFiles[] = {L"Data5.bdt", L"Data1.bdt", L"Data0.bdt"};

struct FileSetTimes {
  bool all = true, any = false;
  uint64_t oldest = 0, newest = 0;  // over the files that exist
};

FileSetTimes TimesOf(const std::vector<std::wstring>& files, const std::function<bool(const std::wstring&)>& exists,
                     const std::function<uint64_t(const std::wstring&)>& time) {
  FileSetTimes t;
  for (const auto& f : files) {
    if (!exists(f)) {
      t.all = false;
      continue;
    }
    uint64_t ft = time(f);
    t.oldest = t.any ? (std::min)(t.oldest, ft) : ft;
    t.newest = t.any ? (std::max)(t.newest, ft) : ft;
    t.any = true;
  }
  return t;
}

// The Python to run the extractor with: the bundled one in an install, the
// system one (PATH) in a dev checkout. "" when there is none.
std::wstring ExtractorPython(const AppPaths& p, const std::function<bool(const std::wstring&)>& exists,
                             const std::function<std::wstring(const wchar_t*)>& searchPath) {
  if (!p.devMode) return exists(p.BundledPython()) ? p.BundledPython() : std::wstring();
  return searchPath(L"python.exe");
}

bool FileExists(const std::wstring& f) { return GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES; }

uint64_t FileTime(const std::wstring& f) {
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &d)) return 0;
  return (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}

FileSetTimes GeneratedDataTimes() {
  std::vector<std::wstring> files;
  for (const wchar_t* t : kGeneratedTables) files.push_back(Paths().Generated(t));
  return TimesOf(files, FileExists, FileTime);
}

// The newest of the game files the generated data comes from (0: none found).
uint64_t GameFilesTime(const std::wstring& gameDir) {
  std::vector<std::wstring> files;
  for (const wchar_t* f : kGameSourceFiles) files.push_back(gameDir + L"\\" + f);
  return TimesOf(files, FileExists, FileTime).newest;
}

// For the log: why the generated data is being read.
const char* MapDataReason(const FileSetTimes& data) {
  return !data.any ? "not there yet" : !data.all ? "incomplete (new tables to read)" : "older than the game's files";
}

std::wstring SearchPathFor(const wchar_t* exe) {
  wchar_t buf[MAX_PATH];
  DWORD n = SearchPathW(nullptr, exe, nullptr, MAX_PATH, buf, nullptr);
  return n > 0 && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

std::wstring SteamPathFromRegistry() {
  wchar_t buf[MAX_PATH];
  DWORD size = sizeof(buf);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) !=
      ERROR_SUCCESS)
    return L"";
  return buf;
}

// The running DS3's exe path, from a module snapshot (no handle on the game
// is opened), and only when Seamless Co-op is loaded -- the same rule as
// attaching. "" otherwise.
std::wstring RunningGameExePath() {
  DWORD pid = FindProcessIdByName(kTargetProcessName);
  if (!pid || !HasModule(ListModuleNames(pid), kSeamlessCoopModuleMarker)) return L"";
  std::wstring path;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE) return L"";
  MODULEENTRY32W e{};
  e.dwSize = sizeof(e);
  if (Module32FirstW(snap, &e)) {
    do {
      if (_wcsicmp(e.szModule, kTargetProcessName) == 0) path = e.szExePath;
    } while (path.empty() && Module32NextW(snap, &e));
  }
  CloseHandle(snap);
  return path;
}

GameDirFound FindGameDirNow(const std::wstring& explicitDir) {
  GameDirSearch s;
  s.explicitDir = explicitDir;
  if (explicitDir.empty()) s.runningGameExe = RunningGameExePath();
  s.savedDir = WidenUtf8(g_gameDir);
  s.steamPath = SteamPathFromRegistry();
  s.exists = FileExists;
  s.readFile = ReadWholeFile;
  return FindDs3GameDir(s);
}

// A job object whose processes Windows ends when the last handle to it
// closes -- i.e. when WASD exits, however it exits. The extractor runs in
// it, so closing WASD mid-read never leaves a Python process behind holding
// the install's files (found 2026-10-07 by the installer test: uninstall
// left python\ behind). Null if the job couldn't be made.
HANDLE CreateKillOnCloseJob() {
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (!job) return nullptr;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
  info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
    CloseHandle(job);
    return nullptr;
  }
  return job;
}

HANDLE ChildProcessJob() {
  static HANDLE job = CreateKillOnCloseJob();  // closed by Windows when this process ends
  return job;
}

// Starts cmd suspended, puts it in the job (if any), then lets it run.
bool StartInJob(std::wstring cmd, HANDLE job, STARTUPINFOW* si, PROCESS_INFORMATION* pi) {
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(0);
  if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                      nullptr, si, pi))
    return false;
  if (job) AssignProcessToJobObject(job, pi->hProcess);
  ResumeThread(pi->hThread);
  return true;
}

// Runs the extractor on gameDir into Paths().generated, its output going to
// this process's console / stdout. Exit codes: the extractor's (0 done,
// 2 no game folder, 3 no archives, 4 unreadable archive), 5 no Python or it
// couldn't start.
int RunExtractor(const std::wstring& gameDir) {
  const AppPaths& p = Paths();
  std::wstring python = ExtractorPython(p, FileExists, SearchPathFor);
  if (python.empty()) {
    if (p.devMode) Log("Map data: no Python on PATH (a dev checkout runs tools\\extract_treasures.py with it).");
    else Log("Map data: the bundled Python is missing (%ls) -- reinstall WASD.", p.BundledPython().c_str());
    return 5;
  }
  EnsureDir(p.generated);
  std::wstring cmd = ExtractorCommandLine(python, p.ExtractorScript(), gameDir, p.generated);
  Log("Map data: reading the map files in %ls (about 10 s)...", gameDir.c_str());
  // Its output comes back through a pipe into Log(): WASD.exe has no
  // console, and CREATE_NO_WINDOW keeps one from flashing up.
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE readEnd = nullptr, writeEnd = nullptr;
  if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return 5;
  SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nullptr;
  si.hStdOutput = writeEnd;
  si.hStdError = writeEnd;
  PROCESS_INFORMATION pi{};
  bool started = StartInJob(cmd, ChildProcessJob(), &si, &pi);
  DWORD startError = GetLastError();
  CloseHandle(writeEnd);
  if (!started) {
    CloseHandle(readEnd);
    Log("Map data: couldn't start %ls (error %lu).", python.c_str(), startError);
    return 5;
  }
  std::string pending;
  char chunk[512];
  DWORD got = 0;
  while (ReadFile(readEnd, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
    pending.append(chunk, got);
    for (size_t nl; (nl = pending.find('\n')) != std::string::npos; pending.erase(0, nl + 1)) {
      std::string line = pending.substr(0, nl);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) Log("  %s", line.c_str());
    }
  }
  if (!pending.empty()) Log("  %s", pending.c_str());
  CloseHandle(readEnd);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 5;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (code == 0) Log("Map data: done (%ls).", p.generated.c_str());
  else Log("Map data: the extractor stopped with exit code %lu.", code);
  return static_cast<int>(code);
}

void RememberGameDir(const GameDirFound& g) {
  std::string dir = Utf8(g.dir);
  if (dir == g_gameDir) return;
  g_gameDir = dir;
  SaveSettings();
}

// wasd-cli.exe extract [--game <folder>]
int RunExtractCommand(int argc, wchar_t** argv) {
  std::wstring explicitDir;
  for (int i = 2; i < argc; ++i)
    if (std::wstring(argv[i]) == L"--game" && i + 1 < argc) explicitDir = argv[++i];
  GameDirFound g = FindGameDirNow(explicitDir);
  if (g.dir.empty()) {
    Log("Couldn't find Dark Souls III's \"Game\" folder (the one with Data5.bhd).");
    for (const auto& t : g.tried) Log("  looked in %ls", t.c_str());
    Log("Run:  wasd-cli.exe extract --game \"<...>\\DARK SOULS III\\Game\"");
    return 2;
  }
  Log("Game folder: %ls (from %s)", g.dir.c_str(), g.source.c_str());
  RememberGameDir(g);
  return RunExtractor(g.dir);
}

// `overlay`'s first-run step: read the map data when it's missing or older
// than the game's archives. Never stops the guide from starting.
void EnsureMapDataAtStart() {
  FileSetTimes data = GeneratedDataTimes();
  GameDirFound g = FindGameDirNow(L"");
  if (g.dir.empty()) {
    if (!data.all)
      Log("Map data: DS3's Game folder not found, so item positions, names and boss weaknesses stay off. "
          "Run:  wasd-cli.exe extract --game \"<...>\\DARK SOULS III\\Game\"");
    return;
  }
  if (!MapDataStale(data.all, data.oldest, GameFilesTime(g.dir))) return;
  Log("Map data: %s -- reading it from your DS3 install (first run, about 20 s).", MapDataReason(data));
  RememberGameDir(g);
  if (RunExtractor(g.dir) != 0)
    Log("Map data: not read; the guide runs without item positions. Retry with  wasd-cli.exe extract");
}

// ---- the app window (WASD.exe) ---------------------
//
// WASD.exe is the same program as `wasd-cli.exe overlay` plus this window:
// it has its own taskbar button (pin it to start WASD from the taskbar),
// shows what the guide is doing, and holds the few controls that matter
// while playing. Closing it quits the guide, as the page's Quit button
// does. Its text comes from MainWindowModel (pure, unit-tested).

void BumpMapDataVersion();  // defined with the map-data tables
int RunReport(bool openInBrowser);

constexpr wchar_t kAppUserModelId[] = L"AmishGoose.WASD";  // shared with the Start menu shortcut (part 5)

enum MapDataStatus : LONG { kMapChecking = 0, kMapReading, kMapReady, kMapNoGame, kMapFailed };
volatile LONG g_mapDataStatus = kMapChecking;
bool g_appMode = false;

struct MainWindowText {
  std::string title, status, details, mapData, livePage;
  bool mapDataBusy = false;
};

MainWindowText MainWindowModel(const WindowSnapshot& s, LONG mapStatus, const std::string& liveUrl) {
  MainWindowText t;
  t.title = std::string(WASD_APP_SHORT_NAME) + " " + WASD_VERSION_STRING;
  if (s.paused) t.status = "Paused (F10): no memory reads until you press F10 again";
  else if (s.haveValues) t.status = "Attached to Dark Souls III (Seamless Co-op)";
  else if (!s.status.empty()) t.status = s.status;
  else t.status = "Waiting for Dark Souls III with Seamless Co-op";
  if (s.haveValues) {
    t.details = s.characterName.empty() ? std::string("Your character") : s.characterName;
    if (s.level > 0) t.details += ", level " + std::to_string(s.level);
    if (!s.currentArea.empty()) t.details += "  \xC2\xB7  " + s.currentArea;
    t.details += "  \xC2\xB7  session " + FormatDuration(s.sessionSeconds);
  } else {
    t.details = "Start the game through Seamless Co-op; WASD attaches by itself.";
  }
  switch (mapStatus) {
    case kMapReading:
      t.mapData = "Map data: reading it from your DS3 install (about 10 s)...";
      t.mapDataBusy = true;
      break;
    case kMapReady: t.mapData = "Map data: ready"; break;
    case kMapNoGame: t.mapData = "Map data: DS3's Game folder not found. Use \"Read map data\" to choose it."; break;
    case kMapFailed: t.mapData = "Map data: couldn't be read (the log says why)."; break;
    default: t.mapData = "Map data: checking..."; break;
  }
  t.livePage = liveUrl.empty() ? "Live page: starting..." : "Live page: " + liveUrl;
  return t;
}

// WASD_INSTANCE (tests): a separate identity -- mutex and window class --
// so a test copy never meets a real one. Letters, digits and '-' only.
std::wstring SanitizeInstance(const std::wstring& v) {
  std::wstring out;
  for (wchar_t c : v)
    if (iswalnum(c) || c == L'-') out += c;
  return out.substr(0, 40);
}
std::wstring InstanceMutexName(const std::wstring& suffix) {
  return L"Local\\AmishGoose.WASD" + (suffix.empty() ? std::wstring() : L"." + suffix);
}
std::wstring MainWindowClassName(const std::wstring& suffix) {
  return L"WasdMainWindow" + (suffix.empty() ? std::wstring() : L"." + suffix);
}

// -- background map data ---------------------------------------------------
std::wstring g_mapThreadGameDir;

DWORD WINAPI MapDataThread(LPVOID) {
  int code = RunExtractor(g_mapThreadGameDir);
  if (code == 0) BumpMapDataVersion();  // the poll worker reloads the tables at its next tick
  InterlockedExchange(&g_mapDataStatus, code == 0 ? kMapReady : kMapFailed);
  PostMessageW(g_window.hwnd, kMsgMapData, 0, 0);
  return 0;
}

void StartMapDataThread(const std::wstring& gameDir) {
  if (g_mapDataStatus == kMapReading) return;
  g_mapThreadGameDir = gameDir;
  InterlockedExchange(&g_mapDataStatus, kMapReading);
  HANDLE t = CreateThread(nullptr, 0, MapDataThread, nullptr, 0, nullptr);
  if (t) CloseHandle(t);
  else InterlockedExchange(&g_mapDataStatus, kMapFailed);
  RefreshMainWindow();
}

// What `overlay` does before starting (EnsureMapDataAtStart), but in the
// background so the window and the live page come up straight away.
void StartFirstRunMapData() {
  FileSetTimes data = GeneratedDataTimes();
  GameDirFound g = FindGameDirNow(L"");
  if (g.dir.empty()) {
    InterlockedExchange(&g_mapDataStatus, data.all ? kMapReady : kMapNoGame);
    if (!data.all) Log("Map data: DS3's Game folder not found; choose it with \"Read map data\" in the WASD window.");
    return;
  }
  if (!MapDataStale(data.all, data.oldest, GameFilesTime(g.dir))) {
    InterlockedExchange(&g_mapDataStatus, kMapReady);
    return;
  }
  Log("Map data: %s -- reading it from your DS3 install in the background.", MapDataReason(data));
  RememberGameDir(g);
  StartMapDataThread(g.dir);
}

std::wstring PickFolder(HWND owner, const wchar_t* title) {
  std::wstring out;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dlg->SetTitle(title);
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        out = path;
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

// "Read map data": the found game folder, else ask for it.
void OnReadMapData(HWND hwnd) {
  GameDirFound g = FindGameDirNow(L"");
  if (g.dir.empty()) {
    std::wstring dir = PickFolder(hwnd, L"Choose the DARK SOULS III \"Game\" folder (the one with Data5.bdt)");
    if (dir.empty()) return;
    g = FindGameDirNow(dir);
    if (g.dir.empty()) {
      MessageBoxW(hwnd, L"That folder has no Data5.bhd. Choose the \"Game\" folder inside DARK SOULS III.",
                  L"WASD", MB_OK | MB_ICONINFORMATION);
      return;
    }
  }
  RememberGameDir(g);
  StartMapDataThread(g.dir);
}

// -- the window itself -------------------------------------------------------
enum MainControl : int {
  kIdStatus = 101, kIdDetails, kIdMapData, kIdReadMapData, kIdLivePage, kIdOpenPage,
  kIdOverlay, kIdTierLabel, kIdTier, kIdResults, kIdDataFolder, kIdQuit
};
HFONT g_mainFont = nullptr, g_mainBoldFont = nullptr;

void SetTextIfChanged(HWND w, const std::wstring& text) {
  wchar_t buf[512];
  GetWindowTextW(w, buf, 512);
  if (text != buf) SetWindowTextW(w, text.c_str());
}

void RefreshMainWindow() {
  if (!g_mainWindow) return;
  WindowSnapshot snap;
  EnterCriticalSection(&g_window.lock);
  snap = g_window.snapshot;
  LeaveCriticalSection(&g_window.lock);
  MainWindowText t = MainWindowModel(snap, g_mapDataStatus, Utf8(g_liveUrl));
  SetTextIfChanged(g_mainWindow, WidenUtf8(t.title));
  SetTextIfChanged(GetDlgItem(g_mainWindow, kIdStatus), WidenUtf8(t.status));
  SetTextIfChanged(GetDlgItem(g_mainWindow, kIdDetails), WidenUtf8(t.details));
  SetTextIfChanged(GetDlgItem(g_mainWindow, kIdMapData), WidenUtf8(t.mapData));
  SetTextIfChanged(GetDlgItem(g_mainWindow, kIdLivePage), WidenUtf8(t.livePage));
  EnableWindow(GetDlgItem(g_mainWindow, kIdReadMapData), !t.mapDataBusy);
  EnableWindow(GetDlgItem(g_mainWindow, kIdOpenPage), g_liveUrl[0] != 0);
  CheckDlgButton(g_mainWindow, kIdOverlay, g_ov.visible ? BST_CHECKED : BST_UNCHECKED);
  HWND tier = GetDlgItem(g_mainWindow, kIdTier);
  if (!SendMessageW(tier, CB_GETDROPPEDSTATE, 0, 0) && SendMessageW(tier, CB_GETCURSEL, 0, 0) != SpoilerTier())
    SendMessageW(tier, CB_SETCURSEL, SpoilerTier(), 0);
}

LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_COMMAND: {
      int id = LOWORD(wParam), code = HIWORD(wParam);
      if (id == kIdOpenPage && g_liveUrl[0]) ShellExecuteW(nullptr, L"open", g_liveUrl, nullptr, nullptr, SW_SHOWNORMAL);
      else if (id == kIdReadMapData) OnReadMapData(hwnd);
      else if (id == kIdResults) RunReport(true);
      else if (id == kIdDataFolder) {
        EnsureDir(Paths().user);
        ShellExecuteW(nullptr, L"open", Paths().user.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      } else if (id == kIdQuit) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
      } else if (id == kIdOverlay && code == BN_CLICKED) {
        InterlockedExchange(&g_ov.visible, IsDlgButtonChecked(hwnd, kIdOverlay) == BST_CHECKED ? 1 : 0);
        SaveSettings();
        PostMessageW(g_window.hwnd, kMsgOverlaySettings, 0, 0);
      } else if (id == kIdTier && code == CBN_SELCHANGE) {
        LRESULT sel = SendMessageW(reinterpret_cast<HWND>(lParam), CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel <= 3) {
          InterlockedExchange(&g_spoilerTier, static_cast<LONG>(sel));
          SaveSettings();
          PostMessageW(g_window.hwnd, kMsgSpoilerTierSet, 0, 0);
        }
      }
      return 0;
    }
    case kMsgShowYourself:
      ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
      SetForegroundWindow(hwnd);
      return 0;
    case WM_CLOSE:
      Log("Quit from the WASD window.");
      if (g_window.hwnd) PostMessageW(g_window.hwnd, WM_CLOSE, 0, 0);  // same shutdown as the page's Quit
      else DestroyWindow(hwnd);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HFONT MessageFont(bool bold) {
  NONCLIENTMETRICSW m{};
  m.cbSize = sizeof(m);
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(m), &m, 0);
  if (bold) m.lfMessageFont.lfWeight = FW_SEMIBOLD;
  return CreateFontIndirectW(&m.lfMessageFont);
}

void CreateMainWindow(HINSTANCE instance) {
  std::wstring cls = MainWindowClassName(SanitizeInstance(EnvVar(L"WASD_INSTANCE")));
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = MainWindowProc;
  wc.hInstance = instance;
  wc.lpszClassName = cls.c_str();
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW (this build has no UNICODE define)
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                           GetSystemMetrics(SM_CYICON), 0));
  wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                             GetSystemMetrics(SM_CYSMICON), 0));
  RegisterClassExW(&wc);
  double sc = g_window.scale > 0 ? g_window.scale : 1.0;
  auto px = [sc](int v) { return static_cast<int>(v * sc + 0.5); };
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT r{0, 0, px(560), px(232)};
  AdjustWindowRectEx(&r, style, FALSE, WS_EX_APPWINDOW);
  std::wstring title = WidenUtf8(std::string(WASD_APP_SHORT_NAME) + " " + WASD_VERSION_STRING);
  g_mainWindow = CreateWindowExW(WS_EX_APPWINDOW, cls.c_str(), title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                                 r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, nullptr);
  if (!g_mainWindow) return;
  if (!g_mainFont) g_mainFont = MessageFont(false);
  if (!g_mainBoldFont) g_mainBoldFont = MessageFont(true);
  auto add = [&](const wchar_t* cls2, const wchar_t* text, DWORD st, int x, int y, int w, int h, int id, bool bold = false) {
    HWND c = CreateWindowExW(0, cls2, text, WS_CHILD | WS_VISIBLE | st, px(x), px(y), px(w), px(h), g_mainWindow,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(bold ? g_mainBoldFont : g_mainFont), TRUE);
    return c;
  };
  add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 16, 14, 528, 22, kIdStatus, true);
  add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 16, 40, 528, 20, kIdDetails);
  add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 16, 76, 372, 20, kIdMapData);
  add(L"BUTTON", L"Read map data...", BS_PUSHBUTTON | WS_TABSTOP, 400, 70, 144, 28, kIdReadMapData);
  add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 16, 112, 372, 20, kIdLivePage);
  add(L"BUTTON", L"Open live page", BS_PUSHBUTTON | WS_TABSTOP, 400, 106, 144, 28, kIdOpenPage);
  add(L"BUTTON", L"Show overlay", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 150, 150, 24, kIdOverlay);
  add(L"STATIC", L"Spoiler tier:", SS_RIGHT, 220, 154, 90, 20, kIdTierLabel);
  HWND tier = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 318, 149, 120, 200, kIdTier);
  for (int t = 0; t <= 3; ++t) SendMessageW(tier, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(WidenUtf8(SpoilerTierName(t)).c_str()));
  add(L"BUTTON", L"Results page", BS_PUSHBUTTON | WS_TABSTOP, 16, 192, 130, 28, kIdResults);
  add(L"BUTTON", L"Open data folder", BS_PUSHBUTTON | WS_TABSTOP, 156, 192, 150, 28, kIdDataFolder);
  add(L"BUTTON", L"Quit", BS_PUSHBUTTON | WS_TABSTOP, 444, 192, 100, 28, kIdQuit);
  RefreshMainWindow();
  ShowWindow(g_mainWindow, SW_SHOWNORMAL);
  UpdateWindow(g_mainWindow);
}

// `WASD.exe --smoke-test`: the window comes up as a real app window, with no
// game, overlay or live page, then closes. Prints "ok"/"FAIL" lines (to a
// pipe when a test runs it) and exits 0 when everything holds.
int RunSmokeTest() {
  SetProcessDPIAware();
  InitializeCriticalSection(&g_window.lock);
  HDC screen = GetDC(nullptr);
  g_window.scale = GetDeviceCaps(screen, LOGPIXELSY) / 96.0;
  ReleaseDC(nullptr, screen);
  CreateMainWindow(GetModuleHandleW(nullptr));
  int failures = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
  };
  check(g_mainWindow && IsWindowVisible(g_mainWindow), "window shown");
  LONG_PTR ex = g_mainWindow ? GetWindowLongPtrW(g_mainWindow, GWL_EXSTYLE) : 0;
  check((ex & WS_EX_APPWINDOW) && !(ex & WS_EX_TOOLWINDOW) && g_mainWindow && !GetWindow(g_mainWindow, GW_OWNER),
        "taskbar button (WS_EX_APPWINDOW, not a tool window, no owner)");
  PWSTR id = nullptr;
  bool aumid = SUCCEEDED(GetCurrentProcessExplicitAppUserModelID(&id)) && id && std::wstring(id) == kAppUserModelId;
  if (id) CoTaskMemFree(id);
  check(aumid, "AppUserModelID AmishGoose.WASD");
  wchar_t title[128] = {};
  if (g_mainWindow) GetWindowTextW(g_mainWindow, title, 128);
  check(Utf8(title) == std::string(WASD_APP_SHORT_NAME) + " " + WASD_VERSION_STRING, "title \"WASD <version>\"");
  check(FileExists(Paths().Data(L"bosses.tsv")) && FileExists(Paths().Template(L"live.html")), "assets found");
  check(GetDlgItem(g_mainWindow, kIdQuit) && GetDlgItem(g_mainWindow, kIdTier) && GetDlgItem(g_mainWindow, kIdReadMapData),
        "controls");
  if (g_mainWindow) DestroyWindow(g_mainWindow);
  g_mainWindow = nullptr;
  std::fflush(stdout);
  return failures ? 1 : 0;
}

// WASD.exe: single instance, the taskbar identity, a log file, then the
// overlay machinery with the app window. Exit codes: 0, 3 already running
// (the running window was brought to the front), 4 couldn't create windows.
int RunApp(const std::wstring& args) {
  SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);
  LoadSettings();
  if (args.find(L"--smoke-test") != std::wstring::npos) return RunSmokeTest();
  std::wstring suffix = SanitizeInstance(EnvVar(L"WASD_INSTANCE"));
  HANDLE mutex = CreateMutexW(nullptr, TRUE, InstanceMutexName(suffix).c_str());
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND w = FindWindowW(MainWindowClassName(suffix).c_str(), nullptr)) {
      PostMessageW(w, kMsgShowYourself, 0, 0);
      SetForegroundWindow(w);  // allowed: this process was just started by the user
    }
    CloseHandle(mutex);
    return 3;
  }
  OpenLogFile();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  g_appMode = true;
  int code = RunOverlay(true);
  CoUninitialize();
  if (mutex) {
    ReleaseMutex(mutex);
    CloseHandle(mutex);
  }
  if (g_logFile) std::fclose(g_logFile);
  g_logFile = nullptr;
  return code;
}

std::vector<std::string> SplitStr(const std::string& s, char sep);  // defined with the route

// A session's perf.csv (kPerfCsvHeader) as a JSON array of rows,
// [["time", session_t, window_s, ...], ...], or "" when it has none. The
// header and any row that isn't 10 fields -- a time, then 9 finite
// numbers -- are skipped (e.g. a row cut off by a crash mid-write).
std::string PerfCsvToJson(const std::string& csv) {
  std::string perf;
  for (size_t p = 0; p < csv.size();) {
    size_t eol = csv.find('\n', p);
    std::string line = csv.substr(p, eol == std::string::npos ? std::string::npos : eol - p);
    p = eol == std::string::npos ? csv.size() : eol + 1;
    while (!line.empty() && line.back() == '\r') line.pop_back();
    auto fields = SplitStr(line, ',');
    if (fields.size() != 10 || fields[0].empty() ||
        fields[0].find_first_not_of("0123456789-:T") != std::string::npos)
      continue;
    std::string row = "[\"" + fields[0] + "\"";
    bool ok = true;
    for (size_t f = 1; f < fields.size() && ok; ++f) {
      char* end = nullptr;
      double v = std::strtod(fields[f].c_str(), &end);
      ok = !fields[f].empty() && end && *end == '\0' && std::isfinite(v);
      row += "," + fields[f];
    }
    if (!ok) continue;
    perf += (perf.empty() ? "" : ",") + row + "]";
  }
  return perf.empty() ? "" : "[" + perf + "]";
}

// The results page from its template: the sessions JSON goes in place of
// the last __SESSIONS_JSON__ (the script's data line; the header comment
// mentions the token too), with "</" escaped so the data can't close the
// <script> early, and every __APP_VERSION__ becomes the app's version.
// "" when the template has no data token.
std::string BuildReportHtml(std::string tmpl, std::string sessionsJson) {
  const std::string token = "__SESSIONS_JSON__", version = "__APP_VERSION__";
  if (tmpl.rfind(token) == std::string::npos) return "";
  // The version first, so session data that happens to contain the token
  // (a character name, say) is left alone.
  for (size_t p = 0; (p = tmpl.find(version, p)) != std::string::npos; p += std::strlen(WASD_VERSION_STRING))
    tmpl.replace(p, version.size(), WASD_VERSION_STRING);
  for (size_t p = 0; (p = sessionsJson.find("</", p)) != std::string::npos; p += 3) sessionsJson.replace(p, 2, "<\\/");
  size_t at = tmpl.rfind(token);
  tmpl.replace(at, token.size(), sessionsJson);
  return tmpl;
}

int RunReport(bool openInBrowser) {
  const std::wstring templatePath = Paths().Template(L"results_template.html");
  std::string tmpl = ReadWholeFile(templatePath);
  if (BuildReportHtml(tmpl, "[]").empty()) {
    Log("Template missing or has no __SESSIONS_JSON__ token: %ls", templatePath.c_str());
    return 4;
  }

  std::string json = "[";
  int sessionCount = 0, eventCount = 0;
  WIN32_FIND_DATAW player;
  HANDLE players = FindFirstFileW((Paths().Sessions() + L"\\*").c_str(), &player);
  if (players != INVALID_HANDLE_VALUE) {
    do {
      if (!(player.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || player.cFileName[0] == L'.')
        continue;
      std::wstring dir = Paths().Sessions() + L"\\" + player.cFileName;
      WIN32_FIND_DATAW file;
      HANDLE files = FindFirstFileW((dir + L"\\*.jsonl").c_str(), &file);
      if (files == INVALID_HANDLE_VALUE) continue;
      do {
        std::string text = ReadWholeFile(dir + L"\\" + file.cFileName);
        std::string events;
        size_t pos = 0;
        while (pos < text.size()) {
          size_t eol = text.find('\n', pos);
          std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
          pos = eol == std::string::npos ? text.size() : eol + 1;
          while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
          if (line.size() < 2 || line.front() != '{' || line.back() != '}') continue;
          if (!events.empty()) events += ",";
          events += line;
          ++eventCount;
        }
        if (events.empty()) continue;
        int n = WideCharToMultiByte(CP_UTF8, 0, file.cFileName, -1, nullptr, 0, nullptr, nullptr);
        std::string fileName(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, file.cFileName, -1, fileName.data(), n, nullptr, nullptr);
        if (sessionCount++) json += ",";
        json += "{\"file\":" + JsonString(fileName) + ",\"events\":[" + events + "]";
        // The session's perf figures (<stamp>.perf.csv, kPerfCsvHeader), if
        // any, as "perf": [["time", session_t, window_s, ...], ...]. Rows
        // that aren't 10 fields with numbers after the time are skipped.
        std::wstring stem = file.cFileName;
        stem.resize(stem.size() - 6);  // ".jsonl"
        std::string perf = PerfCsvToJson(ReadWholeFile(dir + L"\\" + stem + L".perf.csv"));
        if (!perf.empty()) json += ",\"perf\":" + perf;
        json += "}";
      } while (FindNextFileW(files, &file));
      FindClose(files);
    } while (FindNextFileW(players, &player));
    FindClose(players);
  }
  json += "]";
  tmpl = BuildReportHtml(tmpl, json);
  std::wstring outDir = Paths().Reports();
  EnsureDir(outDir);
  std::wstring outPath = outDir + L"\\results.html";
  FILE* out = nullptr;
  if (_wfopen_s(&out, outPath.c_str(), L"wb") != 0 || !out) {
    Log("Couldn't write %ls", outPath.c_str());
    return 4;
  }
  std::fwrite(tmpl.data(), 1, tmpl.size(), out);
  std::fclose(out);
  Log("Wrote %ls -- %d session(s), %d event(s).", outPath.c_str(), sessionCount, eventCount);
  if (openInBrowser) ShellExecuteW(nullptr, L"open", outPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return 0;
}

// ---- event flags ---------------------------------------
//
// DS3 records progress as numbered on/off flags (item pickups, boss
// kills, bonfires lit, quest steps). Method from SoulSplitter's DS3
// ReadEventFlag (github.com/FrankvdStam/SoulSplitter, GPL-3.0) --
// reimplemented here from that description, no code copied. The two
// byte patterns are our own (see "memory roots"). A flag id splits into digits:
//   group = id / 10,000,000 % 10   -> which flag table (x 0x18)
//   area  = id / 100,000 % 100     -> world area (e.g. 30 = High Wall)
//   block = id / 10,000 % 10       -> block within the area
//   sub   = id / 1,000 % 10        -> 1000-flag chunk (x 0x10)
//   bit   = id % 1,000             -> word (bit >> 5), bit 31 - (bit & 31)
// Global flags (area >= 90, or area and block both 0) use category 0.
// Area flags look the area/block up in FieldArea's world-info table
// (entries 0x38 apart: area byte +0x0B, block count byte +0x20, block
// array pointer +0x28; blocks 0x70 apart: packed id +0x08, category
// +0x20) and use category + 1. The flag word then lives at
//   [[[EventFlagMan + 0x218] + group*0x18] + sub*0x10 + category*0xA8]
//     + (bit >> 5) * 4
// UNVERIFIED until checked live against known flags (docs/TECHNICAL.md).
// mov rcx,[rip+EventFlagMan]; add rcx,250h; mov .. (386 references)
constexpr const char* kEventFlagManPattern = "48 8B 0D ?? ?? ?? ?? 48 81 C1 50 02 00 00 48 8B";
// mov rax,[rip+FieldArea]; movsxd rbx,dword [rax+20h]; test rcx,rcx; jnz (105 references)
constexpr const char* kFieldAreaPattern = "48 8B 05 ?? ?? ?? ?? 48 63 58 20 48 85 C9 75 26";

struct EventFlagRoots {
  uintptr_t eventFlagManCell = 0;  // module-relative
  uintptr_t fieldAreaCell = 0;     // module-relative
};

// RIP-relative cell from a pattern: disp32 at +3, instruction `len` bytes.
uintptr_t ResolveRipCell(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize,
                         const char* pattern, int instrLen) {
  uintptr_t instr = FindPatternInModule(hProcess, moduleBase, moduleSize, ParsePattern(pattern));
  if (instr == 0) return 0;
  int32_t disp = 0;
  SIZE_T br = 0;
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(instr + 3), &disp, sizeof(disp), &br))
    return 0;
  return static_cast<uintptr_t>(static_cast<intptr_t>(instr) + instrLen + disp) - moduleBase;
}

EventFlagRoots ResolveEventFlagRoots(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize) {
  EventFlagRoots r;
  r.eventFlagManCell = ResolveRipCell(hProcess, moduleBase, moduleSize, kEventFlagManPattern, 7);
  r.fieldAreaCell = ResolveRipCell(hProcess, moduleBase, moduleSize, kFieldAreaPattern, 7);
  return r;
}

// ---- roots: where every memory root resolves ----
//
// Every root's pattern must match exactly once in the game's code; `roots`
// shows each match count and what it resolves to. Checked live on
// 2026-10-09 (DS3 1.15.2): every pattern matched once and resolved to the
// same place as the copied patterns it replaced (and the old hard-coded
// param master cell, +0x479B8B0).
struct RootPattern {
  const char* name;
  const char* pattern;
  int length;  // instruction length for a RIP-relative cell; 0 = a struct offset (the disp32 itself)
};
constexpr RootPattern kRootPatterns[] = {
    {"WorldChrMan", kWorldChrManPattern, 7}, {"BaseA", kBaseAPattern, 7},
    {"GameMan", kGameManPattern, 7},         {"EventFlagMan", kEventFlagManPattern, 7},
    {"FieldArea", kFieldAreaPattern, 7},     {"XA", kXaPattern, 0},
    {"ParamMaster", kParamMasterPattern, 7}};

// Every offset where `pat` matches in data[0..size). Pure: the tests use it on synthetic code.
std::vector<size_t> FindPatternInBytes(const unsigned char* data, size_t size, const std::vector<PatternByte>& pat) {
  std::vector<size_t> hits;
  if (pat.empty() || size < pat.size()) return hits;
  for (size_t i = 0; i + pat.size() <= size; ++i) {
    bool ok = true;
    for (size_t k = 0; k < pat.size() && ok; ++k) ok = pat[k].wildcard || data[i + k] == pat[k].value;
    if (ok) hits.push_back(i);
  }
  return hits;
}

// What a root pattern matched at `at` yields: the disp32 at +3 is either a
// RIP-relative displacement (cell = at + length + disp32, an offset into
// the same buffer / module) or, for length 0, a struct offset itself.
uintptr_t RootValueAt(const unsigned char* data, size_t at, int length) {
  int32_t disp = 0;
  std::memcpy(&disp, data + at + 3, sizeof(disp));
  if (length == 0) return static_cast<uintptr_t>(static_cast<intptr_t>(disp));
  return static_cast<uintptr_t>(static_cast<intptr_t>(at) + length + disp);
}

int RunRoots() {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  if (moduleBase == 0 || moduleSize == 0) {
    CloseHandle(hProcess);
    return 4;
  }
  std::vector<unsigned char> image(moduleSize, 0);
  for (SIZE_T off = 0; off < moduleSize; off += 0x10000) {  // the whole module, chunk by chunk (read-only)
    SIZE_T len = (std::min)(static_cast<SIZE_T>(0x10000), moduleSize - off), br = 0;
    if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(moduleBase + off), image.data() + off, len, &br))
      std::fill(image.begin() + off, image.begin() + off + len, 0);
  }
  CloseHandle(hProcess);
  Log("Memory roots (module 0x%llx, size 0x%llx): pattern matches, then what it resolves to",
      static_cast<unsigned long long>(moduleBase), static_cast<unsigned long long>(moduleSize));
  int bad = 0;
  for (const auto& r : kRootPatterns) {
    auto hits = FindPatternInBytes(image.data(), image.size(), ParsePattern(r.pattern));
    uintptr_t v = hits.empty() ? 0 : RootValueAt(image.data(), hits[0], r.length);
    bad += hits.size() == 1 ? 0 : 1;
    Log("  %-13s [%zu]  %s0x%llx%s", r.name, hits.size(), r.length ? "DarkSoulsIII.exe+" : "offset ",
        static_cast<unsigned long long>(v), hits.size() == 1 ? "" : "  ** not exactly one match **");
  }
  Log(bad ? "%d root(s) need attention." : "Every root's pattern matches exactly once.", bad);
  return bad ? 1 : 0;
}

// -1 = couldn't read (roots missing, area not loaded), 0 = off, 1 = on.
// `trace` (optional) collects the intermediate addresses for debugging.
int ReadEventFlag(HANDLE hProcess, uintptr_t moduleBase, const EventFlagRoots& roots,
                  uint32_t id, std::string* trace = nullptr) {
  SIZE_T br = 0;
  auto ptrAt = [&](uintptr_t addr) -> uintptr_t {
    uintptr_t v = 0;
    if (addr == 0 || !ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), &v, sizeof(v), &br))
      return 0;
    return v;
  };
  auto i32At = [&](uintptr_t addr) -> int32_t {
    int32_t v = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), &v, sizeof(v), &br);
    return v;
  };
  auto u8At = [&](uintptr_t addr) -> uint8_t {
    uint8_t v = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), &v, sizeof(v), &br);
    return v;
  };
  auto note = [&](const char* what, uintptr_t v) {
    if (!trace) return;
    char buf[64];
    std::snprintf(buf, sizeof(buf), " %s=0x%llx", what, static_cast<unsigned long long>(v));
    *trace += buf;
  };
  if (roots.eventFlagManCell == 0) return -1;

  const int group = static_cast<int>(id / 10000000 % 10);
  const int area = static_cast<int>(id / 100000 % 100);
  const int block = static_cast<int>(id / 10000 % 10);
  const int sub = static_cast<int>(id / 1000 % 10);
  const int bit = static_cast<int>(id % 1000);

  int category = -1;
  if (area >= 90 || area + block == 0) {
    category = 0;
  } else {
    uintptr_t fieldArea = ptrAt(moduleBase + roots.fieldAreaCell);
    uintptr_t worldInfoOwner = ptrAt(fieldArea + 0x10);
    note("fieldArea", fieldArea);
    note("worldInfo", worldInfoOwner);
    if (worldInfoOwner == 0) return -1;
    // +0x10 holds a POINTER to the area array (live-checked 2026-10-06: it
    // points just past the header, at owner+0x30), not the array itself.
    int32_t areaCount = i32At(worldInfoOwner + 0x8);
    uintptr_t areas = ptrAt(worldInfoOwner + 0x10);
    for (int32_t i = 0; i < areaCount && i < 256 && category < 0; ++i) {
      uintptr_t entry = areas + static_cast<uintptr_t>(i) * 0x38;
      if (u8At(entry + 0x0B) != area) continue;
      int blockCount = u8At(entry + 0x20);
      uintptr_t blocks = ptrAt(entry + 0x28);
      for (int b = 0; b < blockCount; ++b) {
        int32_t packed = i32At(blocks + static_cast<uintptr_t>(b) * 0x70 + 0x8);
        if (((packed >> 16) & 0xFF) == block && (packed >> 24) == area) {
          category = i32At(blocks + static_cast<uintptr_t>(b) * 0x70 + 0x20) + 1;
          break;
        }
      }
    }
    if (category < 0) return -1;  // area not in the loaded world info
  }
  note("category", static_cast<uintptr_t>(category));

  uintptr_t flagMan = ptrAt(moduleBase + roots.eventFlagManCell);
  uintptr_t tables = ptrAt(flagMan + 0x218);
  uintptr_t table = ptrAt(tables + static_cast<uintptr_t>(group) * 0x18);
  note("flagMan", flagMan);
  note("table", table);
  if (table == 0) return -1;
  uintptr_t chunk = ptrAt(table + static_cast<uintptr_t>(sub) * 0x10 + static_cast<uintptr_t>(category) * 0xA8);
  note("chunk", chunk);
  if (chunk == 0) return -1;
  uint32_t word = 0;
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(chunk + static_cast<uintptr_t>(bit >> 5) * 4),
                         &word, sizeof(word), &br))
    return -1;
  return (word >> (31 - (bit & 31))) & 1u ? 1 : 0;
}

// Bosses: data/bosses.tsv with the names the game shows (see "bosses and
// progression" below). `flag --bosses` lists their defeat flags as a live
// self-check.
struct BossInfo {
  uint32_t flag;
  std::string name, area, status, note;
};
std::vector<BossInfo> LoadBosses();

// flag <id> [id ...] [--trace] | flag --bosses
int RunFlag(const std::vector<uint32_t>& ids, bool bosses, bool trace) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  EventFlagRoots roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  Log("EventFlagMan cell: DarkSoulsIII.exe+0x%llx, FieldArea cell: DarkSoulsIII.exe+0x%llx%s",
      static_cast<unsigned long long>(roots.eventFlagManCell),
      static_cast<unsigned long long>(roots.fieldAreaCell),
      roots.eventFlagManCell && roots.fieldAreaCell ? "" : "  ** a pattern was NOT found **");
  auto show = [&](uint32_t id, const char* name) {
    std::string t;
    int v = ReadEventFlag(hProcess, moduleBase, roots, id, trace ? &t : nullptr);
    Log("  %u  %-50s %s%s", id, name ? name : "",
        v < 0 ? "unreadable" : v ? "ON" : "off", t.c_str());
  };
  if (bosses) {
    Log("Boss-defeated flags (ON = defeated):");
    for (const auto& b : LoadBosses()) show(b.flag, b.name.c_str());
  }
  for (uint32_t id : ids) show(id, nullptr);
  CloseHandle(hProcess);
  return 0;
}

// ---- flagwatch: which event flags flip during an action --
//
// Snapshots every flag chunk the game has allocated -- each group's
// table, every category (0 = global, the rest = world-area blocks from
// FieldArea's table), all 10 chunks of 1000 flags -- every 200ms, and
// prints each flag that changes, turned back into its id:
//   id = group*10,000,000 + area*100,000 + block*10,000 + sub*1,000 + bit
// (category 0 is printed as "global", area/block 0). The first `learn`
// ticks only learn flags that change by themselves; those are suppressed.
struct FlagChunkRef {
  int group, category, sub;
  int area, block;  // from the world-info table; -1 for global
  uintptr_t addr;
};

std::vector<FlagChunkRef> EnumerateFlagChunks(HANDLE hProcess, uintptr_t moduleBase,
                                              const EventFlagRoots& roots) {
  std::vector<FlagChunkRef> out;
  SIZE_T br = 0;
  auto ptrAt = [&](uintptr_t addr) -> uintptr_t {
    uintptr_t v = 0;
    if (addr == 0 || !ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), &v, sizeof(v), &br))
      return 0;
    return v;
  };
  // category -> (area, block), from the world-info table.
  std::unordered_map<int, std::pair<int, int>> catArea;
  uintptr_t owner = ptrAt(ptrAt(moduleBase + roots.fieldAreaCell) + 0x10);
  int32_t areaCount = 0;
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(owner + 0x8), &areaCount, sizeof(areaCount), &br);
  uintptr_t areas = ptrAt(owner + 0x10);
  int maxCategory = 0;
  for (int32_t i = 0; owner && i < areaCount && i < 256; ++i) {
    uintptr_t entry = areas + static_cast<uintptr_t>(i) * 0x38;
    uint8_t blockCount = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(entry + 0x20), &blockCount, 1, &br);
    uintptr_t blocks = ptrAt(entry + 0x28);
    for (int b = 0; b < blockCount; ++b) {
      int32_t packed = 0, category = 0;
      ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(blocks + b * 0x70 + 0x8), &packed, 4, &br);
      ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(blocks + b * 0x70 + 0x20), &category, 4, &br);
      catArea[category + 1] = {packed >> 24, (packed >> 16) & 0xFF};
      maxCategory = (std::max)(maxCategory, category + 1);
    }
  }
  uintptr_t tables = ptrAt(ptrAt(moduleBase + roots.eventFlagManCell) + 0x218);
  // Some chunks are reachable from two (group, category) pairs -- live
  // 2026-10-06, Iudex's kill showed as both 14000800 and 4100800, and
  // both read ON. Each chunk is reported once, under the higher group
  // (the form the community's boss ids use), so groups go high to low.
  std::unordered_map<uintptr_t, bool> seen;
  for (int group = 9; group >= 0; --group) {
    uintptr_t table = ptrAt(tables + static_cast<uintptr_t>(group) * 0x18);
    if (!table) continue;
    for (int cat = 0; cat <= maxCategory; ++cat) {
      auto it = catArea.find(cat);
      if (cat != 0 && it == catArea.end()) continue;
      for (int sub = 0; sub < 10; ++sub) {
        uintptr_t chunk = ptrAt(table + static_cast<uintptr_t>(sub) * 0x10 + static_cast<uintptr_t>(cat) * 0xA8);
        if (!chunk || seen[chunk]) continue;
        seen[chunk] = true;
        out.push_back({group, cat, sub, cat == 0 ? -1 : it->second.first,
                       cat == 0 ? -1 : it->second.second, chunk});
      }
    }
  }
  return out;
}

int RunFlagWatch(int learnTicks, int iterations) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  EventFlagRoots roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  auto chunks = EnumerateFlagChunks(hProcess, moduleBase, roots);
  constexpr SIZE_T kChunkBytes = 128;  // 1000 flags = 125 bytes, read as 32 words
  Log("flagwatch: %zu flag chunk(s) (%zu flags). Learning noise for %d ticks (~%.1fs) -- don't "
      "touch anything yet.",
      chunks.size(), chunks.size() * 1000, learnTicks, learnTicks * 0.2);
  if (chunks.empty()) {
    Log("No flag chunks found -- is a character loaded?");
    CloseHandle(hProcess);
    return 6;
  }
  std::vector<uint32_t> prev(chunks.size() * 32), cur(prev.size());
  std::vector<uint32_t> noisy(prev.size(), 0);  // bitmask of noisy bits per word
  auto snap = [&](std::vector<uint32_t>& into) {
    SIZE_T br = 0;
    for (size_t c = 0; c < chunks.size(); ++c)
      ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(chunks[c].addr), &into[c * 32], kChunkBytes, &br);
  };
  snap(prev);
  for (int i = 0; i < iterations; ++i) {
    Sleep(200);
    DWORD exitCode = 0;
    if (GetExitCodeProcess(hProcess, &exitCode) && exitCode != STILL_ACTIVE) break;
    snap(cur);
    bool learning = i < learnTicks;
    if (i == learnTicks) Log("Learning done. NOW do the action.");
    for (size_t w = 0; w < cur.size(); ++w) {
      uint32_t diff = (cur[w] ^ prev[w]);
      if (!diff) continue;
      if (learning) { noisy[w] |= diff; continue; }
      diff &= ~noisy[w];
      for (int b = 0; b < 32 && diff; ++b) {
        uint32_t mask = 1u << (31 - b);
        if (!(diff & mask)) continue;
        diff &= ~mask;
        const auto& ch = chunks[w / 32];
        int bit = static_cast<int>(w % 32) * 32 + b;
        if (bit >= 1000) continue;
        uint32_t id = static_cast<uint32_t>(ch.group) * 10000000u +
                      static_cast<uint32_t>(ch.area < 0 ? 0 : ch.area) * 100000u +
                      static_cast<uint32_t>(ch.block < 0 ? 0 : ch.block) * 10000u +
                      static_cast<uint32_t>(ch.sub) * 1000u + static_cast<uint32_t>(bit);
        Log("  flag %u %s  (%s)", id, (cur[w] & mask) ? "off -> ON" : "ON -> off",
            ch.category == 0 ? "global" : ("area " + std::to_string(ch.area) + " block " +
                                           std::to_string(ch.block)).c_str());
      }
    }
    prev.swap(cur);
  }
  Log("Done.");
  CloseHandle(hProcess);
  return 0;
}

// ---- paramsearch: find a number in any game table ------
//
// Scans every row of a live param table for an int32 equal to `value`
// at any 4-byte-aligned offset within the first `rowBytes` bytes, and
// prints row id + offset. Used to find which ItemLotParam row sets a
// given pickup flag, which BonfireWarpParam row names a bonfire flag,
// and so on -- the game's own tables as the source of flag ids.
// graphs: every CalcCorrectGraph row (breakpoints, outputs, exponents) --
// a diagnostic for finding the game's stat curves (level planner).
int RunGraphs() {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  auto table = ResolveParamTable(hProcess, moduleBase, L"CalcCorrectGraph");
  if (!table.ok) {
    Log("CalcCorrectGraph not found.");
    CloseHandle(hProcess);
    return 5;
  }
  SIZE_T br = 0;
  std::vector<unsigned char> index(static_cast<size_t>(table.rowCount) * 24);
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(table.tableBase + 0x40), index.data(), index.size(), &br);
  for (uint16_t i = 0; i < table.rowCount; ++i) {
    int64_t id = 0;
    std::memcpy(&id, index.data() + static_cast<size_t>(i) * 24, 8);
    auto g = ReadCalcCorrectGraphRow(hProcess, moduleBase, id);
    Log("  row %4lld  stat %5.0f %5.0f %5.0f %5.0f %5.0f | out %7.1f %7.1f %7.1f %7.1f %7.1f | adj %5.2f %5.2f %5.2f %5.2f",
        static_cast<long long>(id), g.stageMaxVal[0], g.stageMaxVal[1], g.stageMaxVal[2], g.stageMaxVal[3],
        g.stageMaxVal[4], g.stageMaxGrowVal[0], g.stageMaxGrowVal[1], g.stageMaxGrowVal[2], g.stageMaxGrowVal[3],
        g.stageMaxGrowVal[4], g.adjPt[0], g.adjPt[1], g.adjPt[2], g.adjPt[3]);
  }
  CloseHandle(hProcess);
  return 0;
}

int RunParamSearch(const std::wstring& tableName, int32_t value, SIZE_T rowBytes) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  (void)moduleSize;
  auto table = ResolveParamTable(hProcess, moduleBase, tableName.c_str());
  if (!table.ok) {
    Log("Table '%ls' not found.", tableName.c_str());
    CloseHandle(hProcess);
    return 6;
  }
  SIZE_T br = 0;
  std::vector<unsigned char> index(static_cast<size_t>(table.rowCount) * 24);
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(table.tableBase + 0x40), index.data(),
                    index.size(), &br);
  std::vector<unsigned char> row(rowBytes);
  int hits = 0;
  for (uint16_t i = 0; i < table.rowCount; ++i) {
    int64_t id = 0, offset = 0;
    std::memcpy(&id, index.data() + static_cast<size_t>(i) * 24, 8);
    std::memcpy(&offset, index.data() + static_cast<size_t>(i) * 24 + 8, 8);
    if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(table.tableBase + offset), row.data(),
                           row.size(), &br))
      continue;
    for (SIZE_T off = 0; off + 4 <= row.size(); off += 4) {
      int32_t v = 0;
      std::memcpy(&v, row.data() + off, 4);
      if (v != value) continue;
      Log("  %ls row %lld, offset 0x%02llx", tableName.c_str(), static_cast<long long>(id),
          static_cast<unsigned long long>(off));
      ++hits;
    }
  }
  Log("%d hit(s) for %d in %ls (%u rows, first 0x%llx bytes each).", hits, value, tableName.c_str(),
      table.rowCount, static_cast<unsigned long long>(rowBytes));
  CloseHandle(hProcess);
  return 0;
}

// ---- world pickups per map section ---------------------
//
// Every one-time world pickup is an ItemLotParam row whose getItemFlagId
// (+0x80) is that pickup's event flag (live-verified 2026-10-06: the
// Titanite Shard on the Cemetery path is lot 4000050 / flag 54000050).
// World pickups are taken to be the lots whose flag reads 5AAxxxxx,
// AA = the map section (m40 -> 40); enemy drops repeat (no flag) and
// boss rewards sit in the global 500xxxxx range (Iudex's Coiled Sword:
// 50002180). Found = the flag is ON. Layout: Paramdex ITEMLOT_PARAM_ST
// -- ItemLotId1-8 +0x00 (s32), LotItemCategory1-8 +0x20 (u32),
// getItemFlagId +0x80 (s32), LotItemNum1-8 +0x8A (u8).
// Categories: 0 = weapon, 0x10000000 = armor, 0x20000000 = ring,
// 0x40000000 = goods (the Titanite Shard lot reads 0x40000000).
struct LotItem {
  uint32_t category;
  int32_t id;
  int count;
};
struct WorldPickup {
  int64_t lotId;
  uint32_t flagId;
  std::vector<LotItem> items;
};

std::vector<WorldPickup> LoadWorldPickups(HANDLE hProcess, uintptr_t moduleBase, int mapArea) {
  std::vector<WorldPickup> out;
  auto table = ResolveParamTable(hProcess, moduleBase, L"ItemLotParam");
  if (!table.ok) return out;
  SIZE_T br = 0;
  std::vector<unsigned char> index(static_cast<size_t>(table.rowCount) * 24);
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(table.tableBase + 0x40), index.data(),
                    index.size(), &br);
  unsigned char row[0x94] = {};
  for (uint16_t i = 0; i < table.rowCount; ++i) {
    int64_t id = 0, offset = 0;
    std::memcpy(&id, index.data() + static_cast<size_t>(i) * 24, 8);
    std::memcpy(&offset, index.data() + static_cast<size_t>(i) * 24 + 8, 8);
    if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(table.tableBase + offset), row,
                           sizeof(row), &br))
      continue;
    int32_t flag = 0;
    std::memcpy(&flag, row + 0x80, 4);
    if (flag < 50000000 || flag > 59999999 || flag / 100000 % 100 != mapArea) continue;
    WorldPickup p{id, static_cast<uint32_t>(flag), {}};
    for (int k = 0; k < 8; ++k) {
      int32_t itemId = 0;
      uint32_t category = 0;
      std::memcpy(&itemId, row + k * 4, 4);
      std::memcpy(&category, row + 0x20 + k * 4, 4);
      if (itemId <= 0) continue;
      p.items.push_back({category, itemId, row[0x8A + k] ? row[0x8A + k] : 1});
    }
    if (!p.items.empty()) out.push_back(std::move(p));
  }
  return out;
}

std::string LotItemName(const ItemNameTable& names, const LotItem& it) {
  int32_t uniquePrefix = kUniqueIdPrefixWeapon, giveId = it.id;
  switch (it.category) {
    case 0x10000000: uniquePrefix = kUniqueIdPrefixArmor; giveId = it.id + kGiveIdPrefixProtector; break;
    case 0x20000000: uniquePrefix = kUniqueIdPrefixAccessory; giveId = it.id + kGiveIdPrefixAccessory; break;
    case 0x40000000: uniquePrefix = kUniqueIdPrefixGoods; giveId = it.id + kGiveIdPrefixGoods; break;
    default: break;
  }
  std::string name = LookupItemName(names, uniquePrefix, giveId);
  if (name.empty()) name = "item " + std::to_string(it.id);
  if (it.category == 0 && it.id % 100) name += " +" + std::to_string(it.id % 100);
  if (it.count > 1) name += " x" + std::to_string(it.count);
  return name;
}

// "Titanite Shard, Ember x2"
std::string PickupLabel(const ItemNameTable& names, const WorldPickup& p) {
  std::string out;
  for (const auto& it : p.items) out += (out.empty() ? "" : ", ") + LotItemName(names, it);
  return out;
}

// One entry per pickup (= per flag). Live-observed 2026-10-06 on m40:
//   - consecutive lots can share a flag: one corpse giving a set (Pale
//     Shade Mask/Robe/Gloves/Trousers = lots 4000140-143, flag 54000140)
//     -> their items merge into one pickup;
//   - lots 200,000,000 above a base lot share its flag but give upgraded
//     items (Soul of a Champion for Soul of a Crestfallen Knight): the
//     New Game+ version of the same pickup. NG (clearCount 0) keeps the
//     base lots, NG+ prefers the 2xxxxxxxx ones where they exist.
struct GroupedPickup {
  uint32_t flagId;
  int64_t firstLot;
  WorldPickup merged;
};
constexpr int64_t kNgPlusLotOffset = 200000000;

std::vector<GroupedPickup> GroupPickups(const std::vector<WorldPickup>& lots, int clearCount) {
  std::unordered_map<uint32_t, bool> hasNgPlus;
  for (const auto& p : lots)
    if (p.lotId >= kNgPlusLotOffset) hasNgPlus[p.flagId] = true;
  std::vector<GroupedPickup> out;
  std::unordered_map<uint32_t, size_t> at;
  // +1/+2/+3 rings (accessory id not ending in 0: Life Ring 20000, Life
  // Ring+3 20003) are only placed in the world from NG+ on. The lot's own
  // ClearCount byte (+0x94) reads 0xFF ("any") even for them, so this is
  // a written rule, not game data: a pickup of only +N rings is hidden
  // on the first journey.
  auto onlyUpgradedRings = [](const WorldPickup& p) {
    for (const auto& it : p.items)
      if (it.category != 0x20000000 || it.id % 10 == 0) return false;
    return !p.items.empty();
  };
  for (const auto& p : lots) {
    bool ngPlusLot = p.lotId >= kNgPlusLotOffset;
    bool wantNgPlus = clearCount > 0 && hasNgPlus[p.flagId];
    if (ngPlusLot != wantNgPlus) continue;
    if (clearCount == 0 && onlyUpgradedRings(p)) continue;
    auto it = at.find(p.flagId);
    if (it == at.end()) {
      at[p.flagId] = out.size();
      out.push_back({p.flagId, p.lotId, p});
    } else {
      auto& g = out[it->second].merged.items;
      g.insert(g.end(), p.items.begin(), p.items.end());
    }
  }
  return out;
}

// NG cycle: GameDataMan + 0x78 (The Grand Archives' "ClearCount": 0 = NG).
constexpr uintptr_t kGameDataClearCountOffset = 0x78;
int ReadClearCount(HANDLE hProcess, uintptr_t moduleBase, uintptr_t baseACellOffset) {
  uintptr_t gameDataMan = 0;
  int32_t clearCount = 0;
  SIZE_T br = 0;
  if (!baseACellOffset ||
      !ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(moduleBase + baseACellOffset),
                         &gameDataMan, sizeof(gameDataMan), &br) || !gameDataMan)
    return 0;
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(gameDataMan + kGameDataClearCountOffset),
                    &clearCount, sizeof(clearCount), &br);
  return clearCount;
}

// Found count for the window/overlay; the grouped pickup list is cached
// per (map section, journey).
int CountFoundPickups(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize,
                      uintptr_t baseACellOffset, int mapArea, int* total) {
  static int cachedMap = -1, cachedClear = -1;
  static std::vector<GroupedPickup> cached;
  static EventFlagRoots roots;
  static bool haveRoots = false;
  if (!haveRoots) {
    roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
    haveRoots = roots.eventFlagManCell != 0;
  }
  int clearCount = ReadClearCount(hProcess, moduleBase, baseACellOffset);
  if (mapArea != cachedMap || clearCount != cachedClear) {
    cached = GroupPickups(LoadWorldPickups(hProcess, moduleBase, mapArea), clearCount);
    cachedMap = mapArea;
    cachedClear = clearCount;
  }
  int found = 0;
  for (const auto& g : cached)
    if (ReadEventFlag(hProcess, moduleBase, roots, g.flagId) > 0) ++found;
  *total = static_cast<int>(cached.size());
  return found;
}

std::string PickupHint(const ItemNameTable& names, const WorldPickup& p, int tier);
int32_t PickupRegion(const GroupedPickup& g);
const std::vector<GroupedPickup>& AllWorldPickups(HANDLE hProcess, uintptr_t moduleBase, int clearCount);

// items [--map NN]: the world pickups in the current named area (by each
// pickup's play region, from the map files), or in the given map section.
// Without map data, or in an area with no positioned pickups, it falls
// back to the current map section.
int RunItems(int mapOverride) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  int mapArea = mapOverride;
  std::string areaName;
  if (mapArea < 0) {
    auto resolved = ResolvePlayerHp(hProcess, moduleBase, moduleSize);
    PlayerArea area = ReadPlayerArea(hProcess, moduleBase, resolved.entityAddress, 0);
    if (!area.ok) {
      Log("No character loaded -- pass --map NN (e.g. --map 40).");
      CloseHandle(hProcess);
      return 6;
    }
    mapArea = static_cast<int>(area.mapId >> 24);
    areaName = AreaInfo().AreaOf(area.playRegionId);  // "" between regions -> map section
  }
  EventFlagRoots roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  ItemNameTable names = LoadItemNameTable(FindItemNameTablePath());
  auto profile = ResolvePlayerProfile(hProcess, moduleBase, moduleSize);
  int clearCount = ReadClearCount(hProcess, moduleBase, profile.baseACellOffset);
  std::vector<GroupedPickup> pickups;
  if (!areaName.empty()) {
    const AreaInfo areaInfo;
    for (const auto& g : AllWorldPickups(hProcess, moduleBase, clearCount))
      if (areaInfo.AreaOf(PickupRegion(g)) == areaName) pickups.push_back(g);
  }
  if (pickups.empty()) {
    areaName.clear();
    pickups = GroupPickups(LoadWorldPickups(hProcess, moduleBase, mapArea), clearCount);
  }
  int found = 0, unknown = 0;
  std::vector<std::string> foundLines, missingLines;
  const int tier = SpoilerTier();
  for (const auto& g : pickups) {
    int v = ReadEventFlag(hProcess, moduleBase, roots, g.flagId);
    char buf[400];
    // Found items are always named (you have them); unfound ones follow
    // the spoiler tier, and below Category they're only counted.
    std::string label = v > 0 ? PickupLabel(names, g.merged) : PickupHint(names, g.merged, tier);
    std::snprintf(buf, sizeof(buf), "  %s %-60s flag %u (lot %lld)", v > 0 ? "[x]" : v == 0 ? "[ ]" : "[?]",
                  label.c_str(), g.flagId, static_cast<long long>(g.firstLot));
    if (v > 0) { ++found; foundLines.push_back(buf); }
    else { if (v < 0) ++unknown; missingLines.push_back(buf); }
  }
  std::string scope = areaName.empty() ? "Map m" + std::to_string(mapArea) : areaName;
  Log("%s (journey %d): %d of %zu world pickups found%s.", scope.c_str(), clearCount + 1, found,
      pickups.size(), unknown ? " (some flags unreadable)" : "");
  if (tier >= kTierCategory) {
    Log("Not found yet%s:", tier < kTierFull ? " (spoiler tier Category; --full for names)" : "");
    for (const auto& l : missingLines) Log("%s", l.c_str());
  } else {
    Log("Not found yet: %zu (spoiler tier %s hides them; F9 in game or --full to show).", missingLines.size(),
        SpoilerTierName(tier));
  }
  Log("Found:");
  for (const auto& l : foundLines) Log("%s", l.c_str());
  CloseHandle(hProcess);
  return 0;
}

// ---- bosses and progression ------------------------------------------
//
// data/bosses.tsv: flag, area, required/optional, note (the area from where
// the boss stands in the game's map files; sources in the file header).
// The name is the one the game puts on the boss's health bar, from the
// generated boss_names.tsv; "boss <flag>" until
// the map data has been read. Defeated = the flag is ON.
// Shards: world pickups (any map section) whose lot gives Estus Shard
// (goods 2141) or Undead Bone Shard (goods 2143); found = flag ON. Only
// world pickups count -- a shard handed over by an NPC wouldn't be in
// the total.
const std::unordered_map<int32_t, std::string>& BossNames();  // defined with the map-data tables

std::vector<BossInfo> LoadBosses() {
  std::vector<BossInfo> out;
  const auto& names = BossNames();
  std::ifstream in(Paths().Data(L"bosses.tsv"));
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f;
    size_t pos = 0;
    while (true) {
      size_t tab = line.find('\t', pos);
      f.push_back(line.substr(pos, tab == std::string::npos ? std::string::npos : tab - pos));
      if (tab == std::string::npos) break;
      pos = tab + 1;
    }
    if (f.size() < 3) continue;
    uint32_t flag = static_cast<uint32_t>(std::strtoul(f[0].c_str(), nullptr, 10));
    auto it = names.find(static_cast<int32_t>(flag));
    out.push_back({flag, it != names.end() ? it->second : "boss " + f[0], f[1], f[2], f.size() > 3 ? f[3] : ""});
  }
  return out;
}

// The boss list for the live loop: reloaded with the map data, so names
// appear once a first-run read is done (LoadBosses for one-off commands).
const std::vector<BossInfo>& Bosses();

struct ShardCount {
  int estusFound = 0, estusTotal = 0, boneFound = 0, boneTotal = 0;
};
constexpr int32_t kGoodsEstusShard = 2141;
constexpr int32_t kGoodsUndeadBoneShard = 2143;

// All map sections' pickups, grouped; cached by journey (the lot table
// doesn't change while the game runs).
const std::vector<GroupedPickup>& AllWorldPickups(HANDLE hProcess, uintptr_t moduleBase, int clearCount) {
  static int cachedClear = -1;
  static std::vector<GroupedPickup> cached;
  if (clearCount != cachedClear) {
    std::vector<WorldPickup> all;
    for (int area = 30; area <= 55; ++area) {
      auto lots = LoadWorldPickups(hProcess, moduleBase, area);
      all.insert(all.end(), lots.begin(), lots.end());
    }
    cached = GroupPickups(all, clearCount);
    cachedClear = clearCount;
  }
  return cached;
}

ShardCount CountShards(HANDLE hProcess, uintptr_t moduleBase, const EventFlagRoots& roots,
                       int clearCount) {
  ShardCount c;
  for (const auto& g : AllWorldPickups(hProcess, moduleBase, clearCount)) {
    bool estus = false, bone = false;
    for (const auto& it : g.merged.items) {
      if (it.category != 0x40000000) continue;
      estus |= it.id == kGoodsEstusShard;
      bone |= it.id == kGoodsUndeadBoneShard;
    }
    if (!estus && !bone) continue;
    bool found = ReadEventFlag(hProcess, moduleBase, roots, g.flagId) > 0;
    if (estus) { ++c.estusTotal; c.estusFound += found; }
    if (bone) { ++c.boneTotal; c.boneFound += found; }
  }
  return c;
}

// Defined with the route (further down).
bool BossIsLater(HANDLE hProcess, uintptr_t moduleBase, const EventFlagRoots& roots, uint32_t flag,
                 std::string* opensAfter = nullptr);

// "Iudex Gundyr (defeated)" / "Vordt of the Boreal Valley (defeated),
// Dancer of the Boreal Valley (later)" -- for one area; "" when it has
// none. Counts leave out "later" bosses (see BossIsLater); *laterOut gets
// how many there are.
std::string BossSummary(HANDLE hProcess, uintptr_t moduleBase, const EventFlagRoots& roots,
                        const std::vector<BossInfo>& bosses, const std::string& area,
                        int* defeatedOut, int* totalOut, int* laterOut = nullptr) {
  int total = 0, defeated = 0, later = 0;
  std::string names, defeatedNames;
  for (const auto& b : bosses) {
    if (b.area != area) continue;
    bool dead = ReadEventFlag(hProcess, moduleBase, roots, b.flag) > 0;
    bool isLater = !dead && BossIsLater(hProcess, moduleBase, roots, b.flag);
    if (isLater) {
      ++later;
    } else {
      ++total;
      defeated += dead;
    }
    names += (names.empty() ? "" : ", ") + b.name +
             (dead ? " (defeated)" : isLater ? " (later)" : b.status == "optional" ? " (optional)" : "");
    if (dead) defeatedNames += (defeatedNames.empty() ? "" : ", ") + b.name;
  }
  if (defeatedOut) *defeatedOut = defeated;
  if (totalOut) *totalOut = total;
  if (laterOut) *laterOut = later;
  // Spoiler tiers: Full names all. Below Full, defeated bosses are still
  // named (you've seen them -- user's call, 2026-10-06) and the rest are
  // counted: "2 bosses here: 1 defeated (Vordt), 1 not yet".
  const int tier = SpoilerTier();
  if (tier >= kTierFull || total + later == 0) return names;
  std::string out = std::to_string(total) + (total == 1 ? " boss here: " : " bosses here: ") +
                    std::to_string(defeated) + " defeated" + (defeated ? " (" + defeatedNames + ")" : "");
  if (total > defeated) out += ", " + std::to_string(total - defeated) + " not yet";
  if (later) out += ", " + std::to_string(later) + " later in the game";
  return out;
}

void WindowProgress(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize,
                    uintptr_t baseACellOffset, const std::string& area, std::string* bossesOut,
                    int* bossesDefeated, int* bossesTotal, int* bossesLater, std::string* shardsOut) {
  const std::vector<BossInfo>& bosses = Bosses();
  static EventFlagRoots roots;
  static bool haveRoots = false;
  if (!haveRoots) {
    roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
    haveRoots = roots.eventFlagManCell != 0;
  }
  *bossesOut = BossSummary(hProcess, moduleBase, roots, bosses, area, bossesDefeated, bossesTotal, bossesLater);
  ShardCount c = CountShards(hProcess, moduleBase, roots, ReadClearCount(hProcess, moduleBase, baseACellOffset));
  *shardsOut = "Estus " + std::to_string(c.estusFound) + "/" + std::to_string(c.estusTotal) +
               " | Bone " + std::to_string(c.boneFound) + "/" + std::to_string(c.boneTotal);
}

int RunProgress() {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  EventFlagRoots roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  auto profile = ResolvePlayerProfile(hProcess, moduleBase, moduleSize);
  int clearCount = ReadClearCount(hProcess, moduleBase, profile.baseACellOffset);
  auto bosses = LoadBosses();
  if (bosses.empty()) Log("data/bosses.tsv missing or empty.");

  int baseDefeated = 0, baseTotal = 0, requiredLeft = 0;
  std::string lastArea;
  Log("Bosses (journey %d):", clearCount + 1);
  // Grouped by area, areas in order of first appearance (the file lists
  // bosses in fight order, which revisits High Wall and Lothric Castle).
  std::vector<std::string> areaOrder;
  for (const auto& b : bosses)
    if (std::find(areaOrder.begin(), areaOrder.end(), b.area) == areaOrder.end()) areaOrder.push_back(b.area);
  std::vector<BossInfo> ordered;
  for (const auto& a : areaOrder)
    for (const auto& b : bosses)
      if (b.area == a) ordered.push_back(b);
  // Spoiler tiers: Full lists every boss. Below Full, only areas where
  // you've defeated a boss are listed, naming the defeated and hiding the
  // rest.
  const int tier = SpoilerTier();
  std::unordered_map<std::string, bool> areaSeen;
  for (const auto& b : ordered)
    if (ReadEventFlag(hProcess, moduleBase, roots, b.flag) > 0) areaSeen[b.area] = true;
  int hiddenAreas = 0;
  for (const auto& b : ordered) {
    int v = ReadEventFlag(hProcess, moduleBase, roots, b.flag);
    bool dlc = b.note.find("DLC") != std::string::npos;
    if (!dlc) {
      ++baseTotal;
      baseDefeated += v > 0;
      requiredLeft += (v <= 0 && b.status == "required");
    }
    if (tier < kTierFull && !areaSeen.count(b.area)) {
      if (b.area != lastArea) ++hiddenAreas;
      lastArea = b.area;
      continue;
    }
    if (b.area != lastArea) {
      Log("  %s", b.area.c_str());
      lastArea = b.area;
    }
    std::string opensAfter;
    bool later = v == 0 && BossIsLater(hProcess, moduleBase, roots, b.flag, &opensAfter);
    if (tier < kTierFull && v <= 0) {
      Log("    [ ] (not yet defeated%s)", later ? ", later in the game" : "");
      continue;
    }
    std::string note = later ? "later: opens after " + opensAfter + (b.note.empty() ? "" : " | " + b.note) : b.note;
    Log("    %s %-48s %-8s %s", v > 0 ? "[x]" : v == 0 ? "[ ]" : "[?]", b.name.c_str(), b.status.c_str(),
        note.c_str());
  }
  if (tier < kTierFull)
    Log("  (spoiler tier %s: %d area(s) not shown; F9 in game or --full to show everything)",
        SpoilerTierName(tier), hiddenAreas);
  Log("Base game: %d of %d bosses defeated, %d required left.", baseDefeated, baseTotal, requiredLeft);
  ShardCount s = CountShards(hProcess, moduleBase, roots, clearCount);
  Log("Estus Shards: %d of %d found. Undead Bone Shards: %d of %d found (world pickups).",
      s.estusFound, s.estusTotal, s.boneFound, s.boneTotal);
  CloseHandle(hProcess);
  return 0;
}

// ---- key items --------------------------------------------------------
//
// data/key_items.tsv: goods id, kind (key / tome / quest / shop / other),
// what it does -- the game's own key-item category plus Loretta's Bone.
// Names come from the generated item names
// (ItemNames); rows sharing a name (Tower Key, Cinders of a Lord) are one
// item. State per item:
//   held     -- in the inventory now
//   obtained -- not held, but a world pickup giving it has its flag ON
//               (used up: e.g. Cinders of a Lord leave once placed)
//   not yet  -- neither
// Key items given by NPCs or bosses have no pickup, so only "held" can
// be known for them.
struct KeyItem {
  int32_t goodsId;
  std::string name, kind, opens;
};

std::vector<KeyItem> LoadKeyItems() {
  std::vector<KeyItem> out;
  const ItemNameTable& names = ItemNames();
  std::ifstream in(Paths().Data(L"key_items.tsv"));
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f;
    size_t pos = 0;
    while (true) {
      size_t tab = line.find('\t', pos);
      f.push_back(line.substr(pos, tab == std::string::npos ? std::string::npos : tab - pos));
      if (tab == std::string::npos) break;
      pos = tab + 1;
    }
    if (f.size() < 3) continue;
    int32_t id = std::atoi(f[0].c_str());
    auto it = names.goods.find(f[0]);
    out.push_back({id, it != names.goods.end() ? it->second : "goods " + f[0], f[1], f[2]});
  }
  return out;
}

// The key item list for the live loop: reloaded with the map data, so
// names appear once a first-run read is done (LoadKeyItems for commands).
const std::vector<KeyItem>& KeyItems();  // defined with the map-data tables

// Goods ids currently in the inventory.
std::unordered_map<int32_t, bool> HeldGoods(HANDLE hProcess, const ResolvedProfile& profile) {
  std::unordered_map<int32_t, bool> held;
  if (profile.xBase == 0) return held;
  uintptr_t equipInventoryData = profile.xBase + kEquipGameDataOffset + kEquipInventoryDataOffset;
  InventoryLayout layout = ResolveInventoryLayout(hProcess, equipInventoryData);
  if (!layout.ok) return held;
  for (int32_t id = 0; id < layout.capacity; ++id) {
    auto item = ResolveItemFast(hProcess, layout, id);
    if (!item.ok || !item.plausible) continue;
    if ((item.uniqueId & static_cast<int32_t>(0xF0000000)) != static_cast<int32_t>(0xB0000000)) continue;
    held[item.giveId - kGiveIdPrefixGoods] = true;
  }
  return held;
}

struct KeyItemState {
  const KeyItem* item;
  bool held = false, obtained = false;
  int mapArea = -1;  // map section of a world pickup giving it, -1 if none
};

std::vector<KeyItemState> KeyItemStates(HANDLE hProcess, uintptr_t moduleBase, const EventFlagRoots& roots,
                                        const ResolvedProfile& profile, int clearCount,
                                        const std::vector<KeyItem>& keys) {
  auto held = HeldGoods(hProcess, profile);
  const auto& pickups = AllWorldPickups(hProcess, moduleBase, clearCount);
  std::vector<KeyItemState> out;
  for (const auto& k : keys) {
    KeyItemState st{&k};
    st.held = held.count(k.goodsId) > 0;
    for (const auto& g : pickups) {
      bool gives = false;
      for (const auto& it : g.merged.items) gives |= it.category == 0x40000000 && it.id == k.goodsId;
      if (!gives) continue;
      st.mapArea = static_cast<int>(g.flagId / 100000 % 100);
      st.obtained |= ReadEventFlag(hProcess, moduleBase, roots, g.flagId) > 0;
    }
    st.obtained |= st.held;
    // One entry per name: Tower Key has two goods ids, Cinders of a Lord
    // four (one per Lord) -- held/obtained if any of them is.
    auto same = std::find_if(out.begin(), out.end(),
                             [&](const KeyItemState& o) { return o.item->name == k.name; });
    if (same != out.end()) {
      same->held |= st.held;
      same->obtained |= st.obtained;
      if (same->mapArea < 0) same->mapArea = st.mapArea;
      continue;
    }
    out.push_back(st);
  }
  return out;
}

std::string WindowKeyItems(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize,
                           const ResolvedProfile& profile) {
  const std::vector<KeyItem>& keys = KeyItems();
  static EventFlagRoots roots;
  static bool haveRoots = false;
  if (!haveRoots) {
    roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
    haveRoots = roots.eventFlagManCell != 0;
  }
  auto states = KeyItemStates(hProcess, moduleBase, roots, profile,
                              ReadClearCount(hProcess, moduleBase, profile.baseACellOffset), keys);
  int obtained = 0, held = 0;
  for (const auto& st : states) { obtained += st.obtained; held += st.held; }
  return std::to_string(obtained) + " / " + std::to_string(states.size()) + " key items (" +
         std::to_string(held) + " held)";
}

int RunKeys() {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  EventFlagRoots roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  auto profile = ResolvePlayerProfile(hProcess, moduleBase, moduleSize);
  int clearCount = ReadClearCount(hProcess, moduleBase, profile.baseACellOffset);
  auto keys = LoadKeyItems();
  if (keys.empty()) Log("data/key_items.tsv missing or empty.");
  auto states = KeyItemStates(hProcess, moduleBase, roots, profile, clearCount, keys);
  int obtained = 0, hiddenKeys = 0;
  const int tier = SpoilerTier();
  for (const char* kind : {"key", "quest", "tome", "shop", "other"}) {
    bool header = false;  // printed before the first shown item, so hidden kinds stay silent
    for (const auto& st : states) {
      if (st.item->kind != kind) continue;
      obtained += st.obtained;
      // Key items you have are always named; the rest only at Full.
      if (!st.obtained && tier < kTierFull) {
        ++hiddenKeys;
        continue;
      }
      if (!header) {
        Log("%s:", std::string(kind) == "key" ? "Keys" : std::string(kind) == "quest" ? "Quest items"
                                                    : std::string(kind) == "tome" ? "Spell tomes and scrolls"
                                                    : std::string(kind) == "shop" ? "Smith, merchant and Ludleth"
                                                                                  : "Other");
        header = true;
      }
      char where[16] = "";
      if (st.mapArea >= 0) std::snprintf(where, sizeof(where), "m%02d", st.mapArea);
      Log("  %-8s %-32s %-4s %s", st.held ? "held" : st.obtained ? "obtained" : "-", st.item->name.c_str(),
          where, st.item->opens.c_str());
    }
  }
  if (hiddenKeys)
    Log("%d not yet obtained, hidden at spoiler tier %s (F9 in game or --full to show).", hiddenKeys,
        SpoilerTierName(tier));
  Log("%d of %zu key items obtained (map section shown where the item is a world pickup).", obtained,
      states.size());
  CloseHandle(hProcess);
  return 0;
}

// ---- upgrade tracker --------------------------------------------------
//
// Materials for a weapon's next level come from EquipMtrlSetParam row
//   weapon.materialSetId (EquipParamWeapon +0x58, s32)
//   + reinforce.materialSetId (ReinforceParamWeapon row
//     reinforceTypeId + nextLevel, +0x56, u8)
// Live-checked 2026-10-06 on a Long Sword (material set 0; level offsets
// 1..10 = the level): rows 1-3 = Titanite Shard x2/x4/x6, 4-6 = Large
// Titanite Shard x2/x4/x6, 7-9 = Titanite Chunk x2/x4/x6, 10 = Titanite
// Slab x1 -- the shard counts match the Fextralife Titanite Shard page
// ("+1: 2 shards, +2: 4 shards, +3: 6 shards"). Material-set layout:
// Paramdex EQUIP_MTRL_SET_PARAM_ST (MaterialId01-05 s32 +0x00, ItemNum01-05
// s8 +0x14). Max level = no reinforce row for the next level.
constexpr uintptr_t kWeaponMaterialSetOffset = 0x58;          // s32
constexpr uintptr_t kReinforceMaterialSetOffset = 0x56;       // u8

struct UpgradeNeed {
  int32_t goodsId;
  int need, have;
};
struct UpgradeInfo {
  bool ok = false, maxed = false;
  std::string weaponName;
  int level = 0;
  std::vector<UpgradeNeed> materials;
};

// Goods id -> quantity held.
std::unordered_map<int32_t, int> HeldGoodsQty(HANDLE hProcess, const ResolvedProfile& profile) {
  std::unordered_map<int32_t, int> qty;
  if (profile.xBase == 0) return qty;
  InventoryLayout layout =
      ResolveInventoryLayout(hProcess, profile.xBase + kEquipGameDataOffset + kEquipInventoryDataOffset);
  if (!layout.ok) return qty;
  for (int32_t id = 0; id < layout.capacity; ++id) {
    auto item = ResolveItemFast(hProcess, layout, id);
    if (!item.ok || !item.plausible) continue;
    if ((item.uniqueId & static_cast<int32_t>(0xF0000000)) != static_cast<int32_t>(0xB0000000)) continue;
    qty[item.giveId - kGiveIdPrefixGoods] += static_cast<int>(item.quantity);
  }
  return qty;
}

UpgradeInfo WeaponUpgrade(HANDLE hProcess, uintptr_t moduleBase, const ItemNameTable& names,
                          int32_t giveId, const std::unordered_map<int32_t, int>& held) {
  UpgradeInfo u;
  int level = giveId % 100;
  int32_t baseRow = giveId - level;
  u.level = level;
  u.weaponName = LookupItemName(names, kUniqueIdPrefixWeapon, giveId);
  auto weapons = ResolveParamTable(hProcess, moduleBase, L"EquipParamWeapon");
  uintptr_t row = FindParamRow(hProcess, weapons, baseRow);
  if (!row) return u;
  int32_t materialSet = 0;
  int16_t reinforceType = 0;
  SIZE_T br = 0;
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(row + kWeaponMaterialSetOffset), &materialSet, 4, &br);
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(row + kWeaponReinforceTypeIdOffset), &reinforceType, 2, &br);
  auto reinforce = ResolveParamTable(hProcess, moduleBase, L"ReinforceParamWeapon");
  uintptr_t next = FindParamRow(hProcess, reinforce, static_cast<int64_t>(reinforceType) + level + 1);
  u.ok = true;
  if (!next) { u.maxed = true; return u; }
  uint8_t levelSet = 0;
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(next + kReinforceMaterialSetOffset), &levelSet, 1, &br);
  auto sets = ResolveParamTable(hProcess, moduleBase, L"EquipMtrlSetParam");
  uintptr_t set = FindParamRow(hProcess, sets, static_cast<int64_t>(materialSet) + levelSet);
  if (!set) { u.ok = false; return u; }
  int32_t ids[5] = {};
  int8_t counts[5] = {};
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(set), ids, sizeof(ids), &br);
  ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(set + 0x14), counts, sizeof(counts), &br);
  for (int k = 0; k < 5; ++k) {
    if (ids[k] <= 0 || counts[k] <= 0) continue;
    auto it = held.find(ids[k]);
    u.materials.push_back({ids[k], counts[k], it == held.end() ? 0 : it->second});
  }
  return u;
}

std::string GoodsName(const ItemNameTable& names, int32_t goodsId) {
  std::string n = LookupItemName(names, kUniqueIdPrefixGoods, goodsId + kGiveIdPrefixGoods);
  return n.empty() ? "goods " + std::to_string(goodsId) : n;
}

// "Long Sword +0 -> +1: Titanite Shard 1/2" / "... is at max (+10)"
std::string UpgradeLine(const ItemNameTable& names, const UpgradeInfo& u) {
  if (!u.ok) return u.weaponName + ": upgrade data not found";
  if (u.maxed) return u.weaponName + " +" + std::to_string(u.level) + " (max)";
  std::string out = u.weaponName + " +" + std::to_string(u.level) + " -> +" + std::to_string(u.level + 1) + ":";
  bool ready = true;
  for (const auto& m : u.materials) {
    out += " " + GoodsName(names, m.goodsId) + " " + std::to_string(m.have) + "/" + std::to_string(m.need);
    ready &= m.have >= m.need;
  }
  if (u.materials.empty()) out += " no materials";
  else if (ready) out += " (ready)";
  return out;
}

std::string WindowUpgrade(HANDLE hProcess, uintptr_t moduleBase, const ResolvedProfile& profile,
                          int32_t inventoryItemId) {
  const ItemNameTable& names = ItemNames();
  auto item = ResolveItem(hProcess, profile.xBase + kEquipGameDataOffset + kEquipInventoryDataOffset,
                          inventoryItemId);
  if (!item.ok || !item.plausible || item.giveId == 110000) return "";
  auto u = WeaponUpgrade(hProcess, moduleBase, names, item.giveId, HeldGoodsQty(hProcess, profile));
  // Window width: "+0 -> +1: Titanite Shard 1/2", without the weapon name
  // (the Right line above already shows it).
  if (!u.ok) return "--";
  if (u.maxed) return "+" + std::to_string(u.level) + " (max)";
  std::string out = "+" + std::to_string(u.level) + " -> +" + std::to_string(u.level + 1) + ":";
  for (const auto& m : u.materials)
    out += " " + GoodsName(names, m.goodsId) + " " + std::to_string(m.have) + "/" + std::to_string(m.need);
  return out;
}

int RunUpgrades() {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  auto profile = ResolvePlayerProfile(hProcess, moduleBase, moduleSize);
  if (!profile.xBase) {
    Log("No character loaded.");
    CloseHandle(hProcess);
    return 6;
  }
  ItemNameTable names = LoadItemNameTable(FindItemNameTablePath());
  auto held = HeldGoodsQty(hProcess, profile);
  uintptr_t equipInventoryData = profile.xBase + kEquipGameDataOffset + kEquipInventoryDataOffset;
  Log("Next upgrade for each equipped weapon (materials held / needed):");
  for (const auto& slot : BuildEquipSlotList(profile)) {
    std::string n = slot.name;
    if (n.size() != 2 || (n[0] != 'R' && n[0] != 'L')) continue;
    int32_t id = -1;
    SIZE_T br = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(slot.address), &id, sizeof(id), &br);
    if (id < 0) continue;
    auto item = ResolveItem(hProcess, equipInventoryData, id);
    if (!item.ok || !item.plausible || item.giveId == 110000) continue;  // empty / Fists
    Log("  %s  %s", slot.name, UpgradeLine(names, WeaponUpgrade(hProcess, moduleBase, names, item.giveId, held)).c_str());
  }
  CloseHandle(hProcess);
  return 0;
}

// ---- pickup positions from the map files ---------------
//
// data/generated/treasures.tsv is made locally by tools/extract_treasures.py
// from the user's own install (map, lot, lot2, x, y, z, part). Each MSB
// Treasure event names an ItemLotParam row and the map part it sits on;
// the part's position is the pickup's. Coordinates share one space with
// the player's (continuous across map borders). Grouped
// pickups take the position of their first lot (or, for an NG+ lot, the
// base lot 200,000,000 below it).
struct TreasurePos {
  float x, y, z;
  std::string map;
  int32_t region;  // play region the pickup stands in (-1 unknown), for per-area items
};

// Map-data versions: the app window's background
// extraction bumps g_mapDataVersion when it has written new files; the poll
// worker adopts it at the top of a tick (AdoptNewMapData), the one point
// where no reader holds a reference into the tables, and the tables reload
// on their next use. Commands never bump it, so they load once as before.
volatile LONG g_mapDataVersion = 0;
LONG g_mapDataVersionInUse = 0;
void AdoptNewMapData() { g_mapDataVersionInUse = g_mapDataVersion; }
void BumpMapDataVersion() { InterlockedIncrement(&g_mapDataVersion); }

// The generated name tables reload the same way, so
// names appear as soon as a first-run read finishes, without a restart.
const ItemNameTable& ItemNames() {
  static ItemNameTable table;
  static LONG loadedVersion = -1;
  if (loadedVersion != g_mapDataVersionInUse) {
    loadedVersion = g_mapDataVersionInUse;
    table = LoadItemNameTable(FindItemNameTablePath());
  }
  return table;
}

const std::unordered_map<int32_t, std::string>& BonfireNames() {
  static std::unordered_map<int32_t, std::string> table;
  static LONG loadedVersion = -1;
  if (loadedVersion != g_mapDataVersionInUse) {
    loadedVersion = g_mapDataVersionInUse;
    table = LoadIdNameTable(Paths().Generated(L"bonfire_names.tsv"));
  }
  return table;
}

// Boss names as on their health bars, by flag.
const std::unordered_map<int32_t, std::string>& BossNames() {
  static std::unordered_map<int32_t, std::string> table;
  static LONG loadedVersion = -1;
  if (loadedVersion != g_mapDataVersionInUse) {
    loadedVersion = g_mapDataVersionInUse;
    table = LoadIdNameTable(Paths().Generated(L"boss_names.tsv"));
  }
  return table;
}

const std::vector<BossInfo>& Bosses() {
  static std::vector<BossInfo> list;
  static LONG loadedVersion = -1;
  if (loadedVersion != g_mapDataVersionInUse) {
    loadedVersion = g_mapDataVersionInUse;
    list = LoadBosses();
  }
  return list;
}

const std::vector<KeyItem>& KeyItems() {
  static std::vector<KeyItem> list;
  static LONG loadedVersion = -1;
  if (loadedVersion != g_mapDataVersionInUse) {
    loadedVersion = g_mapDataVersionInUse;
    list = LoadKeyItems();
  }
  return list;
}

// Region labels, e.g. "Undead Settlement (near Cliff Underside)": the area
// and the nearest bonfire.
const std::unordered_map<int32_t, std::string>& RegionNames() {
  static std::unordered_map<int32_t, std::string> table;
  static LONG loadedVersion = -1;
  if (loadedVersion != g_mapDataVersionInUse) {
    loadedVersion = g_mapDataVersionInUse;
    table = LoadIdNameTable(Paths().Generated(L"regions.tsv"));
  }
  return table;
}

const std::unordered_map<int64_t, TreasurePos>& TreasurePositions() {
  static std::unordered_map<int64_t, TreasurePos> table;
  static LONG loadedVersion = -1;
  if (loadedVersion == g_mapDataVersionInUse) return table;
  loadedVersion = g_mapDataVersionInUse;
  table.clear();
  std::ifstream in(Paths().Generated(L"treasures.tsv"));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    // map \t lot \t lot2 \t x \t y \t z \t region \t part
    if (line.size() < 4) continue;
    std::string map = line.substr(0, 3);
    const char* p = line.c_str() + line.find('\t');
    char* end = nullptr;
    long long lot = std::strtoll(p, &end, 10);
    long long lot2 = std::strtoll(end, &end, 10);
    float x = std::strtof(end, &end);
    float y = std::strtof(end, &end);
    float z = std::strtof(end, &end);
    long region = std::strtol(end, &end, 10);  // 0 from an old file without the column
    if (std::isnan(x)) continue;  // treasure whose part wasn't found
    for (long long l : {lot, lot2})
      if (l > 0 && !table.count(l)) table[l] = {x, y, z, map, static_cast<int32_t>(region > 0 ? region : -1)};
  }
  return table;
}

const TreasurePos* PickupPosition(const GroupedPickup& g) {
  const auto& t = TreasurePositions();
  auto it = t.find(g.firstLot);
  if (it == t.end() && g.firstLot >= kNgPlusLotOffset) it = t.find(g.firstLot - kNgPlusLotOffset);
  return it == t.end() ? nullptr : &it->second;
}

int32_t PickupRegion(const GroupedPickup& g) {
  const TreasurePos* p = PickupPosition(g);
  return p ? p->region : -1;
}

// Untended Graves / Dark Firelink (play regions 4000xx) share m40's
// geometry with the Cemetery of Ash / Firelink Shrine (4001xx), so their
// pickups overlap in space: show each world's pickups only while the
// player is in that world (unknown player region = the normal world).
bool SameWorld(int32_t pickupRegion, int32_t playerRegion) {
  auto untended = [](int32_t r) { return r >= 400000 && r < 400100; };
  auto cemetery = [](int32_t r) { return r >= 400100 && r < 400200; };
  if (untended(pickupRegion)) return untended(playerRegion);
  if (cemetery(pickupRegion)) return !untended(playerRegion);
  return true;
}

// Found / total world pickups standing in the named area (via each
// pickup's play region). Returns -1 when the area has no positioned
// pickups (no map data, or an area without any), so callers can fall
// back to the map-section count.
int CountFoundInArea(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
                     const std::string& area, int* total) {
  static const AreaInfo areaInfo;
  static EventFlagRoots roots;
  if (area.empty() || TreasurePositions().empty()) return -1;
  if (!roots.eventFlagManCell) roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  int clearCount = ReadClearCount(hProcess, moduleBase, baseACellOffset);
  int found = 0, count = 0;
  for (const auto& g : AllWorldPickups(hProcess, moduleBase, clearCount)) {
    if (areaInfo.AreaOf(PickupRegion(g)) != area) continue;
    ++count;
    if (ReadEventFlag(hProcess, moduleBase, roots, g.flagId) > 0) ++found;
  }
  if (count == 0) return -1;
  *total = count;
  return found;
}

// Direction as a clock face, 12 = straight ahead. The facing angle
// points the opposite way to atan2(dx, dz): live-checked 2026-10-06, the
// first version (no pi) put a Homeward Bone at 4 o'clock that was really
// at 10 -- rotated 180 degrees.
double RelativeBearing(const PlayerPose& pose, float dx, float dz) {
  const double kPi = 3.141592653589793, kTwoPi = 2 * kPi;
  double rel = std::atan2(dx, dz) - pose.facingRadians + kPi;
  return std::fmod(std::fmod(rel, kTwoPi) + kTwoPi, kTwoPi);  // 0..2pi, clockwise from ahead
}

std::string ClockDirection(const PlayerPose& pose, float dx, float dz) {
  const double kTwoPi = 6.283185307179586;
  double rel = RelativeBearing(pose, dx, dz);
  int hour = static_cast<int>(std::lround(rel / kTwoPi * 12.0)) % 12;
  return std::to_string(hour == 0 ? 12 : hour) + " o'clock";
}

struct NearbyPickup {
  const GroupedPickup* g;
  const TreasurePos* pos;
  float dist, dy;
};

// Unfound world pickups (all map sections) sorted by distance.
// Each map section has its own coordinate space -- they overlap in number
// (m30 High Wall reaches z 499, m40 Cemetery spans z 445-645; found
// 2026-10-06 when High Wall's Titanite Scales showed up 73 m from the
// player in the Cemetery) and only line up across a connected border
// (m33 -> m31). So only the player's own map section counts.
std::vector<NearbyPickup> NearestUnfound(HANDLE hProcess, uintptr_t moduleBase, const EventFlagRoots& roots,
                                         int clearCount, const PlayerPose& pose, size_t limit,
                                         const PlayerArea& where) {
  std::vector<NearbyPickup> out;
  const std::string map = where.ok ? where.MapName().substr(0, 3) : "";
  for (const auto& g : AllWorldPickups(hProcess, moduleBase, clearCount)) {
    const TreasurePos* p = PickupPosition(g);
    if (!p || (!map.empty() && p->map != map) || !SameWorld(p->region, where.playRegionId)) continue;
    float dx = p->x - pose.x, dy = p->y - pose.y, dz = p->z - pose.z;
    out.push_back({&g, p, std::sqrt(dx * dx + dy * dy + dz * dz), dy});
  }
  std::sort(out.begin(), out.end(), [](const NearbyPickup& a, const NearbyPickup& b) { return a.dist < b.dist; });
  std::vector<NearbyPickup> unfound;
  for (const auto& n : out) {
    if (unfound.size() >= limit) break;
    if (ReadEventFlag(hProcess, moduleBase, roots, n.g->flagId) > 0) continue;
    unfound.push_back(n);
  }
  return unfound;
}

// What a pickup is, without naming it (spoiler tier Category).
std::string PickupCategory(const WorldPickup& p) {
  static std::unordered_map<int32_t, bool> keyIds = [] {
    std::unordered_map<int32_t, bool> m;
    for (const auto& k : LoadKeyItems()) m[k.goodsId] = true;
    return m;
  }();
  auto kindOf = [&](const LotItem& it) -> std::string {
    switch (it.category) {
      case 0: return "a weapon or shield";
      case 0x10000000: return "armour";
      case 0x20000000: return "a ring";
      case 0x40000000: break;
      default: return "an item";
    }
    int id = it.id;
    if (keyIds.count(id)) return "a key item";
    if (id == kGoodsEstusShard || id == kGoodsUndeadBoneShard) return "a flask upgrade";
    if (id >= 1000 && id < 1300) return "upgrade material";
    if (id >= 1200000) return "a spell";
    if (id >= 9000 && id < 10000) return "a gesture";
    if ((id >= 400 && id < 500) || (id >= 700 && id < 800)) return "souls";
    return "a consumable";
  };
  std::string kind;
  for (const auto& it : p.items) {
    std::string k = kindOf(it);
    if (kind.empty()) kind = k;
    else if (kind != k) return "several items";
  }
  if (kind == "armour" && p.items.size() > 1) return "armour (" + std::to_string(p.items.size()) + " pieces)";
  return kind.empty() ? "an item" : kind;
}

// An unfound pickup at the spoiler tier: Full = names, Category = kind,
// Vague / Off = "something".
std::string PickupHint(const ItemNameTable& names, const WorldPickup& p, int tier) {
  if (tier >= kTierFull) return PickupLabel(names, p);
  if (tier == kTierCategory) return PickupCategory(p);
  return "something unfound";
}

// Window line "Homeward Bone  37 m, 4 o'clock, +2 m" and the overlay's
// shorter "37 m 4 o'clock: Homeward Bone", at the spoiler tier (Vague
// leaves out the height). Once a second.
void WindowNearest(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
                   const PlayerPose& pose, const PlayerArea& where, std::string* windowLine,
                   std::string* overlayLine) {
  const ItemNameTable& names = ItemNames();
  static EventFlagRoots roots;
  const int tier = SpoilerTier();
  if (tier == kTierOff) {
    *windowLine = "hidden (spoiler tier Off)";
    *overlayLine = "";
    return;
  }
  if (!roots.eventFlagManCell) roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  if (TreasurePositions().empty()) {
    *windowLine = *overlayLine = "no positions (run tools\\extract_treasures.py)";
    return;
  }
  int clearCount = ReadClearCount(hProcess, moduleBase, baseACellOffset);
  auto n = NearestUnfound(hProcess, moduleBase, roots, clearCount, pose, 1, where);
  if (n.empty()) {
    *windowLine = *overlayLine = "--";
    return;
  }
  std::string what = PickupHint(names, n[0].g->merged, tier);
  std::string clock = ClockDirection(pose, n[0].pos->x - pose.x, n[0].pos->z - pose.z);
  char buf[200];
  if (tier >= kTierCategory)
    std::snprintf(buf, sizeof(buf), ", %.0f m, %s, %s%.0f m", n[0].dist, clock.c_str(), n[0].dy >= 0 ? "+" : "",
                  n[0].dy);
  else
    std::snprintf(buf, sizeof(buf), ", %.0f m, %s", n[0].dist, clock.c_str());
  *windowLine = what + buf;
  std::snprintf(buf, sizeof(buf), "%.0f m %s: ", n[0].dist, clock.c_str());
  *overlayLine = buf + what;
}

// nearby --watch: logs, for every pickup taken while it runs, how far the
// player stood from its map position (the map-data spot-check).
int RunNearbyWatch(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, const ItemNameTable& names) {
  auto resolved = ResolvePlayerHp(hProcess, moduleBase, moduleSize);
  EventFlagRoots roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  auto profile = ResolvePlayerProfile(hProcess, moduleBase, moduleSize);
  int clearCount = ReadClearCount(hProcess, moduleBase, profile.baseACellOffset);
  Log("Watching pickups (Ctrl+C to stop). Each line: how far you stood from the item's map position.");
  std::vector<NearbyPickup> watched;
  int checked = 0;
  double sum = 0;
  for (int tick = 0;; ++tick) {
    // After a loading screen the cached chain keeps "reading" the destroyed
    // character (see ReadCurrentPlayerIns), so check the player object every
    // tick and start over when it changes. Found 2026-10-07: a High Wall run
    // logged nothing, still comparing against Firelink's items.
    uintptr_t playerIns = ReadCurrentPlayerIns(hProcess, moduleBase, resolved.worldChrManCellOffset);
    if (playerIns == 0 || playerIns != resolved.entityAddress) {
      watched.clear();
      Sleep(1000);
      if (playerIns == 0) continue;
      resolved = ResolvePlayerHp(hProcess, moduleBase, moduleSize);
      roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
      if (resolved.entityAddress == playerIns) Log("Player (re)loaded -- watching this area's pickups.");
      continue;
    }
    PlayerPose pose = ReadPlayerPose(hProcess, resolved);
    if (!pose.ok) {
      Sleep(1000);
      resolved = ResolvePlayerHp(hProcess, moduleBase, moduleSize);
      continue;
    }
    for (const auto& n : watched) {
      if (ReadEventFlag(hProcess, moduleBase, roots, n.g->flagId) <= 0) continue;
      float dx = n.pos->x - pose.x, dy = n.pos->y - pose.y, dz = n.pos->z - pose.z;
      float d = std::sqrt(dx * dx + dy * dy + dz * dz);
      ++checked;
      sum += d;
      Log("  picked up %-40s %5.1f m from its map position (%s lot %lld)  [%d checked, mean %.1f m]",
          PickupLabel(names, n.g->merged).c_str(), d, n.pos->map.c_str(), static_cast<long long>(n.g->firstLot),
          checked, sum / checked);
    }
    if (tick % 4 == 0 || checked) {
      watched = NearestUnfound(hProcess, moduleBase, roots, clearCount, pose, 20,
                               ReadPlayerArea(hProcess, moduleBase, resolved.entityAddress, 0));
    }
    Sleep(250);
  }
}

// nearby [N]: the N (default 10) closest unfound pickups.
int RunNearby(int count, bool watch) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  if (TreasurePositions().empty()) {
    Log("No pickup positions: run  python -I tools\\extract_treasures.py  first (makes data\\generated\\treasures.tsv).");
    CloseHandle(hProcess);
    return 7;
  }
  auto resolved = ResolvePlayerHp(hProcess, moduleBase, moduleSize);
  PlayerPose pose = ReadPlayerPose(hProcess, resolved);
  if (!pose.ok) {
    Log("No character loaded.");
    CloseHandle(hProcess);
    return 6;
  }
  EventFlagRoots roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  ItemNameTable names = LoadItemNameTable(FindItemNameTablePath());
  if (watch) return RunNearbyWatch(hProcess, moduleBase, moduleSize, names);
  auto profile = ResolvePlayerProfile(hProcess, moduleBase, moduleSize);
  int clearCount = ReadClearCount(hProcess, moduleBase, profile.baseACellOffset);
  Log("You: (%.2f, %.2f, %.2f) facing %.0f deg. Closest unfound pickups:", pose.x, pose.y, pose.z,
      pose.FacingDegrees());
  const int tier = SpoilerTier();
  if (tier == kTierOff) {
    Log("Hints are off (spoiler tier Off): F9 in game cycles the tier, or add --full.");
    CloseHandle(hProcess);
    return 0;
  }
  PlayerArea where = ReadPlayerArea(hProcess, moduleBase, resolved.entityAddress, 0);
  for (const auto& n : NearestUnfound(hProcess, moduleBase, roots, clearCount, pose, count, where)) {
    char height[32];
    std::snprintf(height, sizeof(height), "%s%.0f m", n.dy >= 0 ? "+" : "", n.dy);
    Log("  %6.1f m  %-11s %-7s %-50s %s lot %lld at (%.1f, %.1f, %.1f)", n.dist,
        ClockDirection(pose, n.pos->x - pose.x, n.pos->z - pose.z).c_str(), tier >= kTierCategory ? height : "",
        PickupHint(names, n.g->merged, tier).c_str(), n.pos->map.c_str(), static_cast<long long>(n.g->firstLot),
        n.pos->x, n.pos->y, n.pos->z);
  }
  CloseHandle(hProcess);
  return 0;
}

// ---- boss resistances and weaknesses -------------------
//
// Each boss's NpcParam row comes from the map files: the boss's enemy
// part has EntityID = defeat flag - 10,000,000 (23 of 25 bosses; Iudex
// Gundyr 14000800 -> entity 4000800 -> NpcParam 511000). Two don't follow
// that rule and are listed by hand, found in data/generated/enemies.tsv by
// model and arena: Abyss Watchers (c3040, entity 3300801) and Dancer of
// the Boreal Valley (c5270, entity 3000899). Multi-phase / multi-body
// bosses use the one row that holds their defeat flag's entity.
//
// NpcParam fields (Paramdex NPC_PARAM_ST), live-verified 2026-10-06
// against the Fextralife boss pages, exact on all 8 absorptions for Iudex
// Gundyr (15/18/12/16/5/2/-14/38%), Vordt and Abyss Watchers:
//   +0x19C..+0x1B8  damage rate f32 x8: physical, slash, strike, thrust,
//                   magic, fire, lightning, dark (absorption = 1 - rate;
//                   Paramdex calls them regainRate_*)
//   +0x104..+0x10A  resistPoison, resistToxic, resistBlood, resistCurse s16
//   +0x1D8          resistFrost s16 (999 = immune; Iudex 200 bleed /
//                   63 frost, matching the wiki)
// HP (+0x20) is left out: it's the base value before the game's area
// scaling (Vordt reads 1,190; the wiki's NG HP is 1,328).
constexpr uintptr_t kNpcDamageRatesOffset = 0x19C;
constexpr uintptr_t kNpcResistPoisonOffset = 0x104;  // poison, toxic, bleed, curse (s16 x4)
constexpr uintptr_t kNpcResistFrostOffset = 0x1D8;
constexpr int kStatusImmune = 999;

struct EnemyPlacement {
  int32_t npcParam;
  float x, y, z;
};

// data/generated/enemies.tsv (tools/extract_treasures.py): entity -> placement.
const std::unordered_map<int32_t, EnemyPlacement>& EnemyPlacements() {
  static std::unordered_map<int32_t, EnemyPlacement> table;
  static LONG loadedVersion = -1;
  if (loadedVersion == g_mapDataVersionInUse) return table;
  loadedVersion = g_mapDataVersionInUse;
  table.clear();
  std::ifstream in(Paths().Generated(L"enemies.tsv"));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const char* p = line.c_str() + line.find('\t');
    char* end = nullptr;
    long entity = std::strtol(p, &end, 10);
    long npc = std::strtol(end, &end, 10);
    std::strtol(end, &end, 10);  // think param
    float x = std::strtof(end, &end), y = std::strtof(end, &end), z = std::strtof(end, &end);
    if (entity > 0 && !table.count(entity)) table[entity] = {static_cast<int32_t>(npc), x, y, z};
  }
  return table;
}

int32_t BossEntity(const BossInfo& b) {
  switch (b.flag) {
    case 13300800: return 3300801;  // Abyss Watchers (c3040)
    case 13000890: return 3000899;  // Dancer of the Boreal Valley (c5270)
    default: return static_cast<int32_t>(b.flag) - 10000000;
  }
}

struct BossStats {
  bool ok = false;
  bool humanType = false;  // c0000 human enemies: blank NpcParam row, stats come from their gear
  float rate[8] = {};  // physical, slash, strike, thrust, magic, fire, lightning, dark
  int16_t resist[5] = {};  // poison, toxic, bleed, curse, frost
};

BossStats ReadBossStats(HANDLE hProcess, uintptr_t moduleBase, const BossInfo& b) {
  BossStats s;
  auto it = EnemyPlacements().find(BossEntity(b));
  if (it == EnemyPlacements().end()) return s;
  static ParamTableRef table;
  if (!table.ok) table = ResolveParamTable(hProcess, moduleBase, L"NpcParam");
  uintptr_t row = FindParamRow(hProcess, table, it->second.npcParam);
  if (!row) return s;
  SIZE_T br = 0;
  s.ok = ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(row + kNpcDamageRatesOffset), s.rate,
                           sizeof(s.rate), &br) &&
         ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(row + kNpcResistPoisonOffset), s.resist,
                           4 * sizeof(int16_t), &br) &&
         ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(row + kNpcResistFrostOffset), &s.resist[4],
                           sizeof(int16_t), &br);
  // Champion's Gravetender (NpcParam 21300) and Halflight (21800) are
  // human-type enemies (model c0000): their rows hold rate 1.0 (0%) for
  // every damage type and 0 for every status -- their defences come from
  // equipped gear, which isn't read.
  bool blank = true;
  for (float r : s.rate) blank &= r == 1.0f;
  for (int16_t r : s.resist) blank &= r == 0;
  if (s.ok && blank) {
    s.ok = false;
    s.humanType = true;
  }
  return s;
}

const char* const kDamageNames[8] = {"Physical", "Slash", "Strike", "Thrust", "Magic", "Fire", "Lightning", "Dark"};
const char* const kStatusNames[5] = {"poison", "toxic", "bleed", "curse", "frost"};

int Absorption(float rate) { return static_cast<int>(std::lround((1.0 - rate) * 100.0)); }

// "weak: Lightning -14% | resists: Dark 38% | immune: poison, toxic | bleed 200, frost 63"
// Weak = negative absorption; with none, the least-absorbed type. Brief
// (Vague tier) = just the weak part.
std::string FormatBossWeakness(const BossStats& s, bool brief) {
  if (s.humanType) return "human-type boss: defences come from its gear (not read)";
  if (!s.ok) return "stats not found";
  int lowest = 0, highest = 0;
  for (int i = 1; i < 8; ++i) {
    if (s.rate[i] > s.rate[lowest]) lowest = i;
    if (s.rate[i] < s.rate[highest]) highest = i;
  }
  std::string weak;
  for (int i = 0; i < 8; ++i)
    if (Absorption(s.rate[i]) < 0)
      weak += (weak.empty() ? "" : ", ") + std::string(kDamageNames[i]) + " " + std::to_string(Absorption(s.rate[i])) + "%";
  std::string out = weak.empty() ? "least resisted: " + std::string(kDamageNames[lowest]) + " " +
                                       std::to_string(Absorption(s.rate[lowest])) + "%"
                                 : "weak: " + weak;
  if (brief) return out;
  out += " | resists: " + std::string(kDamageNames[highest]) + " " + std::to_string(Absorption(s.rate[highest])) + "%";
  std::string immune, rest;
  for (int i = 0; i < 5; ++i) {
    if (s.resist[i] >= kStatusImmune) immune += (immune.empty() ? "" : ", ") + std::string(kStatusNames[i]);
    else if (i != 3) rest += (rest.empty() ? "" : ", ") + std::string(kStatusNames[i]) + " " + std::to_string(s.resist[i]);
  }
  if (!immune.empty()) out += " | immune: " + immune;
  if (!rest.empty()) out += " | " + rest;
  return out;
}

// Window "Boss" line: the closest undefeated boss in the current area, at
// the spoiler tier -- Off hidden, Vague the weakness only, Category the
// full breakdown without the name, Full with it. "" when there's none.
std::string WindowBossHint(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, const std::string& area,
                           const PlayerPose& pose) {
  const std::vector<BossInfo>& bosses = Bosses();
  static EventFlagRoots roots;
  const int tier = SpoilerTier();
  if (tier < kTierFull || area.empty()) return "";  // Full only (user's call, 2026-10-07)
  if (!roots.eventFlagManCell) roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  const BossInfo* best = nullptr;
  float bestDist = 0;
  for (const auto& b : bosses) {
    if (b.area != area || ReadEventFlag(hProcess, moduleBase, roots, b.flag) > 0) continue;
    if (BossIsLater(hProcess, moduleBase, roots, b.flag)) continue;
    auto it = EnemyPlacements().find(BossEntity(b));
    float d = 1e9f;
    if (it != EnemyPlacements().end() && pose.ok) {
      float dx = it->second.x - pose.x, dy = it->second.y - pose.y, dz = it->second.z - pose.z;
      d = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    if (!best || d < bestDist) { best = &b; bestDist = d; }
  }
  if (!best) return "";
  if (EnemyPlacements().empty()) return "no map data (run tools\\extract_treasures.py)";
  std::string text = FormatBossWeakness(ReadBossStats(hProcess, moduleBase, *best), false);
  return tier >= kTierFull ? best->name + ": " + text : text;
}

// resist [--full]: every boss's absorptions and status resistances.
// Full tier only (or --full), defeated bosses included (user's call, 2026-10-07).
int RunResist() {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  if (EnemyPlacements().empty()) {
    Log("No enemy placements: run  python -I tools\\extract_treasures.py  first (makes data\\generated\\enemies.tsv).");
    CloseHandle(hProcess);
    return 7;
  }
  EventFlagRoots roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  const int tier = SpoilerTier();
  int hidden = 0;
  Log("Boss absorptions (%% of each damage type absorbed; negative = takes extra) and status resistances:");
  Log("  %-36s %5s %5s %5s %5s %5s %5s %5s %5s  status (999 = immune)", "", "Phys", "Slash", "Strk", "Thrst",
      "Magic", "Fire", "Ltng", "Dark");
  for (const auto& b : LoadBosses()) {
    if (tier < kTierFull) { ++hidden; continue; }  // Full only, defeated too (user's call, 2026-10-07)
    BossStats s = ReadBossStats(hProcess, moduleBase, b);
    if (!s.ok) {
      Log("  %-36s (%s)", b.name.substr(0, 36).c_str(),
          s.humanType ? "human-type boss: defences come from its gear, not read" : "stats not found");
      continue;
    }
    char status[96];
    std::snprintf(status, sizeof(status), "psn %d tox %d bld %d crs %d frs %d", s.resist[0], s.resist[1], s.resist[2],
                  s.resist[3], s.resist[4]);
    Log("  %-36s %4d%% %4d%% %4d%% %4d%% %4d%% %4d%% %4d%% %4d%%  %s", b.name.substr(0, 36).c_str(),
        Absorption(s.rate[0]), Absorption(s.rate[1]), Absorption(s.rate[2]), Absorption(s.rate[3]),
        Absorption(s.rate[4]), Absorption(s.rate[5]), Absorption(s.rate[6]), Absorption(s.rate[7]), status);
  }
  if (hidden)
    Log("%d boss(es) hidden: resistances show at spoiler tier Full, now %s (F9 in game or --full to show).", hidden,
        SpoilerTierName(tier));
  CloseHandle(hProcess);
  return 0;
}

// ---- route hints ----------------------------------------
//
// data/route.tsv: the steps of the game in the order they open (main
// path, optional areas, DLC), each with its unlock condition as boss
// defeat flags, the area its way in starts from, its bosses and the way
// in (our own words; sources in the file header). Status per step:
//   locked -- its condition isn't met; open -- met, bosses left;
//   done   -- all its bosses defeated (a step without bosses: once open).
// "Visited" areas: any saved session of this character entered them, the
// player is there now, an item there was picked up, or a boss there is
// dead. Spoiler tiers (user's choice, 2026-10-06):
//   Off      -- nothing
//   Vague    -- only areas you've visited ("Undead Settlement: a required
//               boss is still alive")
//   Category -- also that a new area is open, and from where, unnamed
//   Full     -- names the next area, the way in and its bosses
struct RouteAlt {
  int atLeast = 0;               // 0: a single flag; N: at least N of `flags`
  std::vector<uint32_t> flags;
};
using FlagNeeds = std::vector<std::vector<RouteAlt>>;  // all groups; any alternative per group

std::vector<std::string> SplitStr(const std::string& s, char sep) {
  std::vector<std::string> parts;
  size_t pos = 0;
  while (true) {
    size_t e = s.find(sep, pos);
    parts.push_back(s.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
    if (e == std::string::npos) break;
    pos = e + 1;
  }
  return parts;
}

std::vector<uint32_t> ParseFlagList(const std::string& s) {
  std::vector<uint32_t> flags;
  if (s == "-" || s.empty()) return flags;
  for (const auto& f : SplitStr(s, ',')) flags.push_back(static_cast<uint32_t>(std::strtoul(f.c_str(), nullptr, 10)));
  return flags;
}

// "a b|c 3of:d,e,f" -> groups (all must hold) of alternatives; "-" = none.
FlagNeeds ParseNeeds(const std::string& text) {
  FlagNeeds needs;
  if (text == "-" || text.empty()) return needs;
  for (const auto& group : SplitStr(text, ' ')) {
    std::vector<RouteAlt> alts;
    for (const auto& alt : SplitStr(group, '|')) {
      RouteAlt a;
      size_t colon = alt.find(':');
      if (colon != std::string::npos && alt.find("of") != std::string::npos) {
        a.atLeast = std::atoi(alt.c_str());
        a.flags = ParseFlagList(alt.substr(colon + 1));
      } else {
        a.flags = ParseFlagList(alt);
      }
      alts.push_back(a);
    }
    needs.push_back(alts);
  }
  return needs;
}

// isOn(flag) -> bool. Empty needs hold trivially.
template <class F>
bool NeedsMet(const FlagNeeds& needs, F isOn) {
  for (const auto& group : needs) {
    bool any = false;
    for (const auto& alt : group) {
      int n = 0;
      for (uint32_t f : alt.flags) n += isOn(f) ? 1 : 0;
      any |= alt.atLeast > 0 ? n >= alt.atLeast : (n == static_cast<int>(alt.flags.size()) && n > 0);
    }
    if (!any) return false;
  }
  return true;
}

struct RouteStep {
  std::string id, area, kind, from, wayIn;
  FlagNeeds needs;
  std::vector<uint32_t> bosses;
};

std::vector<RouteStep> LoadRoute() {
  std::vector<RouteStep> out;
  std::ifstream in(Paths().Data(L"route.tsv"));
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    auto f = SplitStr(line, '\t');
    if (f.size() < 7) continue;
    out.push_back({f[0], f[1], f[2], f[4], f[6], ParseNeeds(f[3]), ParseFlagList(f[5])});
  }
  return out;
}

// A boss is "later" when it's undefeated and its route step isn't open
// yet: the game's normal progression doesn't send you to it until other
// bosses are dead (the Dancer after three Lords of Cinder, the Dragonslayer
// Armour after the Dancer, ...). The boss lists show such bosses apart
// instead of as required now (user's request, 2026-10-07). *opensAfter, if
// given, gets what opens the step, e.g. "Dancer of the Boreal Valley" or
// "3 of Abyss Watchers / Yhorm the Giant / ... (1 so far)".
// The rule itself, on any route / boss list and defeat check (unit-tested).
bool BossIsLaterIn(const std::vector<RouteStep>& route, const std::vector<BossInfo>& bosses, uint32_t flag,
                   const std::function<bool(uint32_t)>& isDead, std::string* opensAfter) {
  if (isDead(flag)) return false;
  auto bossName = [&](uint32_t f) {
    for (const auto& b : bosses)
      if (b.flag == f) return b.name;
    return std::string("a boss");
  };
  for (const auto& st : route) {
    if (std::find(st.bosses.begin(), st.bosses.end(), flag) == st.bosses.end()) continue;
    if (NeedsMet(st.needs, isDead)) return false;
    if (opensAfter) {
      std::string text;
      for (const auto& group : st.needs) {
        std::string g;
        for (const auto& alt : group) {
          std::string names;
          int n = 0;
          for (uint32_t f : alt.flags) {
            names += (names.empty() ? "" : alt.atLeast > 0 ? " / " : " and ") + bossName(f);
            n += isDead(f) ? 1 : 0;
          }
          if (alt.atLeast > 0) names = std::to_string(alt.atLeast) + " of " + names + " (" + std::to_string(n) + " so far)";
          g += (g.empty() ? "" : " or ") + names;
        }
        text += (text.empty() ? "" : " and ") + g;
      }
      *opensAfter = text;
    }
    return true;
  }
  return false;
}

bool BossIsLater(HANDLE hProcess, uintptr_t moduleBase, const EventFlagRoots& roots, uint32_t flag,
                 std::string* opensAfter) {
  static std::vector<RouteStep> route = LoadRoute();
  const std::vector<BossInfo>& bosses = Bosses();
  return BossIsLaterIn(route, bosses, flag,
                       [&](uint32_t f) { return ReadEventFlag(hProcess, moduleBase, roots, f) > 0; }, opensAfter);
}

// Areas this character entered in any saved session ("enter" events),
// rescanned at most every 30s.
std::vector<std::string> SessionAreasVisited(const std::string& character) {
  static std::string cachedFor;
  static std::vector<std::string> cached;
  static ULONGLONG cachedAt = 0;
  if (character == cachedFor && GetTickCount64() - cachedAt < 30000) return cached;
  cachedFor = character;
  cachedAt = GetTickCount64();
  cached.clear();
  if (character.empty()) return cached;
  std::wstring dir = Paths().Sessions() + L"\\" + SafeFolderName(character);
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*.jsonl").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return cached;
  do {
    std::ifstream in(dir + L"\\" + fd.cFileName);
    std::string line;
    while (std::getline(in, line)) {
      if (line.find("\"type\":\"enter\"") == std::string::npos) continue;
      size_t a = line.find("\"area\":\"");
      if (a == std::string::npos) continue;
      a += 8;
      std::string area = line.substr(a, line.find('"', a) - a);
      if (std::find(cached.begin(), cached.end(), area) == cached.end()) cached.push_back(area);
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return cached;
}

// Areas the character has been to: saved sessions, the current area, areas
// with a dead boss, areas with a picked-up item.
std::vector<std::string> VisitedAreas(HANDLE hProcess, uintptr_t moduleBase, const EventFlagRoots& roots,
                                      uintptr_t baseACellOffset, const std::string& character,
                                      const std::string& currentArea) {
  const std::vector<BossInfo>& bosses = Bosses();
  static const AreaInfo areaInfo;
  std::vector<std::string> visited = SessionAreasVisited(character);
  auto visit = [&](const std::string& a) {
    if (!a.empty() && std::find(visited.begin(), visited.end(), a) == visited.end()) visited.push_back(a);
  };
  visit(currentArea);
  for (const auto& b : bosses)
    if (ReadEventFlag(hProcess, moduleBase, roots, b.flag) > 0) visit(b.area);
  if (!TreasurePositions().empty()) {
    int clearCount = ReadClearCount(hProcess, moduleBase, baseACellOffset);
    for (const auto& g : AllWorldPickups(hProcess, moduleBase, clearCount))
      if (ReadEventFlag(hProcess, moduleBase, roots, g.flagId) > 0) visit(areaInfo.AreaOf(PickupRegion(g)));
  }
  return visited;
}

// A route step's state for the player: open (its gate is met), done (open
// and every boss of it dead), visited (they've been to its area),
// required / optional bosses still alive.
struct RouteEval {
  bool open = false, done = false, visited = false;
  int requiredLeft = 0, optionalLeft = 0;
};

// The step the route hint is about: where to go next. The earliest open
// step (main first, then optional, then DLC) that is somewhere not yet
// visited -- its way in is the news -- or that still has a required boss
// alive, which blocks the way on (the Dancer, once three Lords are dead,
// in an area already visited). A step with only optional bosses left
// doesn't hold the hint up (user's report 2026-10-09: in Undead Settlement
// with Vordt dead and the optional Greatwood alive, the hint pointed back
// at the settlement's own way in). If nothing like that is left, the
// earliest open step that isn't done, so optional bosses still get a
// mention. -1 when everything is done.
int PickNextRouteStep(const std::vector<RouteStep>& route, const std::vector<RouteEval>& ev) {
  for (int pass = 0; pass < 2; ++pass)
    for (const char* kind : {"main", "optional", "dlc"})
      for (size_t i = 0; i < route.size(); ++i) {
        const RouteEval& e = ev[i];
        if (route[i].kind != kind || !e.open || e.done) continue;
        if (pass == 1 || !e.visited || e.requiredLeft > 0) return static_cast<int>(i);
      }
  return -1;
}

struct RouteView {
  std::vector<LiveRouteStep> steps;  // in route order, at the tier
  std::string hint;                  // the suggested next step, at the tier ("" at Off)
  int hidden = 0;                    // steps not shown at this tier
};

RouteView BuildRouteView(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
                         const std::string& character, const std::string& currentArea, int tier) {
  static std::vector<RouteStep> route = LoadRoute();
  const std::vector<BossInfo>& bosses = Bosses();
  static EventFlagRoots roots;
  if (!roots.eventFlagManCell) roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  RouteView view;
  if (route.empty()) {
    view.hint = "data/route.tsv missing";
    return view;
  }
  std::unordered_map<uint32_t, bool> dead;
  auto isDead = [&](uint32_t flag) {
    auto it = dead.find(flag);
    if (it != dead.end()) return it->second;
    return dead[flag] = ReadEventFlag(hProcess, moduleBase, roots, flag) > 0;
  };
  auto bossName = [&](uint32_t flag) {
    for (const auto& b : bosses)
      if (b.flag == flag) return b.name;
    return std::string("a boss");
  };
  auto bossRequired = [&](uint32_t flag) {
    for (const auto& b : bosses)
      if (b.flag == flag) return b.status == "required";
    return true;
  };

  std::vector<std::string> visited =
      VisitedAreas(hProcess, moduleBase, roots, baseACellOffset, character, currentArea);
  auto wasVisited = [&](const std::string& a) { return std::find(visited.begin(), visited.end(), a) != visited.end(); };

  // Status per step.
  std::vector<RouteEval> ev(route.size());
  for (size_t i = 0; i < route.size(); ++i) {
    const auto& st = route[i];
    bool open = NeedsMet(st.needs, isDead);
    RouteEval& e = ev[i];
    e.open = open;
    for (uint32_t f : st.bosses) {
      if (isDead(f)) continue;
      (bossRequired(f) ? e.requiredLeft : e.optionalLeft) += 1;
    }
    e.done = open && e.requiredLeft == 0 && e.optionalLeft == 0;
    e.visited = wasVisited(st.area);
  }

  // The step list at the tier.
  if (tier == kTierOff) {
    view.hidden = static_cast<int>(route.size());
    return view;
  }
  for (size_t i = 0; i < route.size(); ++i) {
    const auto& st = route[i];
    const RouteEval& e = ev[i];
    LiveRouteStep s;
    s.kind = st.kind;
    s.status = e.done ? 2 : e.open ? 1 : 0;
    s.requiredLeft = e.requiredLeft;
    s.optionalLeft = e.optionalLeft;
    if (tier >= kTierFull) {
      s.label = st.id == "dancer" ? st.area + " (return)" : st.area;
      s.wayIn = st.wayIn;
      s.from = st.from;
    } else if (e.visited && e.open) {
      s.label = st.area;
    } else if (tier == kTierCategory && e.open && !e.done) {
      s.label = "A new area beyond " + (wasVisited(st.from) ? st.from : std::string("somewhere you've been"));
      s.unnamed = true;
    } else {
      ++view.hidden;
      continue;
    }
    view.steps.push_back(s);
  }

  // The suggestion: where to go next (PickNextRouteStep).
  auto bossesText = [&](const RouteStep& st) {
    std::string names;
    for (uint32_t f : st.bosses)
      if (!isDead(f)) names += (names.empty() ? "" : ", ") + bossName(f) + (bossRequired(f) ? "" : " (optional)");
    return names;
  };
  auto unfinishedHere = [&](size_t i) {
    const RouteEval& e = ev[i];
    return route[i].area + ": " +
           (e.requiredLeft ? std::string(e.requiredLeft == 1 ? "a required boss is" : "required bosses are")
                           : std::string(e.optionalLeft == 1 ? "an optional boss is" : "optional bosses are")) +
           " still alive";
  };
  int next = PickNextRouteStep(route, ev);
  if (next < 0) {
    view.hint = "Everything on the route is done.";
  } else if (tier >= kTierFull) {
    const auto& st = route[next];
    std::string b = bossesText(st);
    view.hint = (st.kind == "main" ? "Next: " : "Open: ") + (st.id == "dancer" ? st.area + " (return)" : st.area) +
                " -- " + st.wayIn + (b.empty() ? "" : "; boss: " + b);
  } else {
    // Vague / Category: the first unfinished step in an area you've been to;
    // at Category a new area comes first if the route reaches it sooner.
    int visitedNext = -1;
    for (size_t i = 0; i < route.size() && visitedNext < 0; ++i)
      if (ev[i].open && !ev[i].done && ev[i].visited && (ev[i].requiredLeft || ev[i].optionalLeft))
        visitedNext = static_cast<int>(i);
    bool newFirst = tier == kTierCategory && !ev[next].visited && (visitedNext < 0 || next < visitedNext);
    if (newFirst)
      view.hint = "A new area is open beyond " +
                  (wasVisited(route[next].from) ? route[next].from : std::string("somewhere you've been")) + ".";
    else if (visitedNext >= 0)
      view.hint = unfinishedHere(visitedNext) + ".";
    else
      view.hint = "Nothing unfinished in the areas you've visited -- explore onward.";
  }
  return view;
}

// route [--full]: the route at the spoiler tier.
int RunRoute() {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  auto profile = ResolvePlayerProfile(hProcess, moduleBase, moduleSize);
  auto resolved = ResolvePlayerHp(hProcess, moduleBase, moduleSize);
  PlayerArea where = ReadPlayerArea(hProcess, moduleBase, resolved.entityAddress, 0);
  std::string area = AreaInfo().AreaOf(where.playRegionId);
  std::string character = profile.xBase ? ReadCharacterName(hProcess, profile.xBase) : "";
  const int tier = SpoilerTier();
  RouteView v = BuildRouteView(hProcess, moduleBase, moduleSize, profile.baseACellOffset, character, area, tier);
  static const char* kStatus[] = {"locked", "open", "done"};
  Log("Route (spoiler tier %s):", SpoilerTierName(tier));
  for (const auto& s : v.steps) {
    std::string left = s.status == 1 ? " (" + std::to_string(s.requiredLeft) + " required, " +
                                           std::to_string(s.optionalLeft) + " optional boss(es) left)"
                                     : "";
    Log("  %-7s %-9s %s%s", kStatus[s.status], s.kind.c_str(), s.label.c_str(), left.c_str());
    if (!s.wayIn.empty() && s.status != 2) Log("            way in: %s", s.wayIn.c_str());
  }
  if (v.hidden) Log("  (%d step(s) not shown at tier %s; F9 in game or --full)", v.hidden, SpoilerTierName(tier));
  Log("Hint: %s", v.hint.empty() ? "(hidden at tier Off)" : v.hint.c_str());
  CloseHandle(hProcess);
  return 0;
}

void LiveRoute(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
               const std::string& character, const std::string& area, std::vector<LiveRouteStep>* steps,
               std::string* hint, int* hidden) {
  RouteView v = BuildRouteView(hProcess, moduleBase, moduleSize, baseACellOffset, character, area, SpoilerTier());
  *steps = std::move(v.steps);
  *hint = v.hint;
  *hidden = v.hidden;
}

// ---- missable-content warnings --------------------------
//
// data/missables.tsv: NPC questline steps, missable items and ending
// requirements, each with when it becomes relevant, its lockout trigger
// and (where the game shows it) a "done" condition, all as event flags
// (sources and syntax in the file header). Most quest-step flags are
// unknown, so the player can also mark an entry done or "don't care" on
// the live page; marks are kept per character in
// sessions/<character>/missables.txt (git-ignored with sessions/). We
// add flags to the data as we find them with `flagwatch` (user's choice,
// 2026-10-06). Status: pending; missed (the trigger happened first); done
// (by the game or a mark); don't care.
// Spoiler tiers: Off nothing; Vague only "something here can still be
// missed" in visited areas; Category the kind of thing ("an NPC questline
// step") and whether a boss kill locks it; Full everything.
struct Missable {
  std::string id, kind, topic, area, shortText, doText, loseText;
  FlagNeeds relevant, trigger, done;
};

std::vector<Missable> LoadMissables() {
  std::vector<Missable> out;
  std::ifstream in(Paths().Data(L"missables.tsv"));
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    auto f = SplitStr(line, '\t');
    if (f.size() < 10) continue;
    out.push_back({f[0], f[1], f[2], f[3], f[7], f[8], f[9], ParseNeeds(f[4]), ParseNeeds(f[5]), ParseNeeds(f[6])});
  }
  return out;
}

// The player's marks (id -> "done" / "ignore"), per character. Written by
// the page server thread, read by the poller: guarded by an SRW lock.
SRWLOCK g_missMarksLock = SRWLOCK_INIT;
std::string g_missMarksFor;
std::unordered_map<std::string, std::string> g_missMarks;

std::wstring MissableMarksPath(const std::string& character) {
  return Paths().Sessions() + L"\\" + SafeFolderName(character) + L"\\missables.txt";
}

void LoadMissableMarksLocked(const std::string& character) {  // caller holds the lock exclusively
  if (g_missMarksFor == character) return;
  g_missMarksFor = character;
  g_missMarks.clear();
  std::ifstream in(MissableMarksPath(character));
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    g_missMarks[line.substr(0, eq)] = line.substr(eq + 1);
  }
}

std::unordered_map<std::string, std::string> MissableMarks(const std::string& character) {
  AcquireSRWLockExclusive(&g_missMarksLock);
  LoadMissableMarksLocked(character);
  auto copy = g_missMarks;
  ReleaseSRWLockExclusive(&g_missMarksLock);
  return copy;
}

// state: done / ignore / clear. False for an unknown id or state.
bool SetMissableMark(const std::string& character, const std::string& id, const std::string& state) {
  static std::vector<Missable> all = LoadMissables();
  if (state != "done" && state != "ignore" && state != "clear") return false;
  if (std::none_of(all.begin(), all.end(), [&](const Missable& m) { return m.id == id; })) return false;
  AcquireSRWLockExclusive(&g_missMarksLock);
  LoadMissableMarksLocked(character);
  if (state == "clear") g_missMarks.erase(id);
  else g_missMarks[id] = state;
  EnsureDir(Paths().Sessions() + L"\\" + SafeFolderName(character));
  std::ofstream out(MissableMarksPath(character), std::ios::trunc);
  out << "# Missable-content marks made on the live page (done / ignore), per entry of data/missables.tsv.\n";
  for (const auto& [k, v] : g_missMarks) out << k << "=" << v << "\n";
  ReleaseSRWLockExclusive(&g_missMarksLock);
  return true;
}

struct MissablesView {
  std::vector<LiveMissable> items;  // at the tier, pending first
  std::string alert;                // a pending warning in the current area, at the tier
  int hidden = 0;                   // relevant entries not shown at this tier
};

MissablesView BuildMissablesView(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
                                 const std::string& character, const std::string& currentArea, int tier) {
  static std::vector<Missable> all = LoadMissables();
  static EventFlagRoots roots;
  if (!roots.eventFlagManCell) roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  MissablesView view;
  if (all.empty()) {
    view.alert = tier > kTierOff ? "data/missables.tsv missing" : "";
    return view;
  }
  std::unordered_map<uint32_t, bool> cache;
  auto isOn = [&](uint32_t flag) {
    auto it = cache.find(flag);
    if (it != cache.end()) return it->second;
    return cache[flag] = ReadEventFlag(hProcess, moduleBase, roots, flag) > 0;
  };
  auto marks = MissableMarks(character);
  std::vector<std::string> visited = VisitedAreas(hProcess, moduleBase, roots, baseACellOffset, character, currentArea);
  auto wasVisited = [&](const std::string& a) { return std::find(visited.begin(), visited.end(), a) != visited.end(); };
  auto kindText = [](const std::string& k) {
    return k == "npc" ? std::string("An NPC questline step") : k == "item" ? std::string("A missable item")
                                                                          : std::string("An ending requirement");
  };
  for (const auto& m : all) {
    if (!NeedsMet(m.relevant, isOn)) continue;
    LiveMissable lm;
    lm.id = m.id;
    lm.kind = m.kind;
    lm.here = !currentArea.empty() && m.area == currentArea;
    bool autoDone = !m.done.empty() && NeedsMet(m.done, isOn);
    auto mark = marks.find(m.id);
    bool triggered = !m.trigger.empty() && NeedsMet(m.trigger, isOn);
    lm.automatic = autoDone;
    lm.status = autoDone ? 2
                : mark != marks.end() ? (mark->second == "done" ? 2 : 3)
                : triggered           ? 1
                                      : 0;
    bool seen = wasVisited(m.area);
    if (tier == kTierOff || (tier == kTierVague && (!seen || lm.status == 1))) {
      ++view.hidden;
      continue;
    }
    if (tier >= kTierFull) {
      lm.title = m.topic + ": " + m.shortText;
      lm.text = m.doText;
      lm.lose = m.loseText;
      lm.area = m.area;
    } else if (tier == kTierCategory) {
      lm.title = kindText(m.kind);
      lm.text = m.trigger.empty() ? "" : "A boss kill or a later step locks it out.";
      lm.area = seen ? m.area : "somewhere you haven't been yet";
    } else {
      lm.title = "Something here can still be missed";
      lm.area = m.area;
    }
    if (lm.status == 0 && lm.here && view.alert.empty())
      view.alert = tier >= kTierFull ? m.topic + ": " + m.shortText
                   : tier == kTierCategory ? kindText(m.kind) + " here can be missed"
                                           : "Something here can be missed";
    view.items.push_back(lm);
  }
  // Pending in this area first, then pending, missed, done, don't care.
  std::stable_sort(view.items.begin(), view.items.end(), [](const LiveMissable& a, const LiveMissable& b) {
    auto rank = [](const LiveMissable& m) { return m.status == 0 ? (m.here ? 0 : 1) : m.status + 1; };
    return rank(a) < rank(b);
  });
  return view;
}

void LiveMissables(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
                   const std::string& character, const std::string& area, std::vector<LiveMissable>* out,
                   std::string* alert, int* hidden) {
  MissablesView v = BuildMissablesView(hProcess, moduleBase, moduleSize, baseACellOffset, character, area, SpoilerTier());
  *out = std::move(v.items);
  *alert = v.alert;
  *hidden = v.hidden;
}

// missables [--full]: the warnings at the spoiler tier.
int RunMissables() {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  auto profile = ResolvePlayerProfile(hProcess, moduleBase, moduleSize);
  auto resolved = ResolvePlayerHp(hProcess, moduleBase, moduleSize);
  PlayerArea where = ReadPlayerArea(hProcess, moduleBase, resolved.entityAddress, 0);
  std::string area = AreaInfo().AreaOf(where.playRegionId);
  std::string character = profile.xBase ? ReadCharacterName(hProcess, profile.xBase) : "";
  const int tier = SpoilerTier();
  MissablesView v = BuildMissablesView(hProcess, moduleBase, moduleSize, profile.baseACellOffset, character, area, tier);
  static const char* kStatus[] = {"PENDING", "missed", "done", "don't care"};
  Log("Missable content (spoiler tier %s, %s):", SpoilerTierName(tier), character.c_str());
  for (const auto& m : v.items) {
    Log("  %-10s %-7s %s%s  [%s] (%s)", kStatus[m.status], m.kind.c_str(), m.title.c_str(),
        m.automatic ? " (the game shows it)" : "", m.area.c_str(), m.id.c_str());
    if (!m.text.empty() && m.status <= 1) Log("             %s", m.text.c_str());
    if (!m.lose.empty() && m.status <= 1) Log("             Lose: %s", m.lose.c_str());
  }
  if (v.hidden) Log("  (%d not shown at tier %s; F9 in game or --full)", v.hidden, SpoilerTierName(tier));
  if (!v.alert.empty()) Log("Here: %s", v.alert.c_str());
  CloseHandle(hProcess);
  return 0;
}

// ---- live page: data lists, JSON, local server (overlay mode) ----------
//
// The overlay process also serves a dashboard for a second monitor:
// templates/live.html at http://localhost:8765/ (next free port up to
// 8774), polling /api/state twice a second. Bound to 127.0.0.1 only and
// requests must carry a localhost Host header (so another site can't
// read it through DNS rebinding). The page can set the spoiler tier
// (POST /api/tier?t=N) -- the guide's own setting, the same as F9;
// nothing on the page touches the game.

// Every positioned pickup in the player's named area (or, where the area
// has none, in their map section and world): unfound ones first, closest
// first, labelled at the tier; then the found ones, always named (you
// have them -- user's call, 2026-10-06).
void LiveNearbyList(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
                    const PlayerPose& pose, const PlayerArea& where, const std::string& area,
                    std::vector<LiveNearby>* out) {
  const ItemNameTable& names = ItemNames();
  static EventFlagRoots roots;
  static const AreaInfo areaInfo;
  out->clear();
  if (!pose.ok || TreasurePositions().empty()) return;
  if (!roots.eventFlagManCell) roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  const int tier = SpoilerTier();
  const int clearCount = ReadClearCount(hProcess, moduleBase, baseACellOffset);
  const std::string map = where.ok ? where.MapName().substr(0, 3) : "";
  const auto& pickups = AllWorldPickups(hProcess, moduleBase, clearCount);
  std::vector<LiveNearby> unfound, found;
  for (int pass = 0; pass < 2 && unfound.empty() && found.empty(); ++pass) {
    for (const auto& g : pickups) {
      const TreasurePos* p = PickupPosition(g);
      if (!p) continue;
      bool inScope = pass == 0 ? !area.empty() && areaInfo.AreaOf(p->region) == area
                               : (map.empty() || p->map == map) && SameWorld(p->region, where.playRegionId);
      if (!inScope) continue;
      LiveNearby l;
      float dx = p->x - pose.x, dy = p->y - pose.y, dz = p->z - pose.z;
      l.found = ReadEventFlag(hProcess, moduleBase, roots, g.flagId) > 0;
      l.label = l.found ? PickupLabel(names, g.merged) : tier <= kTierVague ? "" : PickupHint(names, g.merged, tier);
      if (!l.found && tier >= kTierFull) l.category = PickupCategory(g.merged);
      l.clock = ClockDirection(pose, dx, dz);
      l.angle = RelativeBearing(pose, dx, dz);
      l.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
      l.dy = dy;
      l.x = p->x;
      l.z = p->z;
      l.showHeight = tier >= kTierCategory;
      (l.found ? found : unfound).push_back(l);
    }
  }
  auto byDist = [](const LiveNearby& a, const LiveNearby& b) { return a.dist < b.dist; };
  std::sort(unfound.begin(), unfound.end(), byDist);
  std::sort(found.begin(), found.end(), byDist);
  out->insert(out->end(), unfound.begin(), unfound.end());
  out->insert(out->end(), found.begin(), found.end());
}

// Bosses in the current area at the tier, plus overall counts
// (counts: base defeated, base total, DLC defeated, DLC total).
// Defeated bosses are always named. Numbers (weaknesses, resistances) only
// at Full, for every boss (user's call, 2026-10-07); undefeated bosses are
// unnamed below Full.
void LiveBossList(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, const std::string& area,
                  const PlayerPose& pose, std::vector<LiveBoss>* out, int counts[4]) {
  const std::vector<BossInfo>& bosses = Bosses();
  static EventFlagRoots roots;
  if (!roots.eventFlagManCell) roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  const int tier = SpoilerTier();
  out->clear();
  for (int i = 0; i < 4; ++i) counts[i] = 0;
  float bestDist = 1e9f;
  int best = -1;
  for (const auto& b : bosses) {
    bool dead = ReadEventFlag(hProcess, moduleBase, roots, b.flag) > 0;
    bool dlc = b.note.find("DLC") != std::string::npos;
    counts[dlc ? 2 : 0] += dead ? 1 : 0;
    counts[dlc ? 3 : 1] += 1;
    if (area.empty() || b.area != area) continue;
    LiveBoss lb;
    lb.defeated = dead;
    lb.optional = b.status == "optional";
    // Not open yet in the normal route: shown apart, never the boss ahead.
    // What opens it is named at Full only.
    std::string opensAfter;
    lb.later = !dead && BossIsLater(hProcess, moduleBase, roots, b.flag, &opensAfter);
    if (lb.later) lb.opens = tier >= kTierFull ? "Opens after " + opensAfter : "Comes later in the game";
    if (dead || tier >= kTierFull) lb.name = b.name;
    // Weaknesses and resistances at Full only, defeated bosses included
    // (user's call, 2026-10-07; was Vague = weakness, Category = all).
    lb.statsLevel = tier >= kTierFull ? 2 : 0;
    BossStats st = ReadBossStats(hProcess, moduleBase, b);
    lb.humanType = st.humanType;
    if (st.ok && lb.statsLevel >= 1) lb.weak = FormatBossWeakness(st, true);
    if (st.ok && lb.statsLevel == 2) {
      std::memcpy(lb.rate, st.rate, sizeof(lb.rate));
      std::memcpy(lb.resist, st.resist, sizeof(lb.resist));
    }
    auto it = EnemyPlacements().find(BossEntity(b));
    if (!dead && it != EnemyPlacements().end() && pose.ok) {
      float dx = it->second.x - pose.x, dy = it->second.y - pose.y, dz = it->second.z - pose.z;
      float d = std::sqrt(dx * dx + dy * dy + dz * dz);
      lb.dist = d;
      if (!lb.later && d < bestDist) { bestDist = d; best = static_cast<int>(out->size()); }
    }
    out->push_back(lb);
  }
  if (best >= 0) (*out)[best].nearest = true;
}

std::string JsonNum(double v, int decimals = 2) {
  if (!std::isfinite(v)) return "null";
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  return buf;
}

// The page's whole state; trail points from index `trailFrom` on (the page
// sends how many it already has, so each poll only carries new ones).
std::string BuildLiveJson(const WindowSnapshot& s, size_t trailFrom) {
  const int tier = SpoilerTier();
  std::string j = "{";
  auto kv = [&](const char* k, const std::string& v) {
    if (j.size() > 1) j += ",";
    j += "\"";
    j += k;
    j += "\":" + v;
  };
  auto b = [](bool v) { return std::string(v ? "true" : "false"); };
  auto i = [](long long v) { return std::to_string(v); };
  kv("version", JsonString(WASD_VERSION_STRING));
  kv("status", JsonString(s.status));
  kv("paused", b(s.paused));
  kv("haveValues", b(s.haveValues));
  kv("tier", i(tier));
  kv("tierName", JsonString(SpoilerTierName(tier)));
  kv("character", JsonString(s.characterName));
  kv("level", i(s.level));
  kv("souls", i(s.souls));
  kv("nextLevelCost", i(s.nextLevelCost));
  kv("hp", "[" + i(s.hp) + "," + i(s.maxHp) + "]");
  kv("fp", "[" + i(s.fp) + "," + i(s.maxFp) + "]");
  kv("stamina", "[" + i(s.stamina) + "," + i(s.maxStamina) + "]");
  {
    std::string a = "[";
    for (int k = 0; k < 9; ++k) a += (k ? "," : "") + i(s.attrs[k]);
    kv("attrs", a + "]");
    std::string g = "[", d = "[";
    for (int k = 0; k < 9; ++k) {
      g += (k ? "," : "") + JsonString(s.planGain[k]);
      d += (k ? "," : "") + JsonString(s.planDetail[k]);
    }
    kv("plan", "{\"gain\":" + g + "],\"detail\":" + d + "],\"best\":" + JsonString(s.planBest) + "}");
  }
  kv("equipLoad", s.haveEquipLoad ? "[" + JsonNum(s.equipLoad, 1) + "," + JsonNum(s.maxEquipLoad, 1) + "]" : "null");
  kv("roll", JsonString(s.haveEquipLoad && s.maxEquipLoad > 0
                            ? RollTypeForLoad(s.equipLoad / s.maxEquipLoad * 100.0)
                            : ""));
  kv("spell", JsonString(s.spell));
  kv("quickItem", JsonString(s.quickItem));
  kv("area", "{\"region\":" + JsonString(s.areaName) + ",\"name\":" + JsonString(s.currentArea) +
                 ",\"bonfire\":" + JsonString(s.lastBonfireName) + ",\"mapKey\":" + i(s.mapNumber * 2 + (s.untendedWorld ? 1 : 0)) + "}");
  kv("pose", s.pose.ok ? "{\"x\":" + JsonNum(s.pose.x) + ",\"y\":" + JsonNum(s.pose.y) + ",\"z\":" +
                             JsonNum(s.pose.z) + ",\"facing\":" + JsonNum(s.pose.facingRadians, 4) + "}"
                       : "null");
  kv("items", "{\"found\":" + i(s.itemsFound) + ",\"total\":" + i(s.itemsTotal) +
                  ",\"scope\":" + JsonString(s.itemsScope) + "}");
  {
    std::string a = "[";
    for (size_t k = 0; k < s.nearby.size(); ++k) {
      const auto& n = s.nearby[k];
      a += (k ? "," : "");
      a += "{\"label\":" + JsonString(n.label) + ",\"category\":" + JsonString(n.category) +
           ",\"found\":" + b(n.found) + ",\"clock\":" + JsonString(n.clock) +
           ",\"dist\":" +
           JsonNum(n.dist, 1) + ",\"dy\":" + JsonNum(n.dy, 1) + ",\"showHeight\":" + b(n.showHeight) +
           ",\"angle\":" + JsonNum(n.angle, 4) + ",\"x\":" + JsonNum(n.x) + ",\"z\":" + JsonNum(n.z) + "}";
    }
    kv("nearby", a + "]");
  }
  {
    std::string a = "[";
    for (size_t k = 0; k < s.bosses.size(); ++k) {
      const auto& lb = s.bosses[k];
      a += (k ? "," : "");
      a += "{\"name\":" + JsonString(lb.name) + ",\"defeated\":" + b(lb.defeated) + ",\"optional\":" +
           b(lb.optional) + ",\"nearest\":" + b(lb.nearest) + ",\"human\":" + b(lb.humanType && lb.statsLevel == 2) +
           ",\"later\":" + b(lb.later) + ",\"opens\":" + JsonString(lb.opens) + ",\"statsLevel\":" + i(lb.statsLevel) + ",\"dist\":" + JsonNum(lb.dist, 0) +
           ",\"weak\":" + JsonString(lb.weak);
      if (lb.statsLevel == 2 && !lb.humanType) {
        a += ",\"absorb\":[";
        for (int t = 0; t < 8; ++t) a += (t ? "," : "") + i(Absorption(lb.rate[t]));
        a += "],\"resist\":[";
        for (int t = 0; t < 5; ++t) a += (t ? "," : "") + i(lb.resist[t]);
        a += "]";
      }
      a += "}";
    }
    kv("bosses", a + "]");
  }
  kv("bossesHere", "[" + i(s.bossesDefeated) + "," + i(s.bossesTotal) + "," + i(s.bossesLater) + "]");
  {
    std::string a = "[";
    for (size_t k = 0; k < s.route.size(); ++k) {
      const auto& r = s.route[k];
      a += (k ? "," : "");
      a += "{\"label\":" + JsonString(r.label) + ",\"kind\":" + JsonString(r.kind) + ",\"status\":" + i(r.status) +
           ",\"requiredLeft\":" + i(r.requiredLeft) + ",\"optionalLeft\":" + i(r.optionalLeft) + ",\"unnamed\":" +
           b(r.unnamed) + ",\"from\":" + JsonString(r.from) + ",\"wayIn\":" + JsonString(r.wayIn) + "}";
    }
    kv("route", "{\"hint\":" + JsonString(s.routeHint) + ",\"hidden\":" + i(s.routeHidden) + ",\"steps\":" + a + "]}");
  }
  {
    std::string a = "[";
    for (size_t k = 0; k < s.missables.size(); ++k) {
      const auto& m = s.missables[k];
      a += (k ? "," : "");
      a += "{\"id\":" + JsonString(m.id) + ",\"kind\":" + JsonString(m.kind) + ",\"title\":" + JsonString(m.title) +
           ",\"text\":" + JsonString(m.text) + ",\"lose\":" + JsonString(m.lose) + ",\"area\":" + JsonString(m.area) +
           ",\"status\":" + i(m.status) + ",\"here\":" + b(m.here) + ",\"automatic\":" + b(m.automatic) + "}";
    }
    kv("missables", "{\"alert\":" + JsonString(s.missableAlert) + ",\"hidden\":" + i(s.missablesHidden) +
                        ",\"items\":" + a + "]}");
  }
  kv("bossHint", JsonString(s.bossHint));
  kv("progress", "{\"bossesBase\":[" + i(s.bossCounts[0]) + "," + i(s.bossCounts[1]) + "],\"bossesDlc\":[" +
                     i(s.bossCounts[2]) + "," + i(s.bossCounts[3]) + "],\"shards\":" + JsonString(s.shards) +
                     ",\"keys\":" + JsonString(s.keyItems) + ",\"deathsTotal\":" + i(s.deathsTotal) +
                     ",\"sessionDeaths\":" + i(s.sessionDeaths) + ",\"soulsLost\":" + i(s.soulsLost) +
                     ",\"soulsRecovered\":" + i(s.soulsRecovered) + ",\"soulsPerHour\":" +
                     JsonNum(s.soulsPerHour, 0) + ",\"sessionSeconds\":" + JsonNum(s.sessionSeconds, 0) +
                     ",\"areaSeconds\":" + JsonNum(s.areaSeconds, 0) + "}");
  {
    static const char* kTypes[5] = {"Phys", "Magic", "Fire", "Lightning", "Dark"};
    std::string a = "[";
    for (size_t k = 0; k < s.weapons.size(); ++k) {
      const auto& w = s.weapons[k];
      a += (k ? "," : "");
      a += "{\"slot\":" + JsonString(w.slot) + ",\"name\":" + JsonString(w.name) + ",\"active\":" + b(w.active) +
           ",\"twoHanded\":" + b(w.twoHanded) + ",\"catalyst\":" + b(w.catalyst) + ",\"spellBuff\":" +
           i(w.spellBuff) + ",\"upgrade\":" + JsonString(w.upgrade) + ",\"infusion\":" + JsonString(w.infusion) +
           ",\"infusionDetail\":" + JsonString(w.infusionDetail) + ",\"ar\":[";
      bool first = true;
      for (int t = 0; t < 5 && w.haveAr; ++t) {
        if (!w.present[t]) continue;
        a += std::string(first ? "" : ",") + "{\"type\":\"" + kTypes[t] + "\",\"base\":" + i(w.base[t]) +
             ",\"bonus\":" + i(w.twoHanded ? w.bonus2h[t] : w.bonus1h[t]) + "}";
        first = false;
      }
      a += "]}";
    }
    kv("weapons", a + "]");
  }
  {
    EnterCriticalSection(&g_trail.lock);
    size_t total = g_trail.points.size();
    size_t from = (std::min)(trailFrom, total);
    std::string a = "[";
    for (size_t k = from; k < total; ++k) {
      const auto& p = g_trail.points[k];
      a += (k > from ? "," : "");
      a += "[" + JsonNum(p.x, 1) + "," + JsonNum(p.z, 1) + "," + JsonNum(p.y, 1) + "," + i(p.mapKey) + "," +
           i(p.kind) + "]";
    }
    LeaveCriticalSection(&g_trail.lock);
    kv("trail", "{\"from\":" + i(static_cast<long long>(from)) + ",\"total\":" +
                    i(static_cast<long long>(total)) + ",\"points\":" + a + "]}");
  }
  {
    std::string o = "{";
    for (const auto& d : kOverlaySettingDefs)
      o += std::string(o.size() > 1 ? "," : "") + "\"" + d.key + "\":" + std::to_string(*d.value);
    kv("overlay", o + "}");
  }
  {
    static const char* ids[3] = {"a", "b", "c"};
    std::string m = "[";
    for (int k = 0; k < 6; ++k)
      if (g_pageMuted & (1L << k)) m += std::string(m.size() > 1 ? "," : "") + "\"" + kPagePanels[k] + "\"";
    kv("page", "{\"theme\":\"" + std::string(ids[g_pageTheme % 3]) + "\",\"muted\":" + m + "]}");
  }
  kv("perf", "{\"read\":" + JsonString(s.lastReadTime) + ",\"latencyUs\":" + JsonNum(s.lastLatencyMicros, 1) +
                 ",\"rateHz\":" + JsonNum(s.perf.valid ? s.perf.rateHz : 0.0) + ",\"cpu\":" +
                 JsonNum(s.perf.valid ? s.perf.cpuPercentOfCore : 0.0) + ",\"memMB\":" +
                 JsonNum(s.perf.valid ? s.perf.workingSetMB : 0.0, 1) + "}");
  return j + "}";
}

struct LiveServer {
  SOCKET listener = INVALID_SOCKET;
  HANDLE thread = nullptr;
  int port = 0;
  bool wsa = false;
  volatile ULONGLONG lastStatePoll = 0;  // GetTickCount64 of the last /api/state request
  volatile ULONGLONG previewUntil = 0;   // overlay preview (settings panel open) until this tick
};
LiveServer g_live;

// The page renews the preview every 1.5s while its overlay settings panel
// is open; if it stops (tab closed, page gone) the preview lapses in 4s.
bool OverlayPreviewActive() { return GetTickCount64() < g_live.previewUntil; }

bool LivePageRecentlyPolled() {
  ULONGLONG last = g_live.lastStatePoll;
  return last != 0 && GetTickCount64() - last < 2500;
}
constexpr int kLivePortFirst = 8765, kLivePortLast = 8774;

void SendAll(SOCKET c, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    int n = send(c, data.data() + sent, static_cast<int>((std::min)(data.size() - sent, size_t{1} << 20)), 0);
    if (n <= 0) return;
    sent += static_cast<size_t>(n);
  }
}

void SendResponse(SOCKET c, const char* status, const char* type, const std::string& body) {
  std::string head = std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type +
                     "\r\nContent-Length: " + std::to_string(body.size()) +
                     "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
  SendAll(c, head + body);
}

void HandleLiveRequest(SOCKET c) {
  std::string req;
  char buf[4096];
  while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
    int n = recv(c, buf, sizeof(buf), 0);
    if (n <= 0) return;
    req.append(buf, static_cast<size_t>(n));
  }
  size_t sp1 = req.find(' '), sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
  if (sp2 == std::string::npos) return;
  std::string method = req.substr(0, sp1), target = req.substr(sp1 + 1, sp2 - sp1 - 1);
  // Host must be this machine's name for the page (DNS-rebinding guard).
  std::string host;
  size_t h = req.find("\r\nHost:");
  if (h == std::string::npos) h = req.find("\r\nhost:");
  if (h != std::string::npos) {
    size_t e = req.find("\r\n", h + 2);
    host = req.substr(h + 7, e - h - 7);
    host.erase(0, host.find_first_not_of(' '));
  }
  std::string port = ":" + std::to_string(g_live.port);
  if (host != "localhost" + port && host != "127.0.0.1" + port) {
    SendResponse(c, "403 Forbidden", "text/plain", "localhost only");
    return;
  }
  std::string path = target.substr(0, target.find('?'));
  std::string query = target.find('?') == std::string::npos ? "" : target.substr(target.find('?') + 1);
  auto param = [&](const char* name) -> std::string {
    std::string key = std::string(name) + "=";
    size_t p = ("&" + query).find("&" + key);
    if (p == std::string::npos) return "";
    size_t start = p + key.size();
    return query.substr(start, query.find('&', start) - start);
  };
  if (method == "GET" && (path == "/" || path == "/index.html")) {
    std::ifstream in(Paths().Template(L"live.html"), std::ios::binary);
    if (!in) {
      SendResponse(c, "404 Not Found", "text/plain", "templates/live.html not found");
      return;
    }
    std::string page((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    SendResponse(c, "200 OK", "text/html; charset=utf-8", page);
  } else if (method == "GET" && path == "/api/state") {
    g_live.lastStatePoll = GetTickCount64();
    WindowSnapshot snap;
    EnterCriticalSection(&g_window.lock);
    snap = g_window.snapshot;
    LeaveCriticalSection(&g_window.lock);
    std::string from = param("trail");
    SendResponse(c, "200 OK", "application/json",
                 BuildLiveJson(snap, from.empty() ? 0 : static_cast<size_t>(std::strtoull(from.c_str(), nullptr, 10))));
  } else if (method == "POST" && path == "/api/tier") {
    std::string t = param("t");
    if (t.size() != 1 || t[0] < '0' || t[0] > '3') {
      SendResponse(c, "400 Bad Request", "text/plain", "t must be 0-3");
      return;
    }
    InterlockedExchange(&g_spoilerTier, t[0] - '0');
    SaveSettings();
    PostMessageW(g_window.hwnd, kMsgSpoilerTierSet, 0, 0);
    SendResponse(c, "200 OK", "application/json", "{\"tier\":" + t + "}");
  } else if (method == "POST" && path == "/api/missable") {
    // ?id=<missable id>&state=done|ignore|clear -- the player's own mark, per character
    std::string id = param("id"), state = param("state");
    EnterCriticalSection(&g_window.lock);
    std::string character = g_window.snapshot.characterName;
    LeaveCriticalSection(&g_window.lock);
    bool ok = !character.empty() && SetMissableMark(character, id, state);
    if (ok) SetEvent(g_window.wakeEvent);
    SendResponse(c, ok ? "200 OK" : "400 Bad Request", "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  } else if (method == "POST" && path == "/api/page") {
    // ?theme=a|b|c and/or ?muted=char,map,... (the whole list; empty = none muted)
    std::string theme = param("theme");
    bool hasMuted = ("&" + query).find("&muted=") != std::string::npos;  // sent unencoded: a,b,c
    bool ok = !theme.empty() || hasMuted;
    if (!theme.empty()) ok = theme.size() == 1 && theme[0] >= 'a' && theme[0] <= 'c';
    if (ok && hasMuted) ok = SetPageMuted(param("muted"));
    if (ok && !theme.empty()) InterlockedExchange(&g_pageTheme, theme[0] - 'a');
    if (ok) SaveSettings();
    SendResponse(c, ok ? "200 OK" : "400 Bad Request", "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  } else if (method == "POST" && path == "/api/quit") {
    // The page's Quit button: same clean shutdown as Ctrl+C (session saved).
    SendResponse(c, "200 OK", "application/json", "{\"quitting\":true}");
    Log("Quit requested from the live page.");
    PostMessageW(g_window.hwnd, WM_CLOSE, 0, 0);
  } else if (method == "POST" && path == "/api/preview") {
    bool on = param("on") == "1";
    bool was = OverlayPreviewActive();
    g_live.previewUntil = on ? GetTickCount64() + 4000 : 0;
    if (on != was) PostMessageW(g_window.hwnd, kMsgOverlaySettings, 0, 0);  // show/hide now
    SendResponse(c, "200 OK", "application/json", on ? "{\"preview\":true}" : "{\"preview\":false}");
  } else if (method == "POST" && path == "/api/overlay") {
    // ?key=value[&key=value...]: overlay settings (validated; all or nothing reported)
    bool ok = !query.empty();
    for (size_t pos = 0; ok && pos < query.size();) {
      size_t amp = query.find('&', pos);
      std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
      size_t eq = pair.find('=');
      ok = eq != std::string::npos && SetOverlaySetting(pair.substr(0, eq), pair.substr(eq + 1));
      pos = amp == std::string::npos ? query.size() : amp + 1;
    }
    SaveSettings();
    PostMessageW(g_window.hwnd, kMsgOverlaySettings, 0, 0);
    SendResponse(c, ok ? "200 OK" : "400 Bad Request", "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  } else {
    SendResponse(c, "404 Not Found", "text/plain", "not found");
  }
}

DWORD WINAPI LiveServerMain(LPVOID) {
  while (!g_window.stop) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(g_live.listener, &set);
    timeval tv{0, 250000};  // wake 4x a second to notice shutdown
    if (select(0, &set, nullptr, nullptr, &tv) <= 0) continue;
    SOCKET c = accept(g_live.listener, nullptr, nullptr);
    if (c == INVALID_SOCKET) continue;
    DWORD timeoutMs = 2000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    HandleLiveRequest(c);
    closesocket(c);
  }
  return 0;
}

bool StartLiveServer(int* portOut) {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    Log("Live page: Winsock unavailable -- page disabled.");
    return false;
  }
  g_live.wsa = true;
  for (int port = kLivePortFirst; port <= kLivePortLast; ++port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) break;
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1: this PC only
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && listen(s, 8) == 0) {
      g_live.listener = s;
      g_live.port = port;
      break;
    }
    closesocket(s);
  }
  if (g_live.listener == INVALID_SOCKET) {
    Log("Live page: no free port in %d-%d -- page disabled.", kLivePortFirst, kLivePortLast);
    return false;
  }
  g_live.thread = CreateThread(nullptr, 0, LiveServerMain, nullptr, 0, nullptr);
  *portOut = g_live.port;
  return g_live.thread != nullptr;
}

// Call after g_window.stop is set: the server thread notices within 250ms.
void StopLiveServer() {
  if (g_live.thread) {
    WaitForSingleObject(g_live.thread, 2000);
    CloseHandle(g_live.thread);
    g_live.thread = nullptr;
  }
  if (g_live.listener != INVALID_SOCKET) closesocket(g_live.listener);
  g_live.listener = INVALID_SOCKET;
  if (g_live.wsa) WSACleanup();
  g_live.wsa = false;
}

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
                L"inventory | ar | memdiff | window | overlay | report | extract | paths\n",
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
