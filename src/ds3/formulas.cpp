// src/ds3/formulas.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

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

