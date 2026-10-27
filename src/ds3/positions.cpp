// src/ds3/positions.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

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

