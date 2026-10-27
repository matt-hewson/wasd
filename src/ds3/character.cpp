// src/ds3/character.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

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

