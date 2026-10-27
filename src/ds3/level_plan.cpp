// src/ds3/level_plan.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- what to level next -----------------------------------------------
//
// What +1 in each attribute would give, from the character as it is:
//   Vigor / Attunement -- the game's own curves, CalcCorrectGraph rows 100
//     (HP: 1/15/27/50/99 -> 300/550/1000/1300/1400) and 101 (FP: 1/15/35/
//     60/99 -> 50/120/280/350/450), found with `graphs` 2026-10-06 and
//     matching the wiki's tables. Scaled by current max / curve value, so
//     an Ember's +30% (live HP 590 / 627 = curve 454 / 483 x 1.30) and
//     rings carry over.
//   Endurance -- no curve found in the params: interpolated from the
//     Fextralife Endurance table's points, so marked "≈".
//   Vitality -- equip load +1 x the ring multiplier, and the roll tier.
//   Attunement slots -- ComputeBaseAttunementSlots.
//   Str / Dex / Int / Fth / Luck -- ComputeWeaponAr again with the stat +1
//     for every equipped weapon (computed on gear/level change).
// "Best" = the biggest gain for the weapon in hand (right hand first; a
// catalyst's spell buff counts), a requirement penalty removed first.
double EnduranceStaminaApprox(double endurance) {
  static const double pts[][2] = {{9, 92},   {11, 95},  {12, 97},  {13, 98},  {14, 100}, {15, 102}, {16, 104},
                                  {17, 106}, {18, 108}, {19, 110}, {20, 112}, {21, 114}, {22, 116}, {23, 118},
                                  {24, 120}, {25, 122}, {30, 134}, {35, 146}, {40, 160}, {50, 161}, {99, 170}};
  const int n = static_cast<int>(sizeof(pts) / sizeof(pts[0]));
  if (endurance <= pts[0][0]) return pts[0][1] - (pts[0][0] - endurance) * 1.5;
  for (int i = 1; i < n; ++i)
    if (endurance <= pts[i][0])
      return pts[i - 1][1] + (pts[i][1] - pts[i - 1][1]) * (endurance - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]);
  return pts[n - 1][1];
}

void BuildLevelPlan(HANDLE hProcess, uintptr_t moduleBase, WindowSnapshot* s) {
  static CalcCorrectGraphRow hpCurve, fpCurve;
  if (!hpCurve.ok) hpCurve = ReadCalcCorrectGraphRow(hProcess, moduleBase, 100);
  if (!fpCurve.ok) fpCurve = ReadCalcCorrectGraphRow(hProcess, moduleBase, 101);
  const int32_t* a = s->attrs;
  for (auto& g : s->planGain) g.clear();
  for (auto& d : s->planDetail) d.clear();
  if (a[0] <= 0) return;
  char buf[160];
  auto curveGain = [](const CalcCorrectGraphRow& c, int v, int current) -> int {
    if (!c.ok) return -1;
    double now = std::floor(EvaluateCalcCorrectGraph(c, v)), next = std::floor(EvaluateCalcCorrectGraph(c, v + 1));
    double scale = now > 0 && current > 0 ? current / now : 1.0;
    return static_cast<int>(std::lround((next - now) * scale));
  };
  int hp = curveGain(hpCurve, a[0], s->maxHp);
  if (hp >= 0) s->planGain[0] = "+" + std::to_string(hp) + " HP";
  int fp = curveGain(fpCurve, a[1], s->maxFp);
  if (fp >= 0) s->planGain[1] = "+" + std::to_string(fp) + " FP";
  if (ComputeBaseAttunementSlots(a[1] + 1) > ComputeBaseAttunementSlots(a[1])) s->planGain[1] += ", +1 slot";
  {
    double now = EnduranceStaminaApprox(a[2]), next = EnduranceStaminaApprox(a[2] + 1);
    double scale = now > 0 && s->maxStamina > 0 ? s->maxStamina / now : 1.0;
    s->planGain[2] = "≈+" + std::to_string(static_cast<int>(std::lround((next - now) * scale))) + " stamina";
  }
  if (s->haveEquipLoad && s->maxEquipLoad > 0) {
    double mult = s->maxEquipLoad / ComputeBaseMaxEquipLoad(a[3]);
    double newMax = s->maxEquipLoad + mult;
    std::string now = RollTypeForLoad(s->equipLoad / s->maxEquipLoad * 100.0);
    std::string then = RollTypeForLoad(s->equipLoad / newMax * 100.0);
    std::snprintf(buf, sizeof(buf), "+%.1f load", mult);
    s->planGain[3] = buf;
    if (then != now) s->planGain[3] += " (" + then + ")";
    std::snprintf(buf, sizeof(buf), "Equip load %.1f / %.1f -> %.1f (%s)", s->equipLoad, s->maxEquipLoad, newMax,
                  then.c_str());
    s->planDetail[3] = buf;
  }
  // Offensive stats: the weapon in hand first.
  const LiveWeapon* inHand = nullptr;
  for (const auto& w : s->weapons)
    if (w.active && (!inHand || w.slot[0] == 'R')) inHand = &w;
  int bestK = -1;
  double bestScore = 0;
  for (int k = 0; k < 5; ++k) {
    std::string detail;
    for (const auto& w : s->weapons) {
      int ar = w.twoHanded ? w.plan2h[k] : w.plan1h[k];
      std::string part;
      if (w.planFix[k]) part = "meets a requirement";
      if (ar) part += (part.empty() ? "" : ", ") + std::string(ar > 0 ? "+" : "") + std::to_string(ar) + " AR";
      if (w.catalyst && w.planBuff[k]) part += (part.empty() ? "" : ", ") + std::string("+") +
                                              std::to_string(w.planBuff[k]) + " spell buff";
      if (!part.empty()) detail += (detail.empty() ? "" : "; ") + w.name + ": " + part;
    }
    s->planDetail[4 + k] = detail;
    std::string gain;
    if (inHand) {
      int ar = inHand->twoHanded ? inHand->plan2h[k] : inHand->plan1h[k];
      if (inHand->planFix[k]) gain = "meets req.";
      else if (inHand->catalyst && inHand->planBuff[k]) gain = "+" + std::to_string(inHand->planBuff[k]) + " buff";
      else if (ar) gain = "+" + std::to_string(ar) + " AR";
      double score = (inHand->planFix[k] ? 1000 : 0) + ar + (inHand->catalyst ? inHand->planBuff[k] : 0);
      if (score > bestScore) { bestScore = score; bestK = k; }
    }
    if (gain.empty() && !detail.empty()) gain = "other gear";
    s->planGain[4 + k] = gain.empty() ? "–" : gain;
  }
  // Best other infusion per weapon, at its current grip.
  static const char* kInfusions[16] = {"Normal", "Heavy",     "Sharp", "Refined", "Simple", "Crystal",
                                       "Fire",   "Chaos",     "Lightning", "Deep", "Dark",  "Poison",
                                       "Blood",  "Raw",       "Blessed", "Hollow"};
  for (auto& w : s->weapons) {
    w.infusion.clear();
    w.infusionDetail.clear();
    if (w.infCurrent < 0) continue;
    const double* v = w.twoHanded ? w.inf2h : w.inf1h;
    const char* unit = w.catalyst ? " spell buff" : " AR";
    std::vector<int> order;
    for (int k = 0; k < 16; ++k)
      if (v[k] >= 0 && k != w.infCurrent) order.push_back(k);
    std::sort(order.begin(), order.end(), [&](int x, int y) { return v[x] > v[y]; });
    if (order.empty()) continue;
    double cur = v[w.infCurrent];
    int best = order[0];
    auto delta = [&](int k) {
      int d = static_cast<int>(std::lround(v[k] - cur));
      return (d >= 0 ? "+" : "") + std::to_string(d) + unit;
    };
    if (v[best] <= cur) {
      w.infusion = std::string(kInfusions[w.infCurrent]) + " is already best at your stats";
    } else {
      w.infusion = std::string(kInfusions[best]) + ": " + delta(best) +
                   (best == 0 ? "" : s->gemHeld[best] ? " (gem held)" : " (no gem yet)");
    }
    for (size_t i = 0; i < order.size() && i < 3; ++i)
      w.infusionDetail += (i ? ", " : "") + std::string(kInfusions[order[i]]) + " " + delta(order[i]) +
                          (order[i] == 0 || s->gemHeld[order[i]] ? "" : " (no gem)");
    w.infusionDetail = "vs " + std::string(kInfusions[w.infCurrent]) + " (total" + unit + ", " +
                       (w.twoHanded ? "two-handed" : "one-handed") + "): " + w.infusionDetail +
                       ". Totals add split damage (physical + element) together, which defences cut harder than one type, and leave out status build-up.";
  }

  static const char* kNames[5] = {"Strength", "Dexterity", "Intelligence", "Faith", "Luck"};
  if (!inHand) s->planBest = "";
  else if (bestK < 0)
    s->planBest = "Nothing in hand gains from Str/Dex/Int/Fth/Luck: " + s->planGain[0] + " from Vigor.";
  else
    s->planBest = std::string(kNames[bestK]) + " for " + inHand->name + " (" + s->planGain[4 + bestK] + ")";
}

