// Session bookkeeping (SessionStats): deaths, souls lost / recovered /
// gained, time per area. The game's death counter and the souls drop can
// land on different ticks, in either order; both orders are covered.
// And the area lookup (AreaInfo, real data).

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
