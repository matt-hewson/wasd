// src/ds3/area.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

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
//   death    area, x, y, z, soulsLost, deathsTotal, and boss + bossName +
//            phase when a boss fight was on (an attempt; see BossFightPhase)
//   boss     flag, name, area, playTimeMs        (a boss's defeat flag set)
//   gear     slot ("R" / "L"), id, name, upgrade (the weapon in hand changed;
//            also once at the start)
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
// A death counts as an attempt at the boss whose fight was on (its health
// bar up: BossFightPhase) at most this many seconds before. The game clears
// the fight flags when the map reloads after a death, which can come before
// the death counter does.
constexpr double kBossFightGraceSeconds = 30.0;
// A session is one character's: loading another (a different name, or play
// time going back) ends it and starts a new one. For this long after a
// session starts, defeat flags that turn on only move the baseline: the
// boss list can still hold the previous character's flags for a tick or
// two, and the game takes a moment to load the new one's (a live switch
// logged seven of the new character's old kills ~2 s in).
constexpr double kSessionSettleSeconds = 5.0;
constexpr uint32_t kPlayTimeBackSlackMs = 60000;

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

