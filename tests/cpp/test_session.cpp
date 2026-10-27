// Session bookkeeping (SessionStats): deaths, souls lost / recovered /
// gained, time per area. The game's death counter and the souls drop can
// land on different ticks, in either order; both orders are covered.
// And the area lookup (AreaInfo, real data), and the session log's boss
// kill, boss attempt (fight flags and phase) and weapon events
// (SessionTracker into a capture).

namespace session_test {
// One tick of 0.1s in "Area".
bool Tick(SessionStats& s, int32_t souls, int32_t deaths, const std::string& area = "Area", double dt = 0.1) {
  return s.Tick(dt, souls, deaths, area);
}
}  // namespace session_test

TEST(session_first_tick_only_starts) {
  SessionStats s;
  CHECK(!session_test::Tick(s, 500, 3));
  CHECK(s.started);
  CHECK_EQ(s.seconds, 0.0);
  CHECK_EQ(s.deaths, 0);
  CHECK_EQ(s.gained, int64_t(0));
}

TEST(session_souls_gained) {
  SessionStats s;
  session_test::Tick(s, 0, 0);
  session_test::Tick(s, 120, 0);
  session_test::Tick(s, 300, 0);
  CHECK_EQ(s.gained, int64_t(300));
  CHECK_EQ(s.lost, int64_t(0));
}

TEST(session_death_drop_then_counter) {
  SessionStats s;
  session_test::Tick(s, 500, 0);
  session_test::Tick(s, 0, 0);   // souls drop first
  CHECK(session_test::Tick(s, 0, 1));  // then the death counter
  CHECK_EQ(s.deaths, 1);
  CHECK_EQ(s.lost, int64_t(500));
  CHECK_EQ(s.eventLost, int64_t(500));
  CHECK_EQ(s.bloodstain, int64_t(500));
}

TEST(session_death_counter_then_drop) {
  SessionStats s;
  session_test::Tick(s, 500, 0);
  CHECK(!session_test::Tick(s, 500, 1));  // counter first: waits for the drop
  CHECK_EQ(s.deaths, 1);
  CHECK_EQ(s.lost, int64_t(0));
  CHECK(session_test::Tick(s, 0, 1));
  CHECK_EQ(s.lost, int64_t(500));
}

TEST(session_bloodstain_recovered_is_not_gained) {
  SessionStats s;
  session_test::Tick(s, 500, 0);
  session_test::Tick(s, 0, 0);
  session_test::Tick(s, 0, 1);
  CHECK(session_test::Tick(s, 500, 1));  // picked up exactly the lost amount
  CHECK_EQ(s.recovered, int64_t(500));
  CHECK_EQ(s.eventRecovered, int64_t(500));
  CHECK_EQ(s.gained, int64_t(0));
  CHECK_EQ(s.bloodstain, int64_t(0));
}

TEST(session_dying_again_forfeits_the_bloodstain) {
  SessionStats s;
  session_test::Tick(s, 500, 0);
  session_test::Tick(s, 0, 0);
  session_test::Tick(s, 0, 1);  // first death: 500 on the ground
  session_test::Tick(s, 80, 1);  // 80 gained, not the bloodstain
  session_test::Tick(s, 0, 1);
  CHECK(session_test::Tick(s, 0, 2));
  CHECK_EQ(s.lost, int64_t(580));
  CHECK_EQ(s.bloodstain, int64_t(80));
  CHECK(s.lastEvent.find("Previous bloodstain forfeited") != std::string::npos);
  CHECK_EQ(s.gained, int64_t(80));
}

TEST(session_spending_souls_is_not_a_death) {
  SessionStats s;
  session_test::Tick(s, 900, 0);
  session_test::Tick(s, 0, 0);  // spent at a level-up: drop to 0, no death
  for (int i = 0; i < 200; ++i) session_test::Tick(s, 0, 0);  // 20s later...
  CHECK(session_test::Tick(s, 0, 1));  // ...dying with nothing held loses nothing
  CHECK_EQ(s.deaths, 1);
  CHECK_EQ(s.lost, int64_t(0));
}

TEST(session_time_per_area_and_souls_per_hour) {
  SessionStats s;
  session_test::Tick(s, 0, 0, "A");
  session_test::Tick(s, 0, 0, "A", 30);
  session_test::Tick(s, 0, 0, "B", 20);
  session_test::Tick(s, 0, 0, "", 5);  // between areas: counts as session time only
  CHECK_NEAR(s.AreaSeconds("A"), 30, 1e-9);
  CHECK_NEAR(s.AreaSeconds("B"), 20, 1e-9);
  CHECK_NEAR(s.seconds, 55, 1e-9);
  CHECK_NEAR(s.SoulsPerHour(), 0, 1e-9);  // under a minute
  session_test::Tick(s, 1000, 0, "B", 3545);
  CHECK_NEAR(s.SoulsPerHour(), 1000, 1e-6);
}

TEST(area_lookup) {
  AreaInfo info;
  CHECK_EQ(info.AreaOf(300001), std::string("High Wall of Lothric"));
  CHECK_EQ(info.AreaOf(400102), std::string("Firelink Shrine"));
  CHECK_EQ(info.AreaOf(460000), std::string(""));  // a PvP arena: no area
  CHECK_EQ(info.AreaOf(-1), std::string(""));
}

TEST(area_tracker_keeps_the_last_real_region) {
  AreaTracker t;
  PlayerArea a;
  a.ok = true;
  a.playRegionId = 300001;
  t.Update(a);
  a.playRegionId = 0;  // between regions (elevator, loading)
  t.Update(a);
  CHECK_EQ(t.lastRegion, 300001);
  a.ok = false;
  a.playRegionId = 310001;
  t.Update(a);
  CHECK_EQ(t.lastRegion, 300001);
}

// ---- the session log's boss, attempt and gear events --------------------
namespace session_test {
// A tracker that writes into `lines` instead of a file.
struct Recorder {
  std::vector<std::string> lines;
  SessionTracker tracker;
  Recorder() { tracker.log.capture = &lines; }
  void Tick(const SessionContext& c) { tracker.Tick(0.1, c); }
  // Past the session's first kSessionSettleSeconds, nothing changing.
  void Settle(const SessionContext& c) {
    for (int k = 0; k < int(kSessionSettleSeconds / 0.1) + 1; ++k) Tick(c);
  }
  std::vector<std::string> Of(const std::string& type) const {
    std::vector<std::string> out;
    for (const auto& l : lines)
      if (l.find("\"type\":\"" + type + "\"") != std::string::npos) out.push_back(l);
    return out;
  }
};
SessionContext Context() {
  SessionContext c;
  c.level = 20, c.souls = 500, c.deathNum = 3, c.playTimeMs = 3600000;
  c.area = "High Wall of Lothric", c.characterName = "Test";
  c.bosses = {{13000800, "Vordt of the Boreal Valley", "High Wall of Lothric", false},
              {13000830, "Dancer of the Boreal Valley", "High Wall of Lothric", false}};
  return c;
}
}  // namespace session_test

TEST(session_boss_kill_logged_when_its_flag_sets) {
  session_test::Recorder r;
  SessionContext c = session_test::Context();
  c.bosses[1].defeated = true;  // already dead when the session starts: no event
  r.Tick(c);
  r.Settle(c);
  CHECK(r.Of("boss").empty());
  c.bosses[0].defeated = true;
  c.playTimeMs = 3700000;
  r.Tick(c);
  auto kills = r.Of("boss");
  CHECK_EQ(kills.size(), size_t(1));
  CHECK(kills[0].find("\"flag\":13000800,\"name\":\"Vordt of the Boreal Valley\",\"area\":\"High Wall of Lothric\","
                      "\"playTimeMs\":3700000") != std::string::npos);
  r.Tick(c);  // still set: logged once
  CHECK_EQ(r.Of("boss").size(), size_t(1));
  // Not read on a tick (empty list): the baseline is kept, nothing logged.
  SessionContext unread = c;
  unread.bosses.clear();
  r.Tick(unread);
  CHECK_EQ(r.Of("boss").size(), size_t(1));
}

TEST(session_flags_set_just_after_the_start_are_baseline_not_kills) {
  session_test::Recorder r;
  SessionContext c = session_test::Context();
  r.Tick(c);
  c.bosses[0].defeated = true;  // the game still loading the character's flags
  r.Tick(c);
  r.Settle(c);
  CHECK(r.Of("boss").empty());
  c.bosses[1].defeated = true;  // a real kill later on
  r.Tick(c);
  CHECK_EQ(r.Of("boss").size(), size_t(1));
}

TEST(session_another_character_starts_a_new_session) {
  session_test::Recorder r;
  SessionContext a = session_test::Context();
  r.Tick(a);
  r.Settle(a);
  // Character B: higher level, more deaths, bosses already dead, a different
  // weapon. The first tick still has A's boss list (read last tick).
  SessionContext b = a;
  b.characterName = "Other", b.level = 71, b.deathNum = 33, b.souls = 2279, b.playTimeMs = 84000000;
  b.hands[0] = {8030600, "Fire Reinforced Club", 0};
  r.Tick(b);
  b.bosses[0].defeated = b.bosses[1].defeated = true;
  r.Tick(b);
  r.Settle(b);
  auto starts = r.Of("start"), ends = r.Of("end");
  CHECK_EQ(starts.size(), size_t(2));
  CHECK_EQ(ends.size(), size_t(1));
  CHECK(starts[1].find("\"character\":\"Other\",\"level\":71,\"deathsTotal\":33") != std::string::npos);
  CHECK(ends[0].find("\"deaths\":0,\"gained\":0") != std::string::npos);
  CHECK(ends[0].find("\"level\":20,\"deathsTotal\":3") != std::string::npos);
  CHECK(r.Of("level").empty());
  CHECK(r.Of("boss").empty());
  CHECK(r.Of("death").empty());
  CHECK_EQ(r.tracker.stats.deaths, 0);
  CHECK_EQ(r.tracker.character, std::string("Other"));
  // B's own kill and death still count.
  b.deathNum = 34, b.souls = 0;
  r.Tick(b);
  CHECK_EQ(r.Of("death").size(), size_t(1));
  CHECK_EQ(r.tracker.stats.deaths, 1);
}

TEST(session_play_time_going_back_is_another_character) {
  session_test::Recorder r;
  SessionContext a = session_test::Context();
  r.Tick(a);
  a.playTimeMs += 30000;  // ordinary play: same session
  r.Tick(a);
  a.playTimeMs += 10;
  r.Tick(a);
  CHECK_EQ(r.Of("start").size(), size_t(1));
  SessionContext b = a;  // same name, much less play time: another save
  b.playTimeMs = 1000000;
  r.Tick(b);
  CHECK_EQ(r.Of("start").size(), size_t(2));
  CHECK_EQ(r.Of("end").size(), size_t(1));
  // An unreadable name (empty) isn't a switch.
  SessionContext unnamed = b;
  unnamed.characterName.clear();
  r.Tick(unnamed);
  CHECK_EQ(r.Of("start").size(), size_t(2));
}

TEST(session_death_in_a_boss_fight_is_an_attempt_with_its_phase) {
  session_test::Recorder r;
  SessionContext c = session_test::Context();
  r.Tick(c);
  c.bosses[0].fightPhase = 2;  // Vordt's health bar up, second phase
  r.Tick(c);
  c.souls = 0, c.deathNum = 4;  // died, souls dropped
  r.Tick(c);
  auto deaths = r.Of("death");
  CHECK_EQ(deaths.size(), size_t(1));
  CHECK(deaths[0].find("\"soulsLost\":500,\"deathsTotal\":4,\"boss\":13000800,"
                       "\"bossName\":\"Vordt of the Boreal Valley\",\"phase\":2}") != std::string::npos);
  r.Tick(c);  // the flags stay set until the map reloads
  c.bosses[0].fightPhase = 0;
  c.souls = 300;
  r.Tick(c);
  c.souls = 0, c.deathNum = 5;  // a death with no fight on
  r.Tick(c);
  deaths = r.Of("death");
  CHECK_EQ(deaths.size(), size_t(2));
  CHECK(deaths[1].find("\"boss\"") == std::string::npos);
}

TEST(session_attempt_counted_when_the_death_comes_after_the_reload) {
  session_test::Recorder r;
  SessionContext c = session_test::Context();
  r.Tick(c);
  c.bosses[0].fightPhase = 1;
  r.Tick(c);
  c.bosses[0].fightPhase = 0;  // the map reloaded before the death counter moved
  for (int k = 0; k < 50; ++k) r.Tick(c);
  c.souls = 0, c.deathNum = 4;
  r.Tick(c);
  auto deaths = r.Of("death");
  CHECK_EQ(deaths.size(), size_t(1));
  CHECK(deaths[0].find("\"boss\":13000800") != std::string::npos);
  // Long after a fight (past the grace), a death isn't an attempt.
  c.souls = 300;
  r.Tick(c);
  c.bosses[0].fightPhase = 1;
  r.Tick(c);
  c.bosses[0].fightPhase = 0;
  for (int k = 0; k < int(kBossFightGraceSeconds / 0.1) + 10; ++k) r.Tick(c);
  c.souls = 0, c.deathNum = 5;
  r.Tick(c);
  deaths = r.Of("death");
  CHECK_EQ(deaths.size(), size_t(2));
  CHECK(deaths[1].find("\"boss\"") == std::string::npos);
}

TEST(session_attempt_stays_with_the_fight_already_followed) {
  session_test::Recorder r;
  SessionContext c = session_test::Context();
  r.Tick(c);
  c.bosses[1].fightPhase = 1;  // the later boss in the list first
  r.Tick(c);
  c.bosses[0].fightPhase = 1;  // then another reads as on too
  r.Tick(c);
  c.souls = 0, c.deathNum = 4;
  r.Tick(c);
  auto deaths = r.Of("death");
  CHECK_EQ(deaths.size(), size_t(1));
  CHECK(deaths[0].find("\"boss\":13000830") != std::string::npos);
}

TEST(session_no_attempt_after_the_boss_is_killed) {
  session_test::Recorder r;
  SessionContext c = session_test::Context();
  r.Tick(c);
  c.bosses[0].fightPhase = 1;
  r.Tick(c);
  c.bosses[0].fightPhase = 0, c.bosses[0].defeated = true;  // killed
  r.Tick(c);
  c.souls = 0, c.deathNum = 4;  // died just after
  r.Tick(c);
  auto deaths = r.Of("death");
  CHECK_EQ(deaths.size(), size_t(1));
  CHECK(deaths[0].find("\"boss\"") == std::string::npos);
}

TEST(session_boss_fight_phase_from_its_flags) {
  BossInfo b{14500800, "Sister Friede", "Painted World of Ariandel", "required", "", {14505810}, {14505802, 14505813}};
  auto on = [](std::vector<uint32_t> set) { return [set](uint32_t f) { return std::count(set.begin(), set.end(), f) > 0; }; };
  CHECK_EQ(BossFightPhase(b, false, on({})), 0);                    // no fight
  CHECK_EQ(BossFightPhase(b, false, on({14505810})), 1);
  CHECK_EQ(BossFightPhase(b, false, on({14505810, 14505802})), 2);
  CHECK_EQ(BossFightPhase(b, false, on({14505810, 14505802, 14505813})), 3);
  CHECK_EQ(BossFightPhase(b, false, on({14505810, 14505813})), 3);  // the last phase set counts
  CHECK_EQ(BossFightPhase(b, true, on({14505810})), 0);             // defeated
  CHECK_EQ(BossFightPhase(b, false, on({14505802})), 0);            // a phase flag alone isn't a fight
  BossInfo one{13200800, "Ancient Wyvern", "Archdragon Peak", "optional", "", {13205810}, {}};
  CHECK_EQ(BossFightPhase(one, false, on({13205810})), 1);
}

TEST(session_weapons_logged_at_the_start_and_on_change) {
  session_test::Recorder r;
  SessionContext c = session_test::Context();
  r.Tick(c);  // hands not read yet
  CHECK(r.Of("gear").empty());
  c.hands[0] = {2010004, "Long Sword", 4};
  c.hands[1] = {21100000, "Knight Shield", 0};
  r.Tick(c);
  r.Tick(c);
  auto gear = r.Of("gear");
  CHECK_EQ(gear.size(), size_t(2));
  CHECK(gear[0].find("\"slot\":\"R\",\"id\":2010004,\"name\":\"Long Sword\",\"upgrade\":4}") != std::string::npos);
  CHECK(gear[1].find("\"slot\":\"L\",\"id\":21100000,\"name\":\"Knight Shield\",\"upgrade\":0}") != std::string::npos);
  c.hands[0] = {2010005, "Long Sword", 5};  // upgraded
  r.Tick(c);
  gear = r.Of("gear");
  CHECK_EQ(gear.size(), size_t(3));
  CHECK(gear[2].find("\"slot\":\"R\",\"id\":2010005,\"name\":\"Long Sword\",\"upgrade\":5}") != std::string::npos);
}
