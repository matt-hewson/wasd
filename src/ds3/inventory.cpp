// src/ds3/inventory.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

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

