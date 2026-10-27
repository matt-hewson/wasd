// src/ds3/player.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

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

