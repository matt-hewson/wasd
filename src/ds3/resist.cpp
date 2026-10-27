// src/ds3/resist.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

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

