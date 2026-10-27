// The live page's co-op session (src/ds3/coop_session.cpp): what the page
// is told from the relay's status, when a session ends and why, the codes
// it accepts, the partner's path, and /api/state's "coop" object at the
// spoiler tier. The session thread itself runs against the real relay in
// tests/relay/session.test.mjs.

namespace coop_session_test {
RelayStatus Relay(RelayState state, bool created = true) {
  RelayStatus r;
  r.state = state;
  r.created = created;
  r.code = "K7DP4X";
  return r;
}
CoopRelayInfo Relayed() {
  CoopRelayInfo r;
  r.configured = true;
  return r;
}
}  // namespace coop_session_test

TEST(coop_session_states_from_the_relay) {
  using coop_session_test::Relay;
  const ULONGLONG start = 1000000, now = start + 42500;
  CHECK_EQ(DeriveCoopSession(Relay(RelayState::kCreating), false, start, 0, 0, now).state, std::string("creating"));
  CoopSessionView v = DeriveCoopSession(Relay(RelayState::kWaiting), false, start, 0, 0, now);
  CHECK_EQ(v.state, std::string("hosting"));
  CHECK_EQ(v.code, std::string("K7DP4X"));
  CHECK_EQ(v.openSeconds, int64_t{42});
  CHECK_EQ(v.lastChangeMs, int64_t{-1});
  CHECK_EQ(DeriveCoopSession(Relay(RelayState::kConnecting, false), false, start, 0, 0, now).state,
           std::string("joining"));
  // The joiner in the room before the host: still joining.
  CHECK_EQ(DeriveCoopSession(Relay(RelayState::kWaiting, false), false, start, 0, 0, now).state,
           std::string("joining"));

  RelayStatus paired = Relay(RelayState::kPaired);
  paired.rttMs = 38;
  v = DeriveCoopSession(paired, true, start, 0, now - 400, now);
  CHECK_EQ(v.state, std::string("connected"));
  CHECK_EQ(v.rttMs, 38);
  CHECK_EQ(v.lastChangeMs, int64_t{400});
  // A packet applied after `now` was read is new, not from the future.
  CHECK_EQ(DeriveCoopSession(paired, true, start, 0, now + 15, now).lastChangeMs, int64_t{0});

  // After being paired, either side dropping reads as reconnecting, counting up.
  for (RelayState s : {RelayState::kWaiting, RelayState::kReconnecting}) {
    RelayStatus r = Relay(s);
    r.rttMs = 38;
    v = DeriveCoopSession(r, true, start, now - 12300, now - 12400, now);
    CHECK_EQ(v.state, std::string("reconnecting"));
    CHECK_EQ(v.goneSeconds, int64_t{12});
    CHECK_EQ(v.rttMs, -1);
    CHECK_EQ(v.error, std::string(""));
  }
}

TEST(coop_session_relay_unreachable_while_joining) {
  RelayStatus r = coop_session_test::Relay(RelayState::kReconnecting, false);
  r.problem = RelayProblem::kUnreachable;
  CoopSessionView v = DeriveCoopSession(r, false, 1, 0, 0, 2);
  CHECK_EQ(v.state, std::string("joining"));
  CHECK_EQ(v.error, std::string("relay"));
  // Once in, a dropped connection is only reconnecting, not an error.
  r.everIn = true;
  CHECK_EQ(DeriveCoopSession(r, false, 1, 0, 0, 2).error, std::string(""));
}

TEST(coop_session_ends_and_says_why) {
  using coop_session_test::Relay;
  auto endFor = [](RelayState s, RelayProblem p, bool everIn) {
    RelayStatus r = Relay(s, false);
    r.problem = p;
    r.everIn = everIn;
    r.message = "why";
    return CoopSessionEndFor(r, false, 0, 0, 1000);
  };
  CHECK(!CoopSessionEndFor(Relay(RelayState::kPaired), false, 0, 0, 1000).over);
  CHECK_EQ(endFor(RelayState::kFailed, RelayProblem::kBadCode, false).error, std::string("wrong-code"));
  CHECK_EQ(endFor(RelayState::kEnded, RelayProblem::kNoSession, false).error, std::string("wrong-code"));
  CHECK_EQ(endFor(RelayState::kEnded, RelayProblem::kNoSession, true).error, std::string("ended"));
  CHECK_EQ(endFor(RelayState::kFailed, RelayProblem::kFull, false).error, std::string("room-full"));
  CHECK_EQ(endFor(RelayState::kFailed, RelayProblem::kUnreachable, false).error, std::string("relay"));
  CHECK_EQ(endFor(RelayState::kFailed, RelayProblem::kOther, false).text, std::string("why"));

  CoopSessionEnd e = CoopSessionEndFor(Relay(RelayState::kPaired), true, 2, 0, 1000);
  CHECK(e.over);
  CHECK_EQ(e.error, std::string("version"));
  CHECK_EQ(e.text, std::string("your partner's WASD speaks protocol 2; yours speaks 1"));

  // The partner away for 5 minutes ends it; a moment less doesn't.
  const ULONGLONG gone = 5000;
  CHECK(!CoopSessionEndFor(Relay(RelayState::kWaiting), false, 0, gone, gone + kCoopGoneLimitMs - 1).over);
  e = CoopSessionEndFor(Relay(RelayState::kWaiting), false, 0, gone, gone + kCoopGoneLimitMs);
  CHECK(e.over);
  CHECK_EQ(e.error, std::string("ended"));
}

TEST(coop_session_pairing_rooms) {
  RelayStatus r = coop_session_test::Relay(RelayState::kWaiting, false);
  r.paired = true, r.code = "";
  CoopSessionView v = DeriveCoopSession(r, false, 1, 0, 0, 2);
  CHECK_EQ(v.state, std::string("pairing"));
  CHECK(v.paired);
  CHECK_EQ(v.code, std::string(""));
  CHECK_EQ(DeriveCoopSession(r, true, 1, 1, 0, 2).state, std::string("reconnecting"));
  // The partner away for 5 minutes doesn't end a pairing room: it waits.
  CHECK(!CoopSessionEndFor(r, false, 0, 5000, 5000 + kCoopGoneLimitMs * 3).over);
  r.state = RelayState::kFailed, r.problem = RelayProblem::kFull;
  CHECK_EQ(CoopSessionEndFor(r, false, 0, 0, 1).error, std::string("room-full"));
}

TEST(coop_session_seamless_password) {
  const std::string ini =
      "[GAMEPLAY]\r\n; comment\r\nallow_invaders = 1\r\ncooppassword = not this one\r\n\r\n"
      "[PASSWORD]\r\n\r\n; Session password\r\ncooppassword = Ash en 42  \r\n\r\n[SAVE]\r\nsave_file_extension = co2\r\n";
  CHECK_EQ(SeamlessPasswordFromIni(ini), std::string("Ash en 42"));
  CHECK_EQ(SeamlessPasswordFromIni("[password]\nCoopPassword=x;y\n"), std::string("x;y"));
  CHECK_EQ(SeamlessPasswordFromIni("[PASSWORD]\ncooppassword =\n"), std::string(""));
  CHECK_EQ(SeamlessPasswordFromIni("[PASSWORD]\n; cooppassword = x\n"), std::string(""));
  CHECK_EQ(SeamlessPasswordFromIni(""), std::string(""));
  CHECK_EQ(ReadSeamlessPassword(""), std::string(""));
  CHECK_EQ(ReadSeamlessPassword("Z:\\no\\such\\Game"), std::string(""));

  // The pairing key: a SHA-256, the same for the same password.
  CHECK_EQ(CoopPairKey("hunter2"), std::string("9be78da59ed8471fd932e87f2b4adc1e52f27e3bd1c6dd5ef7e030827b0610fb"));
  CHECK(RelayIsPairKey(CoopPairKey("x")));
  CHECK(CoopPairKey("x") != CoopPairKey("X"));
  CHECK_EQ(CoopPairKey(""), std::string(""));
}

TEST(coop_session_codes_as_typed) {
  CHECK_EQ(CoopNormalizeCode("k7dp4x"), std::string("K7DP4X"));
  CHECK_EQ(CoopNormalizeCode(" K7D-P4X "), std::string("K7DP4X"));
  CHECK_EQ(CoopNormalizeCode("K7DP4"), std::string(""));
  CHECK_EQ(CoopNormalizeCode("K7DP4XY"), std::string(""));
  CHECK_EQ(CoopNormalizeCode(""), std::string(""));
}

TEST(coop_session_partner_path_keeps_30_seconds) {
  std::vector<CoopTrailPoint> trail;
  SharedGameState s;
  s.havePose = true, s.mapId = 0x1E000000;
  CoopTrailAdd(&trail, s, 1000);
  s.x = 0.3f;  // under half a metre: not a new point
  CoopTrailAdd(&trail, s, 1100);
  CHECK_EQ(trail.size(), size_t{1});
  s.x = 1.0f;
  CoopTrailAdd(&trail, s, 1200);
  CHECK_EQ(trail.size(), size_t{2});
  s.mapId = 0x28000000;  // another map section: always a point, even standing still
  CoopTrailAdd(&trail, s, 1300);
  CHECK_EQ(trail.size(), size_t{3});
  CHECK_EQ(trail.back().mapKey, 80);
  CoopTrailAdd(&trail, SharedGameState(), 1200 + kCoopTrailMs + 1);  // no pose: only ages it
  CHECK_EQ(trail.size(), size_t{1});
  CHECK_NEAR(trail[0].x, 1.0f, 1e-6);
  for (int k = 0; k < 400; ++k) {
    s.x = static_cast<float>(k);
    CoopTrailAdd(&trail, s, 40000 + static_cast<ULONGLONG>(k));
  }
  CHECK_EQ(trail.size(), kCoopTrailMax);
  CHECK_NEAR(trail.back().x, 399.0f, 1e-6);
}

TEST(coop_session_live_json_without_a_partner) {
  CoopSessionView v;
  v.state = "hosting", v.code = "K7DP4X", v.openSeconds = 42;
  std::string j = CoopLiveJson(v, coop_session_test::Relayed(), CoopLivePartner(), SharedGameState(), false, CoopLists(), {}, {}, "", "", "",
                               "", "", 0, kTierOff);
  CHECK(j.find("\"state\":\"hosting\"") != std::string::npos);
  CHECK(j.find("\"code\":\"K7DP4X\"") != std::string::npos);
  CHECK(j.find("\"openSeconds\":42") != std::string::npos);
  CHECK(j.find("\"protocol\":1") != std::string::npos);
  CHECK(j.find("\"relay\":true,\"host\":{\"on\":false,\"running\":false,\"port\":0,\"error\":\"\",\"tailscale\":null},\"relayUrl\":\"\","
               "\"pair\":{\"available\":false,\"paused\":false}") != std::string::npos);
  CHECK(j.find("\"paired\":false") != std::string::npos);
  CoopRelayInfo hosting;
  hosting.configured = hosting.hosting = hosting.hostRunning = hosting.pairAvailable = true;
  hosting.port = 8790;
  std::string h = CoopLiveJson(v, hosting, CoopLivePartner(), SharedGameState(), false, CoopLists(), {}, {}, "", "", "",
                               "", "", 0, kTierOff);
  CHECK(h.find("\"host\":{\"on\":true,\"running\":true,\"port\":8790,") != std::string::npos);
  CHECK(h.find("\"pair\":{\"available\":true,") != std::string::npos);
  CHECK(j.size() >= 15 && j.substr(j.size() - 15) == "\"partner\":null}");

  CoopLivePartner p;
  p.shown = p.here = true;  // connected, but their state hasn't come yet
  j = CoopLiveJson(v, coop_session_test::Relayed(), p, SharedGameState(), false, CoopLists(), {}, {}, "", "", "", "", "", 0, kTierOff);
  CHECK(j.find("\"partner\":{\"here\":true,\"have\":false}}") != std::string::npos);
}

TEST(coop_session_live_json_partner_at_the_tier) {
  using coop_test::Flags;
  SharedGameState mine = coop_test::Sample(), partner = coop_test::Sample();
  partner.characterName = "Isolde";
  partner.deaths = 11;
  partner.x = -110.5f;
  partner.bossesDefeated = Flags({14000800, 13000800, 13300850}, {true, false, true});
  partner.pickupsFound = Flags({54000050, 54000060, 53000010}, {true, true, false});
  CoopLists lists;
  lists.bossFlags = {14000800, 13000800, 13300850};
  lists.bossAreas = {"Firelink Shrine", "High Wall of Lothric", "Grand Archives"};
  lists.bossNames = {"Iudex Gundyr", "Vordt of the Boreal Valley", "Lothric"};
  lists.pickupAreas = {"Firelink Shrine", "Firelink Shrine", "High Wall of Lothric"};
  CoopLivePartner p;
  p.shown = p.here = p.have = true;
  p.state = partner;
  p.trail = {{-112.0f, 33.0f, CoopMapKey(partner.mapId, partner.playRegionId), 1}, {-1.0f, 1.0f, 99, 2}};
  CoopSessionView v;
  v.state = "connected";
  auto json = [&](const std::string& area, int tier) {
    return CoopLiveJson(v, coop_session_test::Relayed(), p, mine, true, lists, lists.bossFlags, coop_test::Areas(), "Firelink Shrine", area,
                        area, "", "High Wall of Lothric", 1791800000, tier);
  };
  // Same map: position and the path on this map only; their items in your area.
  std::string j = json("Firelink Shrine", kTierOff);
  CHECK(j.find("\"name\":\"Isolde\"") != std::string::npos);
  CHECK(j.find("\"deaths\":11") != std::string::npos);
  CHECK(j.find("\"hp\":[812,900]") != std::string::npos);
  CHECK(j.find("\"area\":\"Firelink Shrine\"") != std::string::npos);  // you've been there: named at Off
  CHECK(j.find("\"sameMap\":true") != std::string::npos);
  CHECK(j.find("\"pose\":{\"x\":-110.5") != std::string::npos);
  CHECK(j.find("\"trail\":[[-112.0,33.0]]") != std::string::npos);
  CHECK(j.find("\"itemsHere\":{\"area\":\"Firelink Shrine\",\"found\":2,\"total\":2,\"mine\":1}") != std::string::npos);
  CHECK(j.find("\"bosses\":[14000800,13300850]") != std::string::npos);

  // An area you haven't reached: unnamed below Full, "beyond" at Category.
  j = json("Grand Archives", kTierCategory);
  CHECK(j.find("\"area\":\"\",\"region\":\"\",\"bonfire\":\"\",\"elsewhere\":true,\"beyond\":\"High Wall of Lothric\"") !=
        std::string::npos);
  j = json("Grand Archives", kTierFull);
  CHECK(j.find("\"area\":\"Grand Archives\"") != std::string::npos);

  // Another map section: no position; another boss list: no boss flags.
  p.state.mapId = 0x1E000000;
  lists.bossFlags = {1, 2, 3};
  j = CoopLiveJson(v, coop_session_test::Relayed(), p, mine, true, lists, lists.bossFlags, coop_test::Areas(), "Firelink Shrine", "", "", "",
                   "", 1791800000, kTierOff);
  CHECK(j.find("\"sameMap\":false,\"pose\":null,\"trail\":[]") != std::string::npos);
  CHECK(j.find("\"bosses\":[]") != std::string::npos);
}
