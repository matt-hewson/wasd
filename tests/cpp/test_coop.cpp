// Co-op: the shared state's binary form (bit packing, the FullSnapshot
// layout, the pasteable text code), the recorded part summed from session
// logs, and the results page's comparison of both players (your partner's
// data at the live page's spoiler rules: what you've reached is named,
// the rest only at Full).
// The packet layout is documented beside EncodeCoopSnapshot; these pin it.

namespace coop_test {
CoopFlagSet Flags(const std::vector<uint32_t>& ids, const std::vector<bool>& bits) {
  CoopFlagSet f;
  f.fingerprint = CoopFingerprint(ids);
  f.bits = bits;
  return f;
}

// The area list (CoopAreaList's shape: sorted names).
std::vector<std::string> Areas() { return {"Firelink Shrine", "Grand Archives", "High Wall of Lothric"}; }

// A player in Firelink Shrine with a few bosses, keys and pickups.
SharedGameState Sample() {
  SharedGameState s;
  s.capturedUtc = 1791800000;
  s.characterName = "Ashen One";
  s.level = 42;
  s.hp = 812, s.maxHp = 900, s.fp = 93, s.maxFp = 120, s.stamina = 95, s.maxStamina = 110;
  const uint8_t attrs[9] = {15, 10, 12, 10, 14, 13, 9, 9, 7};
  std::copy(attrs, attrs + 9, s.attrs);
  s.souls = 2399, s.deaths = 33, s.playSeconds = 84329, s.clearCount = 1;
  s.mapId = 0x28000000;  // m40_00_00_00
  s.playRegionId = 400110;  // normal world (400000-400099 is Untended Graves)
  s.lastBonfireId = 1400000;
  s.havePose = true;
  s.x = -120.5f, s.y = 12.25f, s.z = 33.0f, s.facing = 1.5f;
  s.bossesDefeated = Flags({14000800, 13000800, 13300850}, {true, true, false});
  s.keyItemsObtained = Flags({2000, 2001}, {true, false});
  s.keyItemsHeld = Flags({2000, 2001}, {true, false});
  s.pickupsFound = Flags({54000050, 54000060, 53000010}, {true, false, true});
  s.areaListFingerprint = CoopAreaListFingerprint(Areas());
  s.areas = {{0, 600, 2}, {2, 95, 0}};  // Firelink Shrine 10 min, 2 deaths; High Wall 1m 35s
  s.levels = {{1400, 40}, {1405, 42}};
  s.totals[0] = 695, s.totals[1] = 2, s.totals[2] = 1500, s.totals[3] = 500, s.totals[4] = 4000, s.totals[5] = 600;
  s.sessionAreaBits = 3;
  s.sessionWeapons = true;
  CoopSessionSummary newer, older;
  newer.startMin = 29300000, newer.minutes = 2, newer.deaths = 0, newer.gained = 1000, newer.levelStart = 40;
  newer.levelEnd = 42, newer.areaBits = 0b100, newer.kills = 1, newer.weapon[0] = 20100, newer.upgrade[0] = 3;
  newer.weapon[1] = 211000;
  older.startMin = 29298000, older.minutes = 10, older.deaths = 2, older.gained = 3000, older.levelStart = 38;
  older.levelEnd = 40, older.areaBits = 0b001, older.kills = 1;
  s.sessions = {newer, older};
  CoopBossRecord iudex, vordt;
  iudex.boss = 0, iudex.deaths = 0, iudex.session = 1, iudex.killMin = 29298005;
  vordt.boss = 1, vordt.deaths = 2, vordt.ttkMin = 25, vordt.session = 0, vordt.killMin = 29300001;
  s.bossRecords = {iudex, vordt};
  s.lifeSec = 420, s.lifeSession = 1, s.lifeArea = 0;
  s.firstDeathAt = 180, s.firstDeathSession = 1, s.firstDeathArea = 0;
  return s;
}

std::vector<uint8_t> Packet(const SharedGameState& s = Sample(), uint32_t seq = 7) { return EncodeCoopSnapshot(s, seq); }

CoopLists Lists() {
  CoopLists l;
  l.bossNames = {"Iudex Gundyr", "Vordt of the Boreal Valley", "Curse-rotted Greatwood"};
  l.bossAreas = {"Cemetery of Ash", "High Wall of Lothric", "Undead Settlement"};
  l.bossFlags = {14000800, 13000800, 13300850};  // the sample's bosses-defeated list
  l.keyItemNames = {"Small Lothric Banner", "Grave Key"};
  l.keyItemKinds = {"key", "key"};
  l.pickupAreas = {"Firelink Shrine", "Firelink Shrine", "High Wall of Lothric"};
  return l;
}
}  // namespace coop_test

TEST(coop_bits_round_trip_at_every_length) {
  for (size_t n : {size_t(0), size_t(1), size_t(7), size_t(8), size_t(9), size_t(31), size_t(32), size_t(33),
                   size_t(100), kCoopMaxFlags}) {
    for (int pattern = 0; pattern < 3; ++pattern) {  // all false, all true, alternating
      std::vector<bool> bits(n);
      for (size_t k = 0; k < n; ++k) bits[k] = pattern == 1 || (pattern == 2 && k % 2 == 0);
      CoopBitWriter w;
      for (bool b : bits) w.WriteBool(b);
      CHECK_EQ(w.BitCount(), n);
      CHECK_EQ(w.Bytes().size(), (n + 7) / 8);
      CoopBitReader r(w.Bytes().data(), w.Bytes().size());
      std::vector<bool> back;
      for (size_t k = 0; k < n; ++k) back.push_back(r.ReadBool());
      CHECK(r.Ok());
      CHECK(back == bits);
      CHECK(r.AtCleanEnd());
    }
  }
}

TEST(coop_bits_mixed_widths_lsb_first) {
  CoopBitWriter w;
  w.Write(5, 3);            // 101
  w.Write(0xFFFFFFFFu, 32);
  w.Write(0, 1);
  w.Write(0x2A, 6);
  CHECK_EQ(w.BitCount(), size_t(42));
  CHECK_EQ(static_cast<int>(w.Bytes()[0]), 0xFD);  // 101 then five 1s, least significant bit first
  CoopBitReader r(w.Bytes().data(), w.Bytes().size());
  CHECK_EQ(r.Read(3), 5u);
  CHECK_EQ(r.Read(32), 0xFFFFFFFFu);
  CHECK_EQ(r.Read(1), 0u);
  CHECK_EQ(r.Read(6), 0x2Au);
  CHECK(r.AtCleanEnd());
  CHECK_EQ(r.Read(8), 0u);  // past the end
  CHECK(!r.Ok());
}

TEST(coop_snapshot_round_trip) {
  SharedGameState s = coop_test::Sample();
  auto bytes = coop_test::Packet(s, 7);
  CHECK_EQ(static_cast<int>(bytes[0]), static_cast<int>(kCoopProtocolVersion));
  CHECK_EQ(static_cast<int>(bytes[1]), static_cast<int>(CoopMessage::kFullSnapshot));
  CoopPacket p = DecodeCoopPacket(bytes.data(), bytes.size());
  CHECK(p.ok);
  CHECK_EQ(p.error, std::string());
  CHECK_EQ(p.sequence, 7u);
  CHECK(p.state == s);
  CHECK(coop_test::Packet(s, 7) == bytes);  // deterministic

  // Values past their field's width are clamped, not wrapped.
  SharedGameState big = s;
  big.areas = {{1, 0x2000000, 7}};
  big.levels = {{0x200000, 2000}};
  CoopPacket clamped = DecodeCoopPacket(coop_test::Packet(big).data(), coop_test::Packet(big).size());
  CHECK(clamped.ok);
  CHECK_EQ(clamped.state.areas[0].seconds, 0xFFFFFFu);
  CHECK_EQ(clamped.state.levels[0].minutes, 0xFFFFFu);
  CHECK_EQ(static_cast<int>(clamped.state.levels[0].level), 1023);

  s.havePose = false;  // no position: four floats fewer
  auto noPose = coop_test::Packet(s, 8);
  CHECK_EQ(bytes.size() - noPose.size(), size_t(16));
  CoopPacket q = DecodeCoopPacket(noPose.data(), noPose.size());
  CHECK(q.ok);
  CHECK(q.state == s);
}

TEST(coop_snapshot_clamps_and_cuts_the_name_on_a_character) {
  SharedGameState s = coop_test::Sample();
  s.characterName = std::string(47, 'a') + "\xC3\xA9";  // 49 bytes, the last character two bytes long
  auto bytes = coop_test::Packet(s);
  CoopPacket p = DecodeCoopPacket(bytes.data(), bytes.size());
  CHECK(p.ok);
  CHECK_EQ(p.state.characterName, std::string(47, 'a'));
  CHECK_EQ(CoopClampU16(-5), uint16_t(0));
  CHECK_EQ(CoopClampU16(70000), uint16_t(0xFFFF));
}

TEST(coop_text_round_trip_ignores_whitespace) {
  auto bytes = coop_test::Packet();
  std::string text = CoopToText(bytes);
  CHECK_EQ(text.rfind("WASD-", 0), size_t(0));
  CHECK(text.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") ==
        std::string::npos);
  CHECK(CoopDecodeText(text).state == coop_test::Sample());
  std::string wrapped = "  " + text.substr(0, 30) + "\r\n" + text.substr(30, 40) + " \t" + text.substr(70) + "\n";
  CHECK(CoopDecodeText(wrapped).ok);
  // A plain base64url round trip at every remainder length.
  for (size_t n = 0; n < 7; ++n) {
    std::vector<uint8_t> in(n);
    for (size_t k = 0; k < n; ++k) in[k] = static_cast<uint8_t>(0xF0 + k);
    std::vector<uint8_t> out;
    CHECK(Base64UrlDecode(Base64UrlEncode(in), &out));
    CHECK(out == in);
  }
}

TEST(coop_text_refuses_damage_with_a_reason) {
  std::string text = CoopToText(coop_test::Packet());
  CHECK_EQ(CoopDecodeText("").error, std::string("nothing pasted"));
  CHECK(CoopDecodeText(text.substr(5)).error.find("not a WASD code") == 0);  // prefix missing
  std::string flipped = text;
  flipped[20] = flipped[20] == 'A' ? 'B' : 'A';
  CHECK(CoopDecodeText(flipped).error.find("damaged") == 0);                 // checksum
  CHECK(CoopDecodeText(text.substr(0, text.size() - 6)).error.find("damaged") == 0);  // cut short
  CHECK(CoopDecodeText(text.substr(0, 25) + "!" + text.substr(26)).error.find("damaged") == 0);  // not base64url
  CHECK(!CoopDecodeText("WASD-").ok);
}

TEST(coop_invalid_packets_are_refused) {
  auto good = coop_test::Packet();
  auto refuse = [](std::vector<uint8_t> b) { return DecodeCoopPacket(b.data(), b.size()); };
  CHECK_EQ(DecodeCoopPacket(nullptr, 0).error, std::string("empty"));
  CHECK(!refuse({1, 2, 0}).ok);  // shorter than the header
  {
    auto b = good;
    b[0] = 2;  // a newer protocol
    CoopPacket p = refuse(b);
    CHECK(!p.ok);
    CHECK(p.error.find("different WASD version") != std::string::npos);
  }
  {
    auto b = good;
    b[1] = 9;
    CHECK(refuse(b).error.find("unknown message type") == 0);
    b[1] = static_cast<uint8_t>(CoopMessage::kDelta);
    CHECK(refuse(b).error.find("not a snapshot") == 0);
  }
  {
    auto b = good;
    b.pop_back();  // truncated: the last flag bits are missing
    CHECK(refuse(b).error.find("truncated") == 0);
  }
  {
    auto b = good;
    b.push_back(0);  // a whole extra byte
    CHECK(refuse(b).error.find("unexpected data") == 0);
  }
  {
    auto b = good;
    b.back() |= 0x80;  // a padding bit set
    CHECK(refuse(b).error.find("unexpected data") == 0);
  }
  CHECK(refuse(std::vector<uint8_t>(kCoopMaxPacketBytes + 1, 1)).error.find("too long") == 0);
  {
    auto b = good;
    // Name length field: payload bits 32-37 = byte 6 + 4, low 6 bits. 63 > 48.
    b[kCoopHeaderBytes + 4] |= 0x3F;
    CHECK(refuse(b).error.find("bad character name length") == 0);
  }
  {
    SharedGameState s = coop_test::Sample();
    s.x = std::nanf("");
    CHECK(refuse(EncodeCoopSnapshot(s, 1)).error.find("position") == 0);
  }
  {
    SharedGameState s = coop_test::Sample();
    s.areas = {{1, 5, 0}, {1, 6, 0}};
    CHECK(refuse(EncodeCoopSnapshot(s, 1)).error.find("an area listed twice") == 0);
  }
}

TEST(coop_fingerprint_follows_ids_and_order) {
  CHECK(CoopFingerprint({}) != 0u);
  CHECK(CoopFingerprint({1, 2}) != CoopFingerprint({2, 1}));
  CHECK(CoopFingerprint({1, 2}) != CoopFingerprint({1, 2, 3}));
  CHECK_EQ(CoopFingerprint({14000800, 13000800}), CoopFingerprint({14000800, 13000800}));
}

TEST(coop_compare_rows_and_counts) {
  SharedGameState mine = coop_test::Sample(), partner = coop_test::Sample();
  partner.characterName = "Partner";
  partner.bossesDefeated.bits = {true, false, true};  // Greatwood only theirs, Vordt only mine
  partner.keyItemsObtained.bits = {true, true};
  partner.keyItemsHeld.bits = {false, true};
  partner.pickupsFound.bits = {true, true, true};  // the second Firelink pickup only theirs
  partner.capturedUtc = mine.capturedUtc - 90;
  CoopCompare c = BuildCoopCompare(mine, true, partner, true, coop_test::Lists(), coop_test::Areas(), "",
                                   mine.capturedUtc);
  CHECK(c.me.shown && c.partner.shown);
  CHECK_EQ(c.partner.name, std::string("Partner"));
  CHECK_EQ(c.partner.ageSeconds, int64_t(90));
  CHECK_EQ(c.me.ageSeconds, int64_t(0));
  CHECK_EQ(c.partner.attrs[0], 15);
  CHECK_EQ(c.partner.ng, 1);
  CHECK_EQ(c.partner.deaths, int64_t(33));
  CHECK_EQ(c.partner.bosses[0], 2);
  CHECK_EQ(c.partner.bosses[1], 3);
  CHECK_EQ(c.partner.keysHeld, 1);
  CHECK(c.bossesCompared && c.keysCompared && c.itemsCompared);
  CHECK_EQ(c.bosses.size(), size_t(3));
  CHECK_EQ(c.bosses[1].name, std::string("Vordt of the Boreal Valley"));  // yours: named
  CHECK_EQ(c.bosses[1].group, std::string("High Wall of Lothric"));
  CHECK(c.bosses[1].me && !c.bosses[1].partner);
  CHECK(!c.bosses[2].me && c.bosses[2].partner);
  CHECK(c.keys[1].partner && c.keys[1].partnerHeld && !c.keys[1].me);
  CHECK(c.keys[0].meHeld && !c.keys[0].partnerHeld);
  CHECK_EQ(c.items.size(), size_t(2));
  CHECK_EQ(c.items[0].area, std::string("Firelink Shrine"));
  CHECK_EQ(c.items[0].total, 2);
  CHECK_EQ(c.items[0].me, 1);
  CHECK_EQ(c.items[0].partner, 2);
  CHECK_EQ(c.items[0].both, 1);
  CHECK_EQ(c.items[1].area, std::string("High Wall of Lothric"));
  CHECK_EQ(c.partner.areas.size(), size_t(2));
  CHECK_EQ(c.partner.areas[0].name, std::string("Firelink Shrine"));
  CHECK_EQ(c.partner.areas[0].seconds, int64_t(600));
  CHECK_EQ(c.partner.areas[0].deaths, 2);
  CHECK_EQ(c.partner.levels.size(), size_t(2));
  CHECK_NEAR(c.partner.levels[1].first, 1405 / 60.0, 1e-9);
  CHECK(c.notes.empty());
}

TEST(coop_compare_names_what_only_your_partner_has_at_full_only) {
  // As the live page would show the same bosses and key items: yours are
  // named at every tier, the others only at Full.
  SharedGameState mine = coop_test::Sample(), partner = coop_test::Sample();
  partner.bossesDefeated.bits = {true, false, true};  // Greatwood (Undead Settlement): only theirs
  partner.keyItemsObtained.bits = {true, true};        // Grave Key: only theirs
  for (int tier : {kTierOff, kTierVague, kTierCategory}) {
    CoopCompare c = BuildCoopCompare(mine, true, partner, true, coop_test::Lists(), coop_test::Areas(), "", 0, tier);
    CHECK(c.bosses[2].partner);
    CHECK_EQ(c.bosses[2].name, std::string());
    CHECK_EQ(c.bosses[2].group, std::string());  // not even its area
    CHECK(c.keys[1].partner);
    CHECK_EQ(c.keys[1].name, std::string());
    CHECK_EQ(c.keys[1].group, std::string());
    CHECK_EQ(c.bosses[1].name, std::string("Vordt of the Boreal Valley"));  // yours
    CHECK(c.reached.count("Undead Settlement") == 0);
  }
  CoopCompare full = BuildCoopCompare(mine, true, partner, true, coop_test::Lists(), coop_test::Areas(), "", 0, kTierFull);
  CHECK_EQ(full.bosses[2].name, std::string("Curse-rotted Greatwood"));
  CHECK_EQ(full.bosses[2].group, std::string("Undead Settlement"));
  CHECK_EQ(full.keys[1].name, std::string("Grave Key"));
  // Once you've defeated it yourself, it's named.
  mine.bossesDefeated.bits[2] = true;
  CoopCompare d = BuildCoopCompare(mine, true, partner, true, coop_test::Lists(), coop_test::Areas(), "", 0);
  CHECK_EQ(d.bosses[2].name, std::string("Curse-rotted Greatwood"));
  CHECK_EQ(d.bosses[2].group, std::string("Undead Settlement"));
}

TEST(coop_compare_names_only_areas_you_have_reached) {
  SharedGameState mine = coop_test::Sample(), partner = coop_test::Sample();
  mine.areas = {{0, 600, 2}};          // you: only Firelink Shrine recorded
  mine.pickupsFound.bits = {true, false, false};
  mine.bossesDefeated.bits = {true, false, false};  // Cemetery of Ash
  partner.areas = {{0, 300, 0}, {1, 50, 1}, {2, 95, 3}};  // Grand Archives, High Wall: not yours
  CoopCompare c = BuildCoopCompare(mine, true, partner, true, coop_test::Lists(), coop_test::Areas(), "", 0);
  CHECK(c.reached == std::set<std::string>({"Firelink Shrine", "Cemetery of Ash"}));
  CHECK_EQ(c.partner.areas.size(), size_t(2));
  CHECK_EQ(c.partner.areas[0].name, std::string("Firelink Shrine"));
  CHECK_EQ(c.partner.areas[1].name, std::string(kCoopOtherAreas));
  CHECK_EQ(c.partner.areas[1].seconds, int64_t(145));
  CHECK_EQ(c.partner.areas[1].deaths, 4);
  CHECK_EQ(c.items.back().area, std::string(kCoopOtherAreas));  // High Wall's pickup
  CHECK_EQ(c.me.areas[0].name, std::string("Firelink Shrine"));
  // Where you are now counts as reached.
  CoopCompare g = BuildCoopCompare(mine, true, partner, true, coop_test::Lists(), coop_test::Areas(), "Grand Archives", 0);
  CHECK_EQ(g.partner.areas.size(), size_t(3));
  CHECK_EQ(g.partner.areas[1].name, std::string("Grand Archives"));
  CHECK_EQ(g.partner.areas[2].name, std::string(kCoopOtherAreas));
  // Not in the game: nothing reached, so none of the partner's areas are named.
  CoopCompare n = BuildCoopCompare(SharedGameState(), false, partner, true, coop_test::Lists(), coop_test::Areas(), "", 0);
  CHECK(n.reached.empty());
  CHECK_EQ(n.partner.areas.size(), size_t(1));
  CHECK_EQ(n.partner.areas[0].name, std::string(kCoopOtherAreas));
  // At Full, all of them.
  CoopCompare f = BuildCoopCompare(mine, true, partner, true, coop_test::Lists(), coop_test::Areas(), "", 0, kTierFull);
  CHECK_EQ(f.partner.areas.size(), size_t(3));
  CHECK_EQ(f.partner.areas[1].name, std::string("Grand Archives"));
  CHECK_EQ(f.items.back().area, std::string("High Wall of Lothric"));
}

TEST(coop_partner_place_only_where_you_have_been) {
  const std::set<std::string> reached = {"Firelink Shrine"};
  CoopPlayerView v;
  v.shown = true;
  CoopPlaceView(&v, "Firelink Shrine", "Firelink Shrine (roof)", "Firelink Shrine", &reached);
  CHECK_EQ(v.area, std::string("Firelink Shrine"));
  CHECK_EQ(v.bonfire, std::string("Firelink Shrine"));
  CHECK(!v.elsewhere);
  CoopPlaceView(&v, "Undead Settlement", "Undead Settlement (sewers)", "Dilapidated Bridge", &reached);
  CHECK(v.area.empty() && v.region.empty() && v.bonfire.empty());
  CHECK(v.elsewhere);
  CoopPlaceView(&v, "", "arena 7", "", &reached);  // a region with no area
  CHECK(v.region.empty());
  CHECK(v.elsewhere);
  CoopPlaceView(&v, "Undead Settlement", "Undead Settlement (sewers)", "Dilapidated Bridge", nullptr);  // yours
  CHECK_EQ(v.bonfire, std::string("Dilapidated Bridge"));
  CHECK(!v.elsewhere);
  CoopPlayerView hidden;  // not shown: left alone
  CoopPlaceView(&hidden, "Firelink Shrine", "", "", &reached);
  CHECK(hidden.area.empty());
  // The live page's tiers for a place: at Category "a new area beyond" the
  // area it opens from (if you've been there), at Full the names.
  CoopPlaceView(&v, "Undead Settlement", "Undead Settlement (sewers)", "Dilapidated Bridge", &reached, kTierCategory,
                "Firelink Shrine");
  CHECK(v.elsewhere && v.area.empty());
  CHECK_EQ(v.beyond, std::string("Firelink Shrine"));
  CoopPlaceView(&v, "Undead Settlement", "", "", &reached, kTierCategory, "High Wall of Lothric");
  CHECK_EQ(v.beyond, std::string("somewhere you've been"));
  CoopPlaceView(&v, "Undead Settlement", "", "", &reached, kTierVague, "Firelink Shrine");
  CHECK(v.elsewhere && v.beyond.empty());
  CoopPlaceView(&v, "Undead Settlement", "Undead Settlement (sewers)", "Dilapidated Bridge", &reached, kTierFull);
  CHECK_EQ(v.bonfire, std::string("Dilapidated Bridge"));
  CHECK(!v.elsewhere && v.beyond.empty());
}

TEST(coop_compare_distance_only_on_the_same_map) {
  SharedGameState mine = coop_test::Sample(), partner = coop_test::Sample();
  mine.facing = 0;
  partner.x = mine.x, partner.y = mine.y + 2, partner.z = mine.z - 10;  // 10 m ahead (facing 0 looks to -z)
  auto build = [&] {
    return BuildCoopCompare(mine, true, partner, true, coop_test::Lists(), coop_test::Areas(), "", 0);
  };
  CoopCompare v = build();
  CHECK(v.sameMap);
  CHECK_NEAR(v.dist, std::sqrt(104.0), 1e-4);
  CHECK_NEAR(v.dy, 2.0, 1e-6);
  CHECK_EQ(v.clock, std::string("12 o'clock"));
  partner.playRegionId = 400050;  // Untended Graves: same map number, other world
  CHECK(!build().sameMap);
  CHECK_EQ(build().dist, -1.0f);
  partner = coop_test::Sample();
  partner.mapId = 0x1E000000;  // m30
  CHECK(!build().sameMap);
  partner = coop_test::Sample();
  mine.havePose = false;
  CHECK(build().sameMap);
  CHECK_EQ(build().dist, -1.0f);
}

TEST(coop_compare_flags_lists_that_differ_and_waits_for_yours) {
  SharedGameState mine = coop_test::Sample(), partner = coop_test::Sample();
  partner.pickupsFound = coop_test::Flags({54000050}, {true});  // another journey's pickups
  partner.areaListFingerprint = 1;                              // another area list
  CoopCompare v = BuildCoopCompare(mine, true, partner, true, coop_test::Lists(), coop_test::Areas(), "", 0);
  CHECK_EQ(v.partner.items[0], 1);
  CHECK(!v.itemsCompared);
  CHECK(v.partner.areas.empty());
  CHECK_EQ(v.notes.size(), size_t(3));
  CHECK(v.notes[0].find("Area lists differ") == 0);
  CHECK(v.notes[1].find("Item lists differ") == 0);
  for (const auto& row : v.items) CHECK_EQ(row.partner, 0);
  // Your own sets not read yet: the partner's counts, nothing compared, no warning.
  SharedGameState unread = coop_test::Sample();
  unread.bossesDefeated = CoopFlagSet();
  CoopCompare u = BuildCoopCompare(unread, true, coop_test::Sample(), true, coop_test::Lists(), coop_test::Areas(),
                                   "", 0);
  CHECK_EQ(u.partner.bosses[0], 2);
  CHECK(u.bosses.empty() && u.items.empty());
  CHECK(u.notes.empty());
  // Not in the game at all: only the partner.
  CoopCompare n = BuildCoopCompare(SharedGameState(), false, coop_test::Sample(), true, coop_test::Lists(),
                                   coop_test::Areas(), "", 0);
  CHECK(!n.me.shown && n.partner.shown);
  // No partner: only you.
  CoopCompare m = BuildCoopCompare(mine, true, SharedGameState(), false, coop_test::Lists(), coop_test::Areas(),
                                   "", 0);
  CHECK(m.me.shown && !m.partner.shown && m.bosses.empty());
}

TEST(coop_sessions_sum_time_deaths_and_levels) {
  const std::vector<std::string> areas = coop_test::Areas();
  std::string a =
      "{\"type\":\"start\",\"t\":0,\"character\":\"Ashen One\",\"level\":40,\"deathsTotal\":3,\"playTimeMs\":60000}\n"
      "{\"type\":\"enter\",\"t\":1.0,\"area\":\"Firelink Shrine\"}\n"
      "{\"type\":\"sample\",\"t\":30.0,\"level\":40,\"souls\":1,\"deathsTotal\":3,\"area\":\"Firelink Shrine\",\"playTimeMs\":120000}\n"
      "{\"type\":\"enter\",\"t\":61.0,\"area\":\"High Wall of Lothric\"}\r\n"
      "{\"type\":\"death\",\"t\":70.0,\"area\":\"High Wall of Lothric\",\"soulsLost\":5}\n"
      "{\"type\":\"death\",\"t\":75.0,\"area\":\"Nowhere\",\"soulsLost\":5}\n"
      "{\"type\":\"sample\",\"t\":90.0,\"level\":41,\"souls\":1,\"deathsTotal\":4,\"area\":\"High Wall of Lothric\",\"playTimeMs\":240000}\n"
      "{\"type\":\"end\",\"t\":100.0,\"seconds\":100.0,\"deaths\":1,\"gained\":0,\"level\":42,\"deathsTotal\":4,\"playTimeMs\":300000}\n";
  std::string b =  // no end (a crash): runs to the last event; a line cut off mid-write
      "{\"type\":\"enter\",\"t\":0.0,\"area\":\"Firelink Shrine\"}\n"
      "{\"type\":\"pos\",\"t\":20.0,\"x\":1,\"y\":2,\"z\":3}\n"
      "{\"type\":\"death\",\"t\":25";
  SharedGameState s;
  SummariseCoopSessions({a, b}, areas, {}, &s);
  CHECK_EQ(s.areaListFingerprint, CoopAreaListFingerprint(areas));
  CHECK_EQ(s.areas.size(), size_t(2));
  CHECK_EQ(static_cast<int>(s.areas[0].index), 0);  // Firelink Shrine: 60 + 20
  CHECK_EQ(s.areas[0].seconds, 80u);
  CHECK_EQ(static_cast<int>(s.areas[0].deaths), 0);
  CHECK_EQ(static_cast<int>(s.areas[1].index), 2);  // High Wall: 39 s, 1 death
  CHECK_EQ(s.areas[1].seconds, 39u);
  CHECK_EQ(static_cast<int>(s.areas[1].deaths), 1);
  CHECK_EQ(s.levels.size(), size_t(3));             // 40 at 2 min, 41 at 4, 42 at 5
  CHECK_EQ(s.levels[0].minutes, 2u);
  CHECK_EQ(static_cast<int>(s.levels[2].level), 42);
  CHECK_EQ(CoopJsonField("{\"area\":\"A \\\"B\\\"\",\"t\":3.5}", "area"), std::string("A \"B\""));
  CHECK_EQ(CoopJsonField("{\"area\":\"A\",\"t\":3.5}", "t"), std::string("3.5"));
  CHECK_EQ(CoopJsonField("{\"area\":\"A\"}", "t"), std::string());

  // A long climb is thinned to kCoopMaxLevelPoints, first and last kept.
  std::string many;
  for (int k = 1; k <= 100; ++k)
    many += "{\"type\":\"sample\",\"t\":" + std::to_string(k) + ",\"level\":" + std::to_string(k + 10) +
            ",\"playTimeMs\":" + std::to_string(k * 60000) + "}\n";
  SummariseCoopSessions({many}, areas, {}, &s);
  CHECK_EQ(s.levels.size(), kCoopMaxLevelPoints);
  CHECK_EQ(static_cast<int>(s.levels.front().level), 11);
  CHECK_EQ(static_cast<int>(s.levels.back().level), 110);
  CHECK(s.areas.empty());
}

TEST(coop_code_carries_sessions_bosses_and_records) {
  SharedGameState s = coop_test::Sample();
  CoopPacket p = DecodeCoopPacket(coop_test::Packet(s).data(), coop_test::Packet(s).size());
  CHECK(p.ok);
  CHECK(p.state == s);
  CHECK_EQ(p.state.sessions.size(), size_t(2));
  CHECK_EQ(p.state.sessions[0].weapon[0], 20100u);
  CHECK_EQ(p.state.bossRecords[1].ttkMin, uint16_t(25));
  // Without weapons the sessions are 46 bits shorter each.
  SharedGameState noWeapons = s;
  noWeapons.sessionWeapons = false;
  for (auto& x : noWeapons.sessions) x.weapon[0] = x.weapon[1] = 0, x.upgrade[0] = x.upgrade[1] = 0;
  CoopPacket q = DecodeCoopPacket(coop_test::Packet(noWeapons).data(), coop_test::Packet(noWeapons).size());
  CHECK(q.ok);
  CHECK(q.state == noWeapons);
  CHECK(coop_test::Packet(s).size() - coop_test::Packet(noWeapons).size() >= 11);
  // A record pointing past the sessions sent is written as "none".
  SharedGameState fewer = s;
  fewer.sessions.pop_back();
  CoopPacket f = DecodeCoopPacket(coop_test::Packet(fewer).data(), coop_test::Packet(fewer).size());
  CHECK(f.ok);
  CHECK_EQ(static_cast<int>(f.state.bossRecords[0].session), static_cast<int>(kCoopNone6));
  CHECK_EQ(static_cast<int>(f.state.lifeSession), static_cast<int>(kCoopNone6));
}

TEST(coop_code_refuses_records_without_their_session) {
  // Two sessions sent; a boss record says session 5. The encoder never
  // writes that (it writes "none"), so set the field's bits by hand: find
  // its lowest bit by encoding session 0 and session 1, then set bit 2 too.
  SharedGameState s = coop_test::Sample();
  s.bossRecords.resize(1);
  s.bossRecords[0].session = 0;
  auto zero = coop_test::Packet(s);
  s.bossRecords[0].session = 1;
  auto one = coop_test::Packet(s);
  CHECK_EQ(zero.size(), one.size());
  size_t bit = 0;
  for (size_t k = 0; k < zero.size() * 8; ++k)
    if (((zero[k / 8] ^ one[k / 8]) >> (k % 8)) & 1u) bit = k;
  auto five = one;
  five[(bit + 2) / 8] = static_cast<uint8_t>(five[(bit + 2) / 8] | (1u << ((bit + 2) % 8)));
  CHECK_EQ(DecodeCoopPacket(five.data(), five.size()).error,
           std::string("a boss record names a session that isn't there"));
  CHECK(DecodeCoopPacket(one.data(), one.size()).ok);
}

TEST(coop_sessions_give_totals_sessions_bosses_and_records) {
  const std::vector<std::string> areas = coop_test::Areas();  // Firelink Shrine, Grand Archives, High Wall of Lothric
  const std::vector<uint32_t> bosses = {14000800, 13000800};  // Iudex Gundyr, Vordt
  std::string older =
      "{\"type\":\"start\",\"t\":0,\"character\":\"A\",\"level\":10,\"startedAt\":\"2026-10-05T20:00:00\"}\n"
      "{\"type\":\"gear\",\"t\":0,\"slot\":\"R\",\"id\":2010000,\"name\":\"Long Sword\",\"upgrade\":0}\n"
      "{\"type\":\"enter\",\"t\":0,\"area\":\"Firelink Shrine\"}\n"
      "{\"type\":\"boss\",\"t\":300,\"flag\":14000800,\"name\":\"Iudex Gundyr\",\"area\":\"Cemetery of Ash\"}\n"
      "{\"type\":\"gear\",\"t\":600,\"slot\":\"R\",\"id\":2010003,\"name\":\"Long Sword\",\"upgrade\":3}\n"
      "{\"type\":\"enter\",\"t\":900,\"area\":\"High Wall of Lothric\"}\n"
      "{\"type\":\"death\",\"t\":1200,\"area\":\"High Wall of Lothric\",\"soulsLost\":700,\"boss\":13000800}\n"
      "{\"type\":\"bloodstain\",\"t\":1300,\"souls\":700}\n"
      "{\"type\":\"end\",\"t\":3600,\"seconds\":3600,\"gained\":2000,\"level\":12}\n";
  std::string newer =
      "{\"type\":\"start\",\"t\":0,\"character\":\"A\",\"level\":12,\"startedAt\":\"2026-10-06T21:00:00\"}\n"
      "{\"type\":\"enter\",\"t\":0,\"area\":\"High Wall of Lothric\"}\n"
      "{\"type\":\"death\",\"t\":100,\"area\":\"High Wall of Lothric\",\"soulsLost\":50,\"boss\":13000800}\n"
      "{\"type\":\"boss\",\"t\":400,\"flag\":13000800,\"name\":\"Vordt of the Boreal Valley\"}\n"
      "{\"type\":\"boss\",\"t\":500,\"flag\":99999999,\"name\":\"Not in the list\"}\n"
      "{\"type\":\"end\",\"t\":1200,\"seconds\":1200,\"gained\":800,\"level\":14}\n";
  SharedGameState s;
  SummariseCoopSessions({newer, older}, areas, bosses, &s);  // any order: sorted by start
  CHECK_EQ(s.totals[0], 4800u);  // recorded seconds
  CHECK_EQ(s.totals[1], 2u);     // deaths
  CHECK_EQ(s.totals[2], 750u);   // lost
  CHECK_EQ(s.totals[3], 700u);   // recovered
  CHECK_EQ(s.totals[4], 2800u);  // gained
  CHECK_EQ(s.totals[5], 3600u);  // longest session
  // Areas in the order first visited: Firelink, then High Wall.
  CHECK_EQ(s.areas.size(), size_t(2));
  CHECK_EQ(static_cast<int>(s.areas[0].index), 0);
  CHECK_EQ(static_cast<int>(s.areas[1].index), 2);
  // Sessions newest first.
  CHECK_EQ(s.sessions.size(), size_t(2));
  CHECK_EQ(s.sessions[0].startMin, static_cast<uint32_t>(CoopLocalStampToUnixMinutes("2026-10-06T21:00:00")));
  CHECK_EQ(s.sessions[1].startMin + 25 * 60, s.sessions[0].startMin);
  CHECK_EQ(s.sessions[0].minutes, uint16_t(20));
  CHECK_EQ(s.sessions[0].deaths, uint16_t(1));
  CHECK_EQ(s.sessions[0].gained, 800u);
  CHECK_EQ(s.sessions[0].levelStart, uint16_t(12));
  CHECK_EQ(s.sessions[0].levelEnd, uint16_t(14));
  CHECK_EQ(s.sessions[0].areaBits, uint64_t(0b100));
  CHECK_EQ(s.sessions[1].areaBits, uint64_t(0b101));
  CHECK_EQ(static_cast<int>(s.sessions[0].kills), 2);
  CHECK_EQ(static_cast<int>(s.sessionAreaBits), 3);
  // Weapons: the older session held the Long Sword +3 longest (50 of 60 m).
  CHECK(s.sessionWeapons);
  CHECK_EQ(s.sessions[1].weapon[0], 20100u);
  CHECK_EQ(static_cast<int>(s.sessions[1].upgrade[0]), 3);
  CHECK_EQ(s.sessions[0].weapon[0], 0u);  // none logged in the newer one
  // Bosses: Iudex first try in the older session, Vordt after 2 attempts.
  CHECK_EQ(s.bossRecords.size(), size_t(2));
  CHECK_EQ(static_cast<int>(s.bossRecords[0].boss), 0);
  CHECK_EQ(s.bossRecords[0].deaths, uint16_t(0));
  CHECK_EQ(s.bossRecords[0].ttkMin, uint16_t(0xFFFF));
  CHECK_EQ(static_cast<int>(s.bossRecords[0].session), 1);
  CHECK_EQ(s.bossRecords[0].killMin, s.sessions[1].startMin + 5);
  CHECK_EQ(static_cast<int>(s.bossRecords[1].boss), 1);
  CHECK_EQ(s.bossRecords[1].deaths, uint16_t(2));
  CHECK_EQ(s.bossRecords[1].ttkMin, uint16_t(47));  // first attempt at 20 m, kill at 1 h 06 m 40 s
  CHECK_EQ(static_cast<int>(s.bossRecords[1].session), 0);
  // Records: the longest life is the older session's 40 m after its death.
  CHECK_EQ(s.lifeSec, 2400u);
  CHECK_EQ(static_cast<int>(s.lifeSession), 1);
  CHECK_EQ(static_cast<int>(s.lifeArea), 2);
  CHECK_EQ(s.firstDeathAt, 1200u);
  CHECK_EQ(static_cast<int>(s.firstDeathSession), 1);
  CHECK_EQ(static_cast<int>(s.firstDeathArea), 2);
  // It all survives the code.
  CoopPacket p = CoopDecodeText(CoopToText(EncodeCoopSnapshot(s, 1)));
  CHECK(p.ok);
  CHECK(p.state == s);
  // No sessions at all: empty, no records.
  SharedGameState none;
  SummariseCoopSessions({}, areas, bosses, &none);
  CHECK(none.sessions.empty() && none.bossRecords.empty());
  CHECK_EQ(none.firstDeathAt, 0xFFFFFFu);
  CHECK_EQ(CoopLocalStampToUnixMinutes("not a date"), int64_t(0));
}

TEST(coop_code_fits_one_chat_message) {
  SharedGameState s = coop_test::Sample();
  std::string leftOut = "x";
  std::string code = FitCoopCode(s, 1, &leftOut);
  CHECK(code.size() <= kCoopMaxCodeChars);
  CHECK_EQ(leftOut, std::string());
  CHECK(CoopDecodeText(code).state == s);
  // A full game (about 1,000 pickups, 40 sessions with weapons) fits in
  // about 1,750 characters. With 4,000 pickups and 63 boss records it
  // doesn't: the weapons per session go first, then the oldest sessions.
  SharedGameState big = s;
  big.pickupsFound = coop_test::Flags(std::vector<uint32_t>(4000, 1), std::vector<bool>(4000, true));
  big.sessionAreaBits = 22;
  big.sessions.assign(kCoopMaxSessions, s.sessions[0]);
  big.bossRecords.assign(63, s.bossRecords[1]);
  code = FitCoopCode(big, 1, &leftOut);
  CHECK(code.size() <= kCoopMaxCodeChars);
  CHECK(leftOut.find("the weapons per session") == 0);
  CHECK(leftOut.find("oldest session") != std::string::npos);
  CoopPacket p = CoopDecodeText(code);
  CHECK(p.ok);
  CHECK(!p.state.sessionWeapons);
  CHECK(p.state.sessions.size() < kCoopMaxSessions);
  CHECK(p.state.sessions.size() >= 15);
  // A full game fits whole: nothing left out.
  SharedGameState full = big;
  full.bossRecords.resize(25);  // DS3 has 25 bosses
  full.pickupsFound = coop_test::Flags(std::vector<uint32_t>(1000, 1), std::vector<bool>(1000, true));
  code = FitCoopCode(full, 1, &leftOut);
  CHECK_EQ(leftOut, std::string());
  CHECK(code.size() <= kCoopMaxCodeChars);
  CHECK(code.size() > 1500);
  CHECK(CoopDecodeText(code).state == full);
}

TEST(coop_describe_lists_what_a_code_holds) {
  CoopPacket p = CoopDecodeText(CoopToText(coop_test::Packet()));
  std::string d = DescribeCoopPacket(p);
  CHECK(d.find("sequence 7") != std::string::npos);
  CHECK(d.find("character  Ashen One") != std::string::npos);
  CHECK(d.find("map        m40_00_00_00") != std::string::npos);
  CHECK(d.find("bosses defeated    2 / 3") != std::string::npos);
  CHECK(d.find("attributes 15 10 12 10 14 13 9 9 7") != std::string::npos);
  CHECK(d.find("play time  23h 25m") != std::string::npos);
  CHECK(d.find("journey    NG+1") != std::string::npos);
  CHECK(d.find("recorded   2 area(s), 0h 11m, 2 death(s)") != std::string::npos);
  CHECK(d.find("levels     2 point(s), level 40 at 23h 20m to 42 at 23h 25m") != std::string::npos);
  CHECK(d.find("totals     0h 11m recorded, 2 death(s), 1500 souls lost, 500 recovered, 4000 gained, longest 0h 10m") !=
        std::string::npos);
  CHECK(d.find("sessions   2, with weapons, 2025-09-14 to 2025-09-16 (UTC)") != std::string::npos);
  CHECK(d.find("bosses     2 kill(s) recorded") != std::string::npos);
  CHECK(d.find("captured   2026-10-12") != std::string::npos);
}

TEST(coop_sessions_together_overlap_only) {
  auto at = [](uint32_t start, uint16_t minutes) {
    CoopSessionSummary x;
    x.startMin = start, x.minutes = minutes;
    return x;
  };
  // Mine: 100-160 and 200-230. Partner: 150-210 overlaps both.
  auto t = CoopSessionsTogether({at(200, 30), at(100, 60)}, {at(150, 60)});
  CHECK_EQ(t.size(), size_t(2));
  CHECK_EQ(t[0].startMin, 200u);  // newest first
  CHECK_EQ(t[0].minutes, 10u);
  CHECK_EQ(t[0].me, size_t(0));
  CHECK_EQ(t[1].startMin, 150u);
  CHECK_EQ(t[1].minutes, 10u);
  CHECK_EQ(t[1].me, size_t(1));
  CHECK_EQ(t[1].partner, size_t(0));
  CHECK(CoopSessionsTogether({at(100, 60)}, {at(160, 30)}).empty());  // touching isn't overlapping
  CHECK(CoopSessionsTogether({at(100, 60)}, {at(300, 30)}).empty());
  CHECK(CoopSessionsTogether({at(0, 60)}, {at(0, 60)}).empty());      // no start time
  auto inside = CoopSessionsTogether({at(100, 60)}, {at(110, 20)});
  CHECK_EQ(inside.size(), size_t(1));
  CHECK_EQ(inside[0].minutes, 20u);
}

TEST(coop_pair_names_only_what_you_have_reached) {
  SharedGameState mine = coop_test::Sample(), partner = coop_test::Sample();
  // You: Iudex defeated (boss 0); Vordt (1) not. Partner killed both.
  mine.bossesDefeated.bits = {true, false, false};
  partner.characterName = "Partner";
  // The partner's newer session was in Grand Archives (bit 1) and High Wall (bit 2) with a Claymore.
  partner.sessions[0].areaBits = 0b110;
  partner.sessions[0].weapon[0] = 30100, partner.sessions[0].upgrade[0] = 5;  // Claymore: you've never held one
  partner.sessions[1].weapon[0] = 20115;                                       // a Long Sword, infused: you have held one
  partner.lifeArea = 1;  // Grand Archives
  const std::set<std::string> reached = {"Firelink Shrine", "High Wall of Lothric"};
  auto names = [](int32_t id) {
    return id == 2010000 ? std::string("Long Sword") : id == 2011500 ? std::string("Heavy Long Sword")
         : id == 3010000 ? std::string("Claymore") : std::string();
  };
  CoopPair c = BuildCoopPair(mine, true, partner, true, coop_test::Lists(), coop_test::Areas(), reached, names);
  // Yours: everything named.
  CHECK_EQ(c.me.sessions[0].weapons[0].name, std::string("Long Sword"));
  CHECK_EQ(c.me.bosses[1].name, std::string("Vordt of the Boreal Valley"));
  CHECK_EQ(c.me.totals[0], 695u);
  // The partner's: only what you've reached.
  CHECK(c.partner.sessions[0].areas == std::vector<std::string>({"High Wall of Lothric", kCoopOtherAreas}));
  CHECK(c.partner.sessions[0].weapons[0].any);
  CHECK_EQ(c.partner.sessions[0].weapons[0].name, std::string());  // a weapon you haven't found
  CHECK_EQ(c.partner.sessions[0].weapons[0].upgrade, 5);
  CHECK_EQ(c.partner.sessions[1].weapons[0].name, std::string("Heavy Long Sword"));
  CHECK(!c.partner.sessions[1].weapons[1].any);
  CHECK_EQ(c.partner.bosses[0].name, std::string("Iudex Gundyr"));  // you've defeated it
  CHECK_EQ(c.partner.bosses[1].name, std::string());                 // you haven't
  CHECK_EQ(c.partner.bosses[1].area, std::string());
  CHECK_EQ(c.partner.bosses[1].deaths, 2u);
  CHECK_EQ(c.partner.bosses[1].ttkMin, 25);
  CHECK_EQ(c.partner.bosses[0].ttkMin, -1);
  CHECK_EQ(c.partner.life.area, std::string(kCoopOtherAreas));
  CHECK_EQ(c.partner.firstDeath.area, std::string("Firelink Shrine"));
  CHECK_EQ(c.partner.firstDeath.session, 1);
  // Both played the same two sessions (the sample's times): together 12 minutes.
  CHECK_EQ(c.together.size(), size_t(2));
  CHECK_EQ(c.togetherMinutes, 12u);
  // The JSON carries no name the rule leaves out.
  std::string j = CoopPairJson(c);
  CHECK(j.find("\"together\":{\"minutes\":12,") != std::string::npos);
  CHECK(j.find("Claymore") == std::string::npos);
  CHECK(j.find("Grand Archives") == std::string::npos);
  CHECK(j.find("{\"name\":\"\",\"upgrade\":5}") != std::string::npos);
  // Not in the game: the partner alone, and nothing of theirs named.
  CoopPair n = BuildCoopPair(SharedGameState(), false, partner, true, coop_test::Lists(), coop_test::Areas(), {}, names);
  CHECK(!n.me.shown && n.partner.shown && n.together.empty());
  CHECK_EQ(n.partner.bosses[0].name, std::string());
  CHECK(n.partner.sessions[0].areas == std::vector<std::string>({kCoopOtherAreas}));
  // At Full, everything of theirs is named.
  CoopPair f = BuildCoopPair(mine, true, partner, true, coop_test::Lists(), coop_test::Areas(), reached, names, kTierFull);
  CHECK_EQ(f.partner.sessions[0].weapons[0].name, std::string("Claymore"));
  CHECK_EQ(f.partner.bosses[1].name, std::string("Vordt of the Boreal Valley"));
  CHECK(f.partner.sessions[0].areas == std::vector<std::string>({"Grand Archives", "High Wall of Lothric"}));
  CHECK_EQ(f.partner.life.area, std::string("Grand Archives"));
  // A partner with another boss list (another WASD version): no names, even at Full.
  SharedGameState other = partner;
  other.bossesDefeated = coop_test::Flags({1, 2, 3}, {true, true, false});
  CoopPair o = BuildCoopPair(mine, true, other, true, coop_test::Lists(), coop_test::Areas(), reached, names, kTierFull);
  CHECK_EQ(o.partner.bosses[1].name, std::string());
}
