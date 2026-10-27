// src/ds3/flags.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

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
  std::vector<uint32_t> fight, phases;  // fight-on flags; one flag per phase after the first
};
std::vector<BossInfo> LoadBosses();
std::vector<uint32_t> ParseFlagList(const std::string& s);

// A boss's fight from its flags: 0 = no fight (defeated, or none of its
// fight flags set), else the phase it's in, 1 + the position of the last
// phase flag set. isOn(flag) reads one event flag.
template <typename F>
int BossFightPhase(const BossInfo& b, bool defeated, F isOn) {
  if (defeated || std::none_of(b.fight.begin(), b.fight.end(), isOn)) return 0;
  int phase = 1;
  for (size_t k = 0; k < b.phases.size(); ++k)
    if (isOn(b.phases[k])) phase = static_cast<int>(k) + 2;
  return phase;
}

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

