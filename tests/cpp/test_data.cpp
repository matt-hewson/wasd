// Lints the real data/ tables through the tool's own loaders: each file
// parses, ids are unique, and the tables agree with each other (route and
// missable flags name real bosses, area names are areas of region_areas.tsv, key
// items exist under the same name in item_names.tsv). A typo in a TSV
// fails here instead of silently hiding a boss or a hint in game.

namespace data_test {
// The areas region_areas.tsv names, with how many regions each has.
std::unordered_map<std::string, int> Areas() {
  std::unordered_map<std::string, int> areas;
  for (const auto& [id, area] : AreaInfo().regionArea) ++areas[area];
  return areas;
}

bool HasCr(const std::string& s) { return s.find('\r') != std::string::npos; }

std::unordered_map<uint32_t, const BossInfo*> BossByFlag(const std::vector<BossInfo>& bosses) {
  std::unordered_map<uint32_t, const BossInfo*> m;
  for (const auto& b : bosses) m[b.flag] = &b;
  return m;
}

std::vector<uint32_t> NeedsFlags(const FlagNeeds& needs) {
  std::vector<uint32_t> out;
  for (const auto& group : needs)
    for (const auto& alt : group) out.insert(out.end(), alt.flags.begin(), alt.flags.end());
  return out;
}
}  // namespace data_test

// The 22 areas players talk about, each a place name.
TEST(data_region_areas) {
  auto areas = data_test::Areas();
  CHECK_EQ(areas.size(), size_t(22));
  for (const auto& [area, regions] : areas) CHECK(!area.empty() && !data_test::HasCr(area) && regions > 0);
  CHECK(LoadIdNameTable(Paths().Data(L"region_areas.tsv")).size() >= 100);
}

TEST(data_bosses) {
  auto bosses = LoadBosses();
  auto areas = data_test::Areas();
  CHECK_EQ(bosses.size(), size_t(25));
  std::unordered_map<uint32_t, int> seen;
  int dlc = 0;
  for (const auto& b : bosses) {
    CHECK(b.flag != 0);
    CHECK(++seen[b.flag] == 1);
    CHECK(!b.name.empty());
    CHECK(b.status == "required" || b.status == "optional");
    CHECK(!data_test::HasCr(b.name + b.area + b.status + b.note));
    if (!areas.count(b.area)) CHECK_EQ(b.area, std::string("(an area of region_areas.tsv)"));
    dlc += b.note.find("DLC") != std::string::npos;
  }
  CHECK_EQ(dlc, 6);
}

TEST(data_route_names_real_bosses_and_areas) {
  auto route = LoadRoute();
  auto bosses = LoadBosses();
  auto byFlag = data_test::BossByFlag(bosses);
  auto areas = data_test::Areas();
  CHECK(route.size() >= 20);
  std::unordered_map<std::string, int> ids;
  std::unordered_map<uint32_t, int> stepsPerBoss;
  for (const auto& st : route) {
    CHECK(++ids[st.id] == 1);
    CHECK(st.kind == "main" || st.kind == "optional" || st.kind == "dlc");
    if (!areas.count(st.area)) CHECK_EQ(st.area, std::string("(an area of region_areas.tsv)"));
    if (st.from != "-" && !areas.count(st.from)) CHECK_EQ(st.from, std::string("(an area or -)"));
    CHECK(!st.wayIn.empty() && !data_test::HasCr(st.wayIn));
    for (uint32_t f : st.bosses) {
      if (!byFlag.count(f)) CHECK_EQ(f, 0u);  // a boss flag not in bosses.tsv
      ++stepsPerBoss[f];
      // A step's bosses stand in that step's area.
      if (byFlag.count(f) && byFlag[f]->area != st.area) CHECK_EQ(byFlag[f]->name, st.area);
    }
    for (uint32_t f : data_test::NeedsFlags(st.needs))
      if (!byFlag.count(f)) CHECK_EQ(f, 0u);
  }
  // Every boss belongs to exactly one step (BossIsLater relies on it).
  for (const auto& b : bosses)
    if (stepsPerBoss[b.flag] != 1) CHECK_EQ(b.name, std::string("(in exactly one route step)"));
}

TEST(data_route_first_step_is_open) {
  auto route = LoadRoute();
  CHECK(!route.empty() && route[0].needs.empty());
}

// The route written for this project agrees with the
// game-derived tables: steps open in file order from earlier steps' bosses
// (so all are reachable and none waits on itself), each "from" is an
// earlier step's area, each step's bosses stand in its area, and the
// bosses' required / optional status agrees with the gates.
TEST(data_route_is_consistent) {
  auto route = LoadRoute();
  auto bosses = LoadBosses();  // kept alive: byFlag points into it
  auto byFlag = data_test::BossByFlag(bosses);
  std::unordered_map<uint32_t, bool> dead;  // bosses of the steps so far
  std::unordered_map<std::string, bool> areasSoFar;
  auto altMet = [&](const RouteAlt& alt) {
    int n = 0;
    for (uint32_t f : alt.flags) n += dead.count(f) ? 1 : 0;
    return alt.atLeast > 0 ? n >= alt.atLeast : n == static_cast<int>(alt.flags.size());
  };
  for (const auto& st : route) {
    for (const auto& group : st.needs) {
      bool any = false;
      for (const auto& alt : group) any |= altMet(alt);
      if (!any) CHECK_EQ(st.id, std::string("(a step whose gate earlier steps' bosses can meet)"));
      // One alternative only: its bosses are needed to go on (all of them, or
      // all N of N). For a main or DLC step, that makes them required.
      if (group.size() == 1 && (st.kind == "main" || st.kind == "dlc")) {
        const RouteAlt& alt = group[0];
        if (alt.atLeast == 0 || alt.atLeast == static_cast<int>(alt.flags.size()))
          for (uint32_t f : alt.flags)
            if (byFlag.count(f) && byFlag[f]->status != "required") CHECK_EQ(byFlag[f]->name, std::string("(required)"));
      }
    }
    if (st.from != "-" && !areasSoFar.count(st.from)) CHECK_EQ(st.id, std::string("(from an earlier step's area)"));
    for (uint32_t f : st.bosses) {
      if (!byFlag.count(f)) continue;  // data_route_names_real_bosses_and_areas reports it
      if (byFlag[f]->area != st.area) CHECK_EQ(byFlag[f]->area, st.area);  // where the boss stands
      if (st.kind == "optional" && byFlag[f]->status != "optional") CHECK_EQ(byFlag[f]->name, std::string("(optional)"));
      dead[f] = true;
    }
    areasSoFar[st.area] = true;
  }
}

TEST(data_missables) {
  auto missables = LoadMissables();
  auto areas = data_test::Areas();
  CHECK(missables.size() >= 15);
  std::unordered_map<std::string, int> ids;
  for (const auto& m : missables) {
    CHECK(++ids[m.id] == 1);
    CHECK(m.kind == "npc" || m.kind == "item" || m.kind == "ending");
    if (!areas.count(m.area)) CHECK_EQ(m.area, std::string("(an area of region_areas.tsv)"));
    CHECK(!m.relevant.empty());  // when it starts to matter
    CHECK(!m.shortText.empty() && !m.doText.empty() && !m.loseText.empty());
    CHECK(!data_test::HasCr(m.loseText));
    // Event flags are 8 digits, or 4 for global flags (NPC questline states).
    for (const auto* needs : {&m.relevant, &m.trigger, &m.done})
      for (uint32_t f : data_test::NeedsFlags(*needs)) CHECK(f >= 10000000u || (f >= 1000u && f <= 9999u));
  }
}

// item_names.tsv and bonfire_names.tsv are generated from the game's files:
// checked when this checkout has them (run
// tools\extract_treasures.py once), skipped with a note otherwise.
// key_items.tsv: the game's key-item category (66
// rows) plus Loretta's Bone; names from the generated item names.
TEST(data_key_items_exist_in_item_names) {
  auto keys = LoadKeyItems();
  CHECK_EQ(keys.size(), size_t(67));
  std::unordered_map<int32_t, int> seen;
  for (const auto& k : keys) {
    CHECK(++seen[k.goodsId] == 1);
    CHECK(k.kind == "key" || k.kind == "tome" || k.kind == "quest" || k.kind == "shop" || k.kind == "other");
    CHECK(!k.opens.empty() && !data_test::HasCr(k.opens));
  }
  auto names = LoadItemNameTable(FindItemNameTablePath());
  if (!names.loaded) {
    std::printf("    note: no generated item_names.tsv -- key item names not checked\n");
    return;
  }
  std::unordered_map<std::string, int> items;  // rows sharing a name are one item
  for (const auto& k : keys) {
    if (!names.goods.count(std::to_string(k.goodsId))) CHECK_EQ(k.goodsId, -1);  // every id has a game name
    ++items[k.name];
  }
  CHECK_EQ(items.size(), size_t(63));  // 62 in the category (Tower Key x2, Cinders of a Lord x4) + Loretta's Bone
}

TEST(data_name_tables_load) {
  if (!FileExists(Paths().Generated(L"bonfire_names.tsv")) || !FileExists(FindItemNameTablePath()) ||
      !FileExists(Paths().Generated(L"regions.tsv"))) {
    std::printf("    note: no generated name tables -- region, bonfire and item names not checked\n");
    return;
  }
  auto regions = LoadIdNameTable(Paths().Generated(L"regions.tsv"));
  CHECK(regions.size() >= 100);  // 126 on the full game
  for (const auto& [id, name] : regions) CHECK(!name.empty() && !data_test::HasCr(name));
  // Region 400102 is where the game put the player in (normal) Firelink Shrine, live (README).
  CHECK_EQ(regions[400102].substr(0, 15), std::string("Firelink Shrine"));
  for (const auto& [id, area] : LoadIdNameTable(Paths().Data(L"region_areas.tsv")))
    if (!regions.count(id)) CHECK_EQ(id, -1);  // every region with an area has a label

  // Boss names, as on their health bars: one per boss.
  if (FileExists(Paths().Generated(L"boss_names.tsv"))) {
    auto bossNames = LoadIdNameTable(Paths().Generated(L"boss_names.tsv"));
    auto bosses = LoadBosses();
    CHECK_EQ(bossNames.size(), bosses.size());
    for (const auto& b : bosses) CHECK(b.name.rfind("boss ", 0) != 0);  // every boss got its name
    CHECK_EQ(bossNames[14000830], std::string("Champion Gundyr"));      // not Iudex's NpcParam name
  } else {
    std::printf("    note: no generated boss_names.tsv -- boss names not checked\n");
  }


  auto bonfires = LoadIdNameTable(Paths().Generated(L"bonfire_names.tsv"));
  CHECK(bonfires.size() >= 75);  // 81 on the full game
  for (const auto& [id, name] : bonfires) CHECK(!name.empty() && !data_test::HasCr(name));
  CHECK_EQ(bonfires[4002952], std::string("Iudex Gundyr"));  // the bonfire seen in game at Iudex
  auto items = LoadItemNameTable(FindItemNameTablePath());
  CHECK(items.weapon.size() > 100 && items.goods.size() > 100 && items.accessory.size() > 50);
  // The equip-load code finds these two rings by name.
  bool havel = false, favor = false;
  for (const auto& [id, name] : items.accessory) {
    havel |= name == "Havel's Ring";
    favor |= name == "Ring of Favor";
  }
  CHECK(havel && favor);
}
