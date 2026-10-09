// The route "needs" mini-language (data/route.tsv, data/missables.tsv)
// and the "later" boss rule built on it (BossIsLaterIn), checked against
// the real route and boss list.

namespace route_test {
constexpr uint32_t kIudex = 14000800, kVordt = 13000800, kGreatwood = 13100800, kSage = 13300850;
constexpr uint32_t kWatchers = 13300800, kDeacons = 13500800, kWolnir = 13800800, kYhorm = 13900800;
constexpr uint32_t kAldrich = 13700800, kLothric = 13410830, kDancer = 13000890, kDragonslayer = 13010800;
constexpr uint32_t kFriede = 14500800, kDemonPrince = 15000800;

std::function<bool(uint32_t)> Dead(std::vector<uint32_t> flags) {
  return [flags](uint32_t f) { return std::find(flags.begin(), flags.end(), f) != flags.end(); };
}

// The real boss list, each named "boss <flag>": these tests check how the
// route text is built, which mustn't depend on whether this checkout has
// the generated names.
std::vector<BossInfo> Bosses() {
  auto bosses = LoadBosses();
  for (auto& b : bosses) b.name = "boss " + std::to_string(b.flag);
  return bosses;
}
}  // namespace route_test

TEST(route_needs_parse_and_evaluate) {
  using namespace route_test;
  auto none = ParseNeeds("-");
  CHECK(none.empty());
  CHECK(NeedsMet(none, Dead({})));

  auto one = ParseNeeds("14000800");
  CHECK(!NeedsMet(one, Dead({})));
  CHECK(NeedsMet(one, Dead({kIudex})));

  auto both = ParseNeeds("13800800 13500800");  // all groups must hold
  CHECK(!NeedsMet(both, Dead({kWolnir})));
  CHECK(NeedsMet(both, Dead({kWolnir, kDeacons})));

  auto either = ParseNeeds("13000800|14000800");  // any alternative
  CHECK(NeedsMet(either, Dead({kIudex})));
  CHECK(!NeedsMet(either, Dead({kSage})));

  auto atLeast = ParseNeeds("3of:13300800,13900800,13700800,13410830");
  CHECK_EQ(atLeast.size(), size_t(1));
  CHECK_EQ(atLeast[0][0].atLeast, 3);
  CHECK(!NeedsMet(atLeast, Dead({kWatchers, kYhorm})));
  CHECK(NeedsMet(atLeast, Dead({kWatchers, kYhorm, kLothric})));

  auto mixed = ParseNeeds("4of:13300800,13900800,13700800,13410830|14500800");
  CHECK(NeedsMet(mixed, Dead({kFriede})));
  CHECK(!NeedsMet(mixed, Dead({kWatchers})));
}

TEST(route_later_bosses_on_a_new_game) {
  using namespace route_test;
  auto route = LoadRoute();
  auto bosses = route_test::Bosses();
  auto nothing = Dead({});
  std::string opens;
  CHECK(!BossIsLaterIn(route, bosses, kIudex, nothing, &opens));  // open from the start
  CHECK(BossIsLaterIn(route, bosses, kVordt, nothing, &opens));
  CHECK_EQ(opens, std::string("boss 14000800"));
  CHECK(BossIsLaterIn(route, bosses, kDancer, nothing, &opens));
  CHECK_EQ(opens, std::string("3 of boss 13300800 / boss 13900800 / boss 13700800 / boss 13410830 (0 so far)"));
}

TEST(route_later_bosses_after_vordt) {
  using namespace route_test;
  auto route = LoadRoute();
  auto bosses = route_test::Bosses();
  auto dead = Dead({kIudex, kVordt});
  std::string opens;
  CHECK(!BossIsLaterIn(route, bosses, kVordt, dead, &opens));      // defeated: never "later"
  CHECK(!BossIsLaterIn(route, bosses, kGreatwood, dead, &opens));  // Undead Settlement is open
  CHECK(BossIsLaterIn(route, bosses, kDancer, dead, &opens));      // the case found in High Wall
  CHECK(BossIsLaterIn(route, bosses, kYhorm, dead, &opens));
  CHECK_EQ(opens, std::string("boss 13800800 and boss 13500800"));
}

TEST(route_dancer_opens_after_three_lords) {
  using namespace route_test;
  auto route = LoadRoute();
  auto bosses = route_test::Bosses();
  std::string opens;
  CHECK(BossIsLaterIn(route, bosses, kDancer, Dead({kIudex, kVordt, kWatchers, kYhorm}), &opens));
  CHECK(opens.find("(2 so far)") != std::string::npos);
  CHECK(!BossIsLaterIn(route, bosses, kDancer, Dead({kIudex, kVordt, kWatchers, kYhorm, kAldrich}), &opens));
  CHECK(BossIsLaterIn(route, bosses, kDragonslayer, Dead({kWatchers, kYhorm, kAldrich}), &opens));
  CHECK_EQ(opens, std::string("boss 13000890"));
}

TEST(route_dlc_entry_has_two_ways_in) {
  using namespace route_test;
  auto route = LoadRoute();
  auto bosses = route_test::Bosses();
  std::string opens;
  CHECK(BossIsLaterIn(route, bosses, kDemonPrince, Dead({}), &opens));
  CHECK(opens.find(" or boss 14500800") != std::string::npos);
  CHECK(!BossIsLaterIn(route, bosses, kDemonPrince, Dead({kFriede}), &opens));
}

namespace route_test {
// Each step's state, as BuildRouteView works it out, for these dead bosses
// and visited areas.
std::vector<RouteEval> Evals(const std::vector<RouteStep>& route, const std::vector<BossInfo>& bosses,
                             const std::vector<uint32_t>& deadFlags, const std::vector<std::string>& visited) {
  auto dead = Dead(deadFlags);
  std::vector<RouteEval> ev(route.size());
  for (size_t i = 0; i < route.size(); ++i) {
    RouteEval& e = ev[i];
    e.open = NeedsMet(route[i].needs, dead);
    for (uint32_t f : route[i].bosses) {
      if (dead(f)) continue;
      bool required = true;
      for (const auto& b : bosses)
        if (b.flag == f) required = b.status == "required";
      (required ? e.requiredLeft : e.optionalLeft) += 1;
    }
    e.done = e.open && e.requiredLeft == 0 && e.optionalLeft == 0;
    e.visited = std::find(visited.begin(), visited.end(), route[i].area) != visited.end();
  }
  return ev;
}

std::string Next(const std::vector<RouteStep>& route, const std::vector<RouteEval>& ev) {
  int i = PickNextRouteStep(route, ev);
  return i < 0 ? "(none)" : route[i].id;
}
}  // namespace route_test

// The user's report (2026-10-09): in Undead Settlement, Vordt dead, the
// optional Greatwood alive -- the hint must point on to Road of Sacrifices,
// not back at the settlement's own way in.
TEST(route_hint_points_to_where_you_go_next) {
  using namespace route_test;
  auto route = LoadRoute();
  auto bosses = route_test::Bosses();
  std::vector<std::string> sofar = {"Cemetery of Ash", "Firelink Shrine", "High Wall of Lothric", "Undead Settlement"};
  CHECK_EQ(Next(route, Evals(route, bosses, {kIudex, kVordt}, sofar)), std::string("road"));
  // Been to the road, Crystal Sage alive: the required boss there is the way on.
  sofar.push_back("Road of Sacrifices");
  CHECK_EQ(Next(route, Evals(route, bosses, {kIudex, kVordt}, sofar)), std::string("road"));
  // A new game: Iudex.
  CHECK_EQ(Next(route, Evals(route, bosses, {}, {})), std::string("cemetery"));
}

TEST(route_hint_dancer_in_a_visited_area) {
  using namespace route_test;
  auto route = LoadRoute();
  auto bosses = route_test::Bosses();
  std::vector<uint32_t> dead = {kIudex, kVordt, kSage, kWatchers, kDeacons, kWolnir, kYhorm, 13700850u, kAldrich};
  std::vector<std::string> all;
  for (const auto& st : route) all.push_back(st.area);  // been everywhere the route names
  // High Wall was visited long ago, but the Dancer (required) is alive there now.
  CHECK_EQ(Next(route, Evals(route, bosses, dead, all)), std::string("dancer"));
}

TEST(route_hint_falls_back_to_optional_bosses_then_none) {
  using namespace route_test;
  auto route = LoadRoute();
  auto bosses = route_test::Bosses();
  std::vector<std::string> all;
  std::vector<uint32_t> everyone;
  for (const auto& st : route) all.push_back(st.area);
  for (const auto& b : bosses) everyone.push_back(b.flag);
  CHECK_EQ(Next(route, Evals(route, bosses, everyone, all)), std::string("(none)"));
  std::vector<uint32_t> butGreatwood;
  for (uint32_t f : everyone)
    if (f != kGreatwood) butGreatwood.push_back(f);
  CHECK_EQ(Next(route, Evals(route, bosses, butGreatwood, all)), std::string("settlement"));  // only optional left
}

TEST(route_unknown_flag_is_never_later) {
  auto route = LoadRoute();
  auto bosses = route_test::Bosses();
  CHECK(!BossIsLaterIn(route, bosses, 12345678u, route_test::Dead({}), nullptr));
}
