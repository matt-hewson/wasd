// The command-line entry point: what runs with no command (a double-click),
// and when the window waits for Enter. The real-process side (a hidden
// console standing in for Explorer) is in tests/pkg/test_static.py.

TEST(entry_no_command_starts_the_overlay) {
  wchar_t exe[] = L"wasd-cli.exe";
  wchar_t* none[] = {exe, nullptr};
  CHECK_EQ(CommandOf(1, none), std::wstring(L"overlay"));  // double-click: was "probe" until 2026-10-07
  wchar_t empty[] = L"";
  wchar_t* blank[] = {exe, empty, nullptr};
  CHECK_EQ(CommandOf(2, blank), std::wstring(L"overlay"));
}

TEST(entry_named_commands_pass_through) {
  wchar_t exe[] = L"wasd-cli.exe";
  for (const wchar_t* cmd : {L"probe", L"report", L"stats", L"--version", L"overlay"}) {
    std::wstring c = cmd;
    wchar_t* argv[] = {exe, c.data(), nullptr};
    CHECK_EQ(CommandOf(2, argv), c);
  }
}

TEST(entry_pauses_only_for_a_failure_in_its_own_console) {
  CHECK(ShouldPauseOnExit(2, 1));    // double-clicked, game not found: keep the message on screen
  CHECK(ShouldPauseOnExit(64, 1));   // double-clicked, bad command
  CHECK(!ShouldPauseOnExit(0, 1));   // double-clicked, quit normally: just close
  CHECK(!ShouldPauseOnExit(2, 2));   // from a terminal: the terminal keeps the output
  CHECK(!ShouldPauseOnExit(64, 3));  // from a script: never wait
  CHECK(!ShouldPauseOnExit(2, 0));   // GetConsoleProcessList failed: don't wait either
}
