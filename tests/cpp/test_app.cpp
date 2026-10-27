// The app window (WASD.exe): its text for each state
// (MainWindowModel), the single-instance identity, and the daily log files.
// The real window -- taskbar button, single instance, closing -- is tested
// on the built WASD.exe by tests/pkg/test_app.py.

namespace app_test {
WindowSnapshot Attached() {
  WindowSnapshot s;
  s.haveValues = true;
  s.characterName = "Ashen One";
  s.level = 10;
  s.currentArea = "Undead Settlement";
  s.sessionSeconds = 3725;
  return s;
}
}  // namespace app_test

TEST(app_window_title) {
  CHECK_EQ(MainWindowModel(WindowSnapshot{}, kMapReady, "").title, std::string("WASD 0.2.0"));
}

TEST(app_window_waiting_for_the_game) {
  auto t = MainWindowModel(WindowSnapshot{}, kMapReady, "");
  CHECK_EQ(t.status, std::string("Waiting for Dark Souls III with Seamless Co-op"));
  CHECK_EQ(t.details, std::string("Start the game through Seamless Co-op; WASD attaches by itself."));
  WindowSnapshot menu;
  menu.status = "Not in game (main menu or loading) -- waiting.";
  CHECK_EQ(MainWindowModel(menu, kMapReady, "").status, menu.status);  // the worker's own words
}

TEST(app_window_attached) {
  auto t = MainWindowModel(app_test::Attached(), kMapReady, "http://localhost:8765/");
  CHECK_EQ(t.status, std::string("Attached to Dark Souls III (Seamless Co-op)"));
  CHECK_EQ(t.details, std::string("Ashen One, level 10  \xC2\xB7  Undead Settlement  \xC2\xB7  session 1:02:05"));
  CHECK_EQ(t.livePage, std::string("Live page: http://localhost:8765/"));
  auto unnamed = app_test::Attached();
  unnamed.characterName.clear();
  unnamed.currentArea.clear();
  unnamed.level = 0;
  CHECK_EQ(MainWindowModel(unnamed, kMapReady, "").details, std::string("Your character  \xC2\xB7  session 1:02:05"));
}

TEST(app_window_paused) {
  auto s = app_test::Attached();
  s.paused = true;
  CHECK_EQ(MainWindowModel(s, kMapReady, "").status, std::string("Paused (F10): no memory reads until you press F10 again"));
}

TEST(app_window_map_data_states) {
  WindowSnapshot s;
  CHECK_EQ(MainWindowModel(s, kMapChecking, "").mapData, std::string("Map data: checking..."));
  auto reading = MainWindowModel(s, kMapReading, "");
  CHECK(reading.mapDataBusy);  // the button is disabled while it runs
  CHECK(reading.mapData.find("reading it from your DS3 install") != std::string::npos);
  CHECK(!MainWindowModel(s, kMapReady, "").mapDataBusy);
  CHECK_EQ(MainWindowModel(s, kMapReady, "").mapData, std::string("Map data: ready"));
  CHECK(MainWindowModel(s, kMapNoGame, "").mapData.find("Read map data") != std::string::npos);
  CHECK(MainWindowModel(s, kMapFailed, "").mapData.find("the log says why") != std::string::npos);
  CHECK_EQ(MainWindowModel(s, kMapReady, "").livePage, std::string("Live page: starting..."));
}

TEST(app_instance_identity) {
  CHECK_EQ(SanitizeInstance(L""), std::wstring(L""));
  CHECK_EQ(SanitizeInstance(L"test-123"), std::wstring(L"test-123"));
  CHECK_EQ(SanitizeInstance(L"a b\\c/d:e;f"), std::wstring(L"abcdef"));  // no path or name tricks
  CHECK_EQ(SanitizeInstance(std::wstring(100, L'x')).size(), size_t(40));
  CHECK_EQ(InstanceMutexName(L""), std::wstring(L"Local\\AmishGoose.WASD"));
  CHECK_EQ(InstanceMutexName(L"t1"), std::wstring(L"Local\\AmishGoose.WASD.t1"));
  CHECK_EQ(MainWindowClassName(L""), std::wstring(L"WasdMainWindow"));
  CHECK_EQ(MainWindowClassName(L"t1"), std::wstring(L"WasdMainWindow.t1"));
  CHECK_EQ(std::wstring(kAppUserModelId), std::wstring(L"AmishGoose.WASD"));
}

TEST(app_log_file_names) {
  CHECK_EQ(LogFileName(2026, 10, 7), std::wstring(L"wasd-20261007.log"));
  CHECK_EQ(LogFileName(2027, 1, 3), std::wstring(L"wasd-20270103.log"));
  CHECK_EQ(DaysFromCivil(1970, 1, 1), 0L);
  CHECK_EQ(DaysFromCivil(2000, 3, 1) - DaysFromCivil(2000, 2, 28), 2L);  // 2000 was a leap year
  CHECK_EQ(DaysFromCivil(2027, 1, 3) - DaysFromCivil(2026, 12, 26), 8L);
}

TEST(app_old_logs_deleted_after_seven_days) {
  std::vector<std::wstring> names = {L"wasd-20261007.log", L"wasd-20260930.log", L"wasd-20260929.log",
                                     L"wasd-20250101.log", L"notes.txt",          L"wasd-2026100.log",
                                     L"wasd-20261332.log", L"wasd-2026x007.log",  L"wasd-20260929.txt"};
  auto old = OldLogs(names, 2026, 10, 7, 7);
  std::sort(old.begin(), old.end());
  CHECK_EQ(old.size(), size_t(2));  // 8 days old and last year; 7 days is kept; non-logs untouched
  if (old.size() == 2) {
    CHECK_EQ(old[0], std::wstring(L"wasd-20250101.log"));
    CHECK_EQ(old[1], std::wstring(L"wasd-20260929.log"));
  }
  CHECK_EQ(OldLogs({L"wasd-20261226.log"}, 2027, 1, 3, 7).size(), size_t(1));  // across the new year
}

TEST(app_live_port_setting) {
  CHECK_EQ(ParseLivePort("8765"), 8765);
  CHECK_EQ(ParseLivePort("8865"), 8865);  // a second copy's own port
  CHECK_EQ(ParseLivePort("1024"), 1024);
  CHECK_EQ(ParseLivePort("65526"), 65526);  // the last ten tries still fit
  CHECK_EQ(ParseLivePort("65527"), -1);
  CHECK_EQ(ParseLivePort("1023"), -1);
  CHECK_EQ(ParseLivePort(""), -1);
  CHECK_EQ(ParseLivePort("-8765"), -1);
  CHECK_EQ(ParseLivePort("8765 "), -1);
  CHECK_EQ(ParseLivePort("99999999999"), -1);
}

TEST(app_relay_url_setting) {
  std::string url = "kept";
  CHECK(ParseRelayUrl("", &url));
  CHECK_EQ(url, std::string(""));  // no relay
  CHECK(ParseRelayUrl("http://localhost:8787", &url));
  CHECK_EQ(url, std::string("http://localhost:8787"));
  CHECK(ParseRelayUrl("https://relay.example.workers.dev/", &url));
  CHECK_EQ(url, std::string("https://relay.example.workers.dev"));  // trailing slash dropped
  CHECK(ParseRelayUrl("https://example.com/wasd//", &url));
  CHECK_EQ(url, std::string("https://example.com/wasd"));
  url = "kept";
  for (const char* bad : {"localhost:8787", "ftp://example.com", "http://", "https:///", "http:///path",
                          "http://exa mple.com", "ws://localhost:8787"}) {
    CHECK(!ParseRelayUrl(bad, &url));
    CHECK_EQ(url, std::string("kept"));  // a bad value changes nothing
  }
}

TEST(app_relay_address_as_the_partner_pastes_it) {
  const std::string want = "http://my-pc.tail1234.ts.net:8790";
  CHECK_EQ(NormalizeRelayInput("relay_url=http://my-pc.tail1234.ts.net:8790"), want);  // the host's copied line
  CHECK_EQ(NormalizeRelayInput("  RELAY_URL = http://my-pc.tail1234.ts.net:8790 \r\n"), want);
  CHECK_EQ(NormalizeRelayInput("http://my-pc.tail1234.ts.net:8790/"), want);
  CHECK_EQ(NormalizeRelayInput("my-pc.tail1234.ts.net"), want);  // just the name: http and the relay's port
  CHECK_EQ(NormalizeRelayInput("HTTP://my-pc.tail1234.ts.net"), want);
  CHECK_EQ(NormalizeRelayInput("100.101.102.103"), std::string("http://100.101.102.103:8790"));
  CHECK_EQ(NormalizeRelayInput("100.101.102.103:9000"), std::string("http://100.101.102.103:9000"));  // a port given is kept
  CHECK_EQ(NormalizeRelayInput("[fd7a:115c::1]"), std::string("http://[fd7a:115c::1]:8790"));
  CHECK_EQ(NormalizeRelayInput("https://relay.example.com"), std::string("https://relay.example.com"));  // https: its own port
  CHECK_EQ(NormalizeRelayInput("my-pc", 8791), std::string("http://my-pc:8791"));
  for (const char* bad : {"", "   ", "relay_url=", "ws://my-pc:8790", "ftp://my-pc", "my pc", "http://"})
    CHECK_EQ(NormalizeRelayInput(bad), std::string(""));
}

TEST(app_query_parameters_decoded) {
  CHECK_EQ(UrlDecode("relay_url%3Dhttp%3A%2F%2Fmy-pc%3A8790"), std::string("relay_url=http://my-pc:8790"));
  CHECK_EQ(UrlDecode("a+b%20c"), std::string("a b c"));
  CHECK_EQ(UrlDecode("100%"), std::string("100%"));  // not an escape: kept
  CHECK_EQ(UrlDecode("%zz"), std::string("%zz"));
}
