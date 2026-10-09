// Game formulas computed by the tool rather than read from memory.
// Expected values are the ones recorded as live-verified in docs/TECHNICAL.md
// where there are any; the rest pin the documented table / curve shape.

TEST(souls_for_level_matches_verified_values) {
  CHECK_EQ(RequiredSoulsForLevel(33), 6640);   // the wiki's table (docs/TECHNICAL.md)
  CHECK_EQ(RequiredSoulsForLevel(48), 13435);  // the in-game level-up screen, 2026-08-19
  CHECK_EQ(RequiredSoulsForLevel(2), 673);
  CHECK_EQ(RequiredSoulsForLevel(1), 0);
  CHECK_EQ(RequiredSoulsForLevel(0), 0);
}

TEST(souls_for_level_rises_every_level) {
  for (int lv = 3; lv <= 802; ++lv) {
    if (RequiredSoulsForLevel(lv) <= RequiredSoulsForLevel(lv - 1)) {
      CHECK_EQ(lv, -1);  // report the first level where the cost doesn't rise
      break;
    }
  }
}

TEST(attunement_slots_follow_the_table) {
  const int pts[][2] = {{1, 0},   {9, 0},   {10, 1},  {13, 1},  {14, 2},  {18, 3},  {23, 3},
                        {24, 4},  {30, 5},  {40, 6},  {50, 7},  {60, 8},  {79, 8},  {80, 9},
                        {98, 9},  {99, 10}};
  for (const auto& p : pts) CHECK_EQ(ComputeBaseAttunementSlots(p[0]), p[1]);
}

// A CalcCorrectGraph row shaped like the game's HP curve (row 100:
// 1/15/27/50/99 -> 300/550/1000/1300/1400), with chosen exponents.
CalcCorrectGraphRow TestGraph(float exp) {
  CalcCorrectGraphRow g;
  g.ok = true;
  const float vals[5] = {1, 15, 27, 50, 99}, grow[5] = {300, 550, 1000, 1300, 1400};
  for (int i = 0; i < 5; ++i) {
    g.stageMaxVal[i] = vals[i];
    g.stageMaxGrowVal[i] = grow[i];
    g.adjPt[i] = exp;
  }
  return g;
}

TEST(calc_correct_graph_hits_breakpoints_and_clamps) {
  auto g = TestGraph(1.0f);
  CHECK_NEAR(EvaluateCalcCorrectGraph(g, 1), 300, 1e-9);
  CHECK_NEAR(EvaluateCalcCorrectGraph(g, 15), 550, 1e-9);
  CHECK_NEAR(EvaluateCalcCorrectGraph(g, 27), 1000, 1e-9);
  CHECK_NEAR(EvaluateCalcCorrectGraph(g, 99), 1400, 1e-9);
  CHECK_NEAR(EvaluateCalcCorrectGraph(g, 0), 300, 1e-9);    // below the first stage
  CHECK_NEAR(EvaluateCalcCorrectGraph(g, 150), 1400, 1e-9);  // above the last
  CHECK_NEAR(EvaluateCalcCorrectGraph(g, 8), 425, 1e-9);     // linear: halfway 1..15
  CHECK_NEAR(EvaluateCalcCorrectGraph(CalcCorrectGraphRow{}, 40), 0, 1e-9);  // not loaded
}

TEST(calc_correct_graph_negative_exponent_flips_the_curve) {
  // Halfway through 1..15 (stat 8): exponent 2 -> 0.5^2 = 0.25 of the way,
  // exponent -2 -> 1 - 0.5^2 = 0.75 of the way.
  CHECK_NEAR(EvaluateCalcCorrectGraph(TestGraph(2.0f), 8), 300 + 250 * 0.25, 1e-6);
  CHECK_NEAR(EvaluateCalcCorrectGraph(TestGraph(-2.0f), 8), 300 + 250 * 0.75, 1e-6);
}

TEST(poise_combines_with_diminishing_returns) {
  CHECK_NEAR(CombinePoise(10, 10), 19, 1e-9);
  CHECK_NEAR(CombinePoise(0, 25), 25, 1e-9);
  CHECK_NEAR(CombinePoise(50, 50), 75, 1e-9);
}

TEST(absorption_from_damage_rate) {
  CHECK_EQ(Absorption(1.0f), 0);
  CHECK_EQ(Absorption(0.9f), 10);
  CHECK_EQ(Absorption(1.2f), -20);
  CHECK_EQ(Absorption(0.7f), 30);
}

TEST(stamina_interpolates_the_wiki_table) {
  CHECK_NEAR(EnduranceStaminaApprox(9), 92, 1e-9);
  CHECK_NEAR(EnduranceStaminaApprox(10), 93.5, 1e-9);  // between 9 (92) and 11 (95)
  CHECK_NEAR(EnduranceStaminaApprox(26), 124.4, 1e-9);  // between 25 (122) and 30 (134)
  CHECK_NEAR(EnduranceStaminaApprox(40), 160, 1e-9);
  CHECK_NEAR(EnduranceStaminaApprox(99), 170, 1e-9);
  CHECK_NEAR(EnduranceStaminaApprox(120), 170, 1e-9);
  CHECK_NEAR(EnduranceStaminaApprox(5), 86, 1e-9);  // extrapolated below the table
}

TEST(roll_tier_boundaries) {
  CHECK_EQ(std::string(RollTypeForLoad(0)), "light roll");
  CHECK_EQ(std::string(RollTypeForLoad(29.99)), "light roll");
  CHECK_EQ(std::string(RollTypeForLoad(30)), "medium roll");
  CHECK_EQ(std::string(RollTypeForLoad(69.99)), "medium roll");
  CHECK_EQ(std::string(RollTypeForLoad(70)), "heavy roll");
  CHECK_EQ(std::string(RollTypeForLoad(100)), "heavy roll");
  CHECK_EQ(std::string(RollTypeForLoad(100.01)), "overloaded");
}

TEST(boss_weakness_text) {
  BossStats s;
  s.ok = true;
  // Absorptions: Physical/Slash/Strike/Thrust 10%, Magic 20%, Fire -20%, Lightning 10%, Dark 30%.
  const float rates[8] = {0.9f, 0.9f, 0.9f, 0.9f, 0.8f, 1.2f, 0.9f, 0.7f};
  std::copy(rates, rates + 8, s.rate);
  const int16_t resist[5] = {999, 999, 200, 999, 63};  // poison, toxic immune; curse immune
  std::copy(resist, resist + 5, s.resist);
  CHECK_EQ(FormatBossWeakness(s, true), std::string("weak: Fire -20%"));
  CHECK_EQ(FormatBossWeakness(s, false),
           std::string("weak: Fire -20% | resists: Dark 30% | immune: poison, toxic, curse | bleed 200, frost 63"));

  // No negative absorption: the least-absorbed type instead.
  const float noWeak[8] = {0.9f, 0.9f, 0.95f, 0.9f, 0.8f, 0.9f, 0.9f, 0.7f};
  std::copy(noWeak, noWeak + 8, s.rate);
  CHECK_EQ(FormatBossWeakness(s, true), std::string("least resisted: Strike 5%"));

  BossStats human;
  human.humanType = true;
  CHECK_EQ(FormatBossWeakness(human, true), std::string("human-type boss: defences come from its gear (not read)"));
  CHECK_EQ(FormatBossWeakness(BossStats{}, true), std::string("stats not found"));
}
