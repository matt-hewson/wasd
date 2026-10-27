// The co-op relay client's pure parts (src/core/relay_client.cpp). The
// client itself is tested against the relay WASD hosts by tests/relay/client.test.mjs.

TEST(relay_endpoint_from_the_url) {
  RelayEndpoint e = ParseRelayEndpoint("http://localhost:8787");
  CHECK(e.ok && !e.secure);
  CHECK_EQ(e.host, std::wstring(L"localhost"));
  CHECK_EQ(static_cast<int>(e.port), 8787);
  CHECK_EQ(e.basePath, std::wstring(L""));

  e = ParseRelayEndpoint("https://wasd-relay.example.workers.dev");
  CHECK(e.ok && e.secure);
  CHECK_EQ(static_cast<int>(e.port), 443);  // the scheme's default

  e = ParseRelayEndpoint("http://example.com/wasd/");
  CHECK(e.ok);
  CHECK_EQ(static_cast<int>(e.port), 80);
  CHECK_EQ(e.basePath, std::wstring(L"/wasd"));

  e = ParseRelayEndpoint("http://[::1]:8787/x");
  CHECK(e.ok);
  CHECK_EQ(e.host, std::wstring(L"::1"));
  CHECK_EQ(static_cast<int>(e.port), 8787);

  for (const char* bad : {"", "localhost:8787", "ws://localhost", "http://", "http://:80", "http://host:",
                          "http://host:0", "http://host:70000", "http://host:8x", "http://[::1"})
    CHECK(!ParseRelayEndpoint(bad).ok);
}

TEST(relay_backoff_doubles_to_thirty_seconds) {
  CHECK_EQ(RelayBackoffMs(0), 1000);
  CHECK_EQ(RelayBackoffMs(1), 2000);
  CHECK_EQ(RelayBackoffMs(4), 16000);
  CHECK_EQ(RelayBackoffMs(5), 30000);
  CHECK_EQ(RelayBackoffMs(500), 30000);
  CHECK_EQ(RelayBackoffMs(-1), 1000);
}

TEST(relay_join_answers) {
  CHECK(RelayJoinFromStatus(101) == RelayJoin::kOk);
  CHECK(RelayJoinFromStatus(400) == RelayJoin::kBadCode);
  CHECK(RelayJoinFromStatus(404) == RelayJoin::kNoSession);
  CHECK(RelayJoinFromStatus(409) == RelayJoin::kFull);
  CHECK(RelayJoinFromStatus(0) == RelayJoin::kFailed);    // couldn't reach it
  CHECK(RelayJoinFromStatus(503) == RelayJoin::kFailed);
}

TEST(relay_code_from_the_create_answer) {
  CHECK_EQ(RelayCodeFromJson("{\"code\":\"K7DP4X\"}"), std::string("K7DP4X"));
  CHECK_EQ(RelayCodeFromJson("{ \"code\" : \"K7DP4X\" }"), std::string("K7DP4X"));
  CHECK_EQ(RelayCodeFromJson("{\"error\":\"no free code\"}"), std::string(""));
  CHECK_EQ(RelayCodeFromJson("{\"code\":\"K7D\"}"), std::string(""));
  CHECK_EQ(RelayCodeFromJson("{\"code\":\"K7D\\\"4X\"}"), std::string(""));
  CHECK_EQ(RelayCodeFromJson(""), std::string(""));
}

TEST(relay_client_refuses_a_bad_url_and_sends_nothing_unconnected) {
  RelayClient c;
  CHECK(!c.Start("localhost:8787", "K7DP4X"));
  CHECK(c.Status().state == RelayState::kFailed);
  CHECK(!c.Send({1, 2, 3}));
  auto events = c.TakeEvents();
  CHECK_EQ(events.size(), size_t(1));
  CHECK(events[0].type == RelayEvent::kFailed);
  CHECK_EQ(std::string(RelayStateName(RelayState::kPaired)), std::string("partner connected"));
}
