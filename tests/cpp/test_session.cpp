// Session bookkeeping (SessionStats): deaths, souls lost / recovered /
// gained, time per area. The game's death counter and the souls drop can
// land on different ticks, in either order; both orders are covered.
// And the area lookup (AreaInfo, real data), and the session log's boss
// kill, boss attempt and weapon events (SessionTracker into a capture).

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
  r.Tick(c);
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

TEST(session_death_near_an_undefeated_boss_is_an_attempt) {
  session_test::Recorder r;
  SessionContext c = session_test::Context();
  r.Tick(c);
  c.souls = 0, c.deathNum = 4;  // died, souls dropped
  c.bossInReach = 13000800, c.bossInReachName = "Vordt of the Boreal Valley";
  r.Tick(c);
  auto deaths = r.Of("death");
  CHECK_EQ(deaths.size(), size_t(1));
  CHECK(deaths[0].find("\"soulsLost\":500,\"deathsTotal\":4,\"boss\":13000800,\"bossName\":\"Vordt of the Boreal Valley\"}") !=
        std::string::npos);
  c.souls = 300;
  r.Tick(c);
  c.souls = 0, c.deathNum = 5, c.bossInReach = 0;  // a death nowhere near a boss
  r.Tick(c);
  deaths = r.Of("death");
  CHECK_EQ(deaths.size(), size_t(2));
  CHECK(deaths[1].find("\"boss\"") == std::string::npos);
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

TEST(session_boss_in_reach_is_the_nearest_undefeated_open_one) {
  auto boss = [](uint32_t flag, float dist, bool defeated = false, bool later = false) {
    LiveBoss b;
    b.flag = flag, b.dist = dist, b.defeated = defeated, b.later = later;
    return b;
  };
  CHECK_EQ(BossInReach({}, kBossFightRange), 0u);
  CHECK_EQ(BossInReach({boss(1, 40), boss(2, 25)}, kBossFightRange), 2u);
  CHECK_EQ(BossInReach({boss(1, 90)}, kBossFightRange), 0u);         // too far
  CHECK_EQ(BossInReach({boss(1, 10, true)}, kBossFightRange), 0u);   // already defeated
  CHECK_EQ(BossInReach({boss(1, 10, false, true)}, kBossFightRange), 0u);  // not open yet
  CHECK_EQ(BossInReach({boss(1, -1)}, kBossFightRange), 0u);         // position unknown
}
