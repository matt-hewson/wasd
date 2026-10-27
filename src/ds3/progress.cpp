// src/ds3/progress.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- bosses and progression ------------------------------------------
//
// data/bosses.tsv: flag, area, required/optional, note, fight flags, phase
// flags (the area from where the boss stands in the game's map files;
// sources in the file header).
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
    out.push_back({flag, it != names.end() ? it->second : "boss " + f[0], f[1], f[2], f.size() > 3 ? f[3] : "",
                   ParseFlagList(f.size() > 4 ? f[4] : ""), ParseFlagList(f.size() > 5 ? f[5] : "")});
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

// This player's co-op flag sets, over the same lists the rest of the tool
// uses: bosses.tsv (defeat flags), key_items.tsv (one entry per name, as
// `keys`), every world pickup of this journey (pickup flags), and the NG
// cycle. `lists` gets the names, kinds and areas the comparison needs, in
// the same order.
void ReadMyCoopFlags(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, const ResolvedProfile& profile,
                     SharedGameState* s, CoopLists* lists) {
  static EventFlagRoots roots;
  if (!roots.eventFlagManCell) roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  *lists = CoopLists();
  std::vector<uint32_t> ids;
  s->bossesDefeated.bits.clear();
  for (const auto& b : Bosses()) {
    ids.push_back(b.flag);
    lists->bossNames.push_back(b.name);
    lists->bossAreas.push_back(b.area);
    lists->bossFlags.push_back(b.flag);
    s->bossesDefeated.bits.push_back(ReadEventFlag(hProcess, moduleBase, roots, b.flag) > 0);
  }
  s->bossesDefeated.fingerprint = CoopFingerprint(ids);

  const int clearCount = ReadClearCount(hProcess, moduleBase, profile.baseACellOffset);
  s->clearCount = static_cast<uint8_t>((std::min)((std::max)(clearCount, 0), 255));
  ids.clear();
  s->keyItemsObtained.bits.clear();
  s->keyItemsHeld.bits.clear();
  for (const auto& st : KeyItemStates(hProcess, moduleBase, roots, profile, clearCount, KeyItems())) {
    ids.push_back(static_cast<uint32_t>(st.item->goodsId));
    lists->keyItemNames.push_back(st.item->name);
    lists->keyItemKinds.push_back(st.item->kind);
    s->keyItemsObtained.bits.push_back(st.obtained);
    s->keyItemsHeld.bits.push_back(st.held);
  }
  s->keyItemsObtained.fingerprint = s->keyItemsHeld.fingerprint = CoopFingerprint(ids);

  ids.clear();
  s->pickupsFound.bits.clear();
  static const AreaInfo areas;
  for (const auto& g : AllWorldPickups(hProcess, moduleBase, clearCount)) {
    ids.push_back(g.flagId);
    lists->pickupAreas.push_back(areas.AreaOf(PickupRegion(g)));
    s->pickupsFound.bits.push_back(ReadEventFlag(hProcess, moduleBase, roots, g.flagId) > 0);
  }
  s->pickupsFound.fingerprint = CoopFingerprint(ids);
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

// held: HeldGoodsQty, read once by the caller for every weapon. materials:
// if given, the goods this next upgrade needs are added to it.
std::string WindowUpgrade(HANDLE hProcess, uintptr_t moduleBase, const ResolvedProfile& profile,
                          int32_t inventoryItemId, const std::unordered_map<int32_t, int>& held,
                          std::vector<int32_t>* materials) {
  const ItemNameTable& names = ItemNames();
  auto item = ResolveItem(hProcess, profile.xBase + kEquipGameDataOffset + kEquipInventoryDataOffset,
                          inventoryItemId);
  if (!item.ok || !item.plausible || item.giveId == 110000) return "";
  auto u = WeaponUpgrade(hProcess, moduleBase, names, item.giveId, held);
  if (materials)
    for (const auto& m : u.materials) materials->push_back(m.goodsId);
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

