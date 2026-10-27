// src/ds3/pickups.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

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

