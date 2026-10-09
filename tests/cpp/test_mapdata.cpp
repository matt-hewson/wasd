// Map data with the bundled Python: reading Steam's
// library list, finding the DS3 Game folder, the extractor's command line,
// and when to re-read. The real extractor run is in tests/pkg/test_extract.py.

namespace mapdata_test {
const char* kNewFormat = R"VDF("libraryfolders"
{
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"label"		""
		"contentid"		"123"
		"apps"
		{
			"228980"		"1234"
		}
	}
	"1"
	{
		"path"		"G:\\Steam\\SteamLibrary"
		"apps"
		{
			"374320"		"25000000000"
			"1245620"		"1"
		}
	}
}
)VDF";

const char* kOldFormat = R"VDF("LibraryFolders"
{
	// written by Steam before 2021
	"TimeNextStatsReport"		"1600000000"
	"ContentStatsID"		"-123"
	"1"		"D:\\SteamLibrary"
	"2"		"F:\\Games \"quoted\"\\Lib"
}
)VDF";

const std::wstring kGame = L"\\steamapps\\common\\DARK SOULS III\\Game";

// Fake file access. Windows paths ignore case (Steam's registry value is
// lower case, "c:/program files (x86)/steam"), so the fakes do too.
GameDirSearch Search(std::vector<std::wstring> existing, std::map<std::wstring, std::string> files = {}) {
  GameDirSearch s;
  s.exists = [existing](const std::wstring& f) {
    return std::any_of(existing.begin(), existing.end(), [&](const std::wstring& e) { return _wcsicmp(e.c_str(), f.c_str()) == 0; });
  };
  s.readFile = [files](const std::wstring& f) {
    for (const auto& kv : files)
      if (_wcsicmp(kv.first.c_str(), f.c_str()) == 0) return kv.second;
    return std::string();
  };
  return s;
}
}  // namespace mapdata_test

TEST(mapdata_vdf_current_format) {
  auto libs = ParseLibraryFolders(mapdata_test::kNewFormat);
  CHECK_EQ(libs.size(), size_t(2));
  if (libs.size() != 2) return;
  CHECK_EQ(libs[0].path, std::wstring(L"C:\\Program Files (x86)\\Steam"));
  CHECK_EQ(libs[1].path, std::wstring(L"G:\\Steam\\SteamLibrary"));
  CHECK_EQ(libs[0].apps.size(), size_t(1));
  CHECK(std::find(libs[1].apps.begin(), libs[1].apps.end(), "374320") != libs[1].apps.end());
}

TEST(mapdata_vdf_old_format) {
  auto libs = ParseLibraryFolders(mapdata_test::kOldFormat);  // also: a // comment, an escaped quote
  CHECK_EQ(libs.size(), size_t(2));
  if (libs.size() != 2) return;
  CHECK_EQ(libs[0].path, std::wstring(L"D:\\SteamLibrary"));
  CHECK_EQ(libs[1].path, std::wstring(L"F:\\Games \"quoted\"\\Lib"));
  CHECK(libs[0].apps.empty());
}

TEST(mapdata_vdf_broken_or_missing) {
  CHECK(ParseLibraryFolders("").empty());
  CHECK(ParseLibraryFolders("\"libraryfolders\" { \"0\" { \"path\" \"C:\\\\x").empty());  // cut off
  CHECK(ParseLibraryFolders("\"libraryfolders\" { \"0\" { \"path\" \"C:\\\\x\" }").empty());  // missing }
  CHECK(ParseLibraryFolders("\"something\" { }").empty());
  CHECK(ParseLibraryFolders("garbage { { }").empty());
}

TEST(mapdata_game_dir_from_steam_library_with_the_app) {
  using namespace mapdata_test;
  auto s = Search({L"G:\\Steam\\SteamLibrary" + kGame + L"\\Data5.bhd"},
                  {{L"C:\\Program Files (x86)\\Steam\\steamapps\\libraryfolders.vdf", kNewFormat}});
  s.steamPath = L"c:/program files (x86)/steam";  // as the registry holds it
  auto f = FindDs3GameDir(s);
  CHECK_EQ(f.dir, L"G:\\Steam\\SteamLibrary" + kGame);
  CHECK_EQ(f.source, std::string("Steam"));
}

TEST(mapdata_game_dir_old_format_scans_every_library) {
  using namespace mapdata_test;
  auto s = Search({L"D:\\SteamLibrary" + kGame + L"\\Data5.bhd"},
                  {{L"C:\\Steam\\steamapps\\libraryfolders.vdf", kOldFormat}});
  s.steamPath = L"C:\\Steam\\";
  CHECK_EQ(FindDs3GameDir(s).dir, L"D:\\SteamLibrary" + kGame);
  // The Steam folder itself counts even when the file doesn't list it.
  auto inRoot = Search({L"C:\\Steam" + kGame + L"\\Data5.bhd"});
  inRoot.steamPath = L"C:\\Steam";
  CHECK_EQ(FindDs3GameDir(inRoot).dir, L"C:\\Steam" + kGame);
}

TEST(mapdata_game_dir_order) {
  using namespace mapdata_test;
  const std::wstring running = L"G:\\DS3\\Game", saved = L"H:\\Saved\\Game", steamLib = L"E:\\Lib";
  auto s = Search({running + L"\\Data5.bhd", saved + L"\\Data5.bhd", steamLib + kGame + L"\\Data5.bhd"},
                  {{L"C:\\Steam\\steamapps\\libraryfolders.vdf", "\"libraryfolders\" { \"1\" { \"path\" \"E:\\\\Lib\" } }"}});
  s.steamPath = L"C:\\Steam";
  s.runningGameExe = running + L"\\DarkSoulsIII.exe";
  s.savedDir = saved;
  CHECK_EQ(FindDs3GameDir(s).source, std::string("the running game"));
  s.runningGameExe.clear();
  CHECK_EQ(FindDs3GameDir(s).dir, saved);
  s.savedDir = L"Z:\\gone";  // saved folder no longer valid: fall through to Steam
  CHECK_EQ(FindDs3GameDir(s).dir, steamLib + kGame);
}

TEST(mapdata_explicit_game_dir_has_no_fallback) {
  using namespace mapdata_test;
  auto s = Search({L"C:\\Steam" + kGame + L"\\Data5.bhd", L"D:\\Mine\\Data5.bhd"});
  s.steamPath = L"C:\\Steam";
  s.explicitDir = L"D:\\Mine\\";
  CHECK_EQ(FindDs3GameDir(s).dir, std::wstring(L"D:\\Mine"));
  s.explicitDir = L"D:\\Typo";
  auto f = FindDs3GameDir(s);
  CHECK(f.dir.empty());  // not silently replaced by the Steam copy
  CHECK_EQ(f.tried.size(), size_t(1));
}

TEST(mapdata_game_dir_not_found_lists_what_was_tried) {
  using namespace mapdata_test;
  auto s = Search({});
  s.savedDir = L"H:\\Saved";
  s.steamPath = L"C:\\Steam";
  auto f = FindDs3GameDir(s);
  CHECK(f.dir.empty());
  CHECK_EQ(f.tried.size(), size_t(2));
  CHECK(FindDs3GameDir(Search({})).tried.empty());  // no Steam, nothing saved
}

// What CommandLineToArgvW (the C runtime's rules) splits a command line into.
std::vector<std::wstring> SplitCommandLine(const std::wstring& cmd) {
  int n = 0;
  LPWSTR* argv = CommandLineToArgvW(cmd.c_str(), &n);
  std::vector<std::wstring> out(argv, argv + n);
  LocalFree(argv);
  return out;
}

TEST(mapdata_quote_arg_round_trips) {
  for (std::wstring a : {std::wstring(L"plain"), std::wstring(L"C:\\Program Files\\WASD\\python\\python.exe"),
                         std::wstring(L"C:\\My Dir\\"), std::wstring(L"say \"hi\""), std::wstring(L"a\\\\\"b"),
                         std::wstring(L"tab\there"), std::wstring(L"C:\\t\u00e9st\\Game"), std::wstring(L"\\\\server\\share\\")}) {
    auto back = SplitCommandLine(L"x.exe " + QuoteArg(a));
    CHECK_EQ(back.size(), size_t(2));
    if (back.size() == 2) CHECK_EQ(back[1], a);
  }
  CHECK_EQ(QuoteArg(L"plain"), std::wstring(L"plain"));
  CHECK_EQ(QuoteArg(L""), std::wstring(L"\"\""));
}

TEST(mapdata_extractor_command_line) {
  std::wstring cmd = ExtractorCommandLine(L"C:\\Users\\A B\\AppData\\Local\\Programs\\WASD\\python\\python.exe",
                                          L"C:\\Users\\A B\\AppData\\Local\\Programs\\WASD\\tools\\extract_treasures.py",
                                          L"E:\\Steam Library\\steamapps\\common\\DARK SOULS III\\Game",
                                          L"C:\\Users\\A B\\AppData\\Local\\WASD\\generated");
  auto argv = SplitCommandLine(cmd);
  CHECK_EQ(argv.size(), size_t(8));
  if (argv.size() != 8) return;
  CHECK_EQ(argv[1], std::wstring(L"-I"));
  CHECK_EQ(argv[2], std::wstring(L"-B"));  // no __pycache__ in the program folder
  CHECK_EQ(argv[3], std::wstring(L"-u"));  // progress reaches the log line by line
  CHECK_EQ(argv[5], std::wstring(L"E:\\Steam Library\\steamapps\\common\\DARK SOULS III\\Game"));
  CHECK_EQ(argv[6], std::wstring(L"--out"));
  CHECK_EQ(argv[7], std::wstring(L"C:\\Users\\A B\\AppData\\Local\\WASD\\generated"));
}

TEST(mapdata_when_to_reread) {
  CHECK(MapDataStale(false, 0, 0));          // never read
  CHECK(MapDataStale(true, 100, 200));       // the game's archives are newer: patched
  CHECK(!MapDataStale(true, 200, 100));      // up to date
  CHECK(!MapDataStale(true, 200, 200));
  CHECK(!MapDataStale(true, 200, 0));        // game file time unknown: keep what we have
}

// Every generated table must be there; the oldest counts.
TEST(mapdata_generated_tables_all_or_reread) {
  std::unordered_map<std::wstring, uint64_t> files = {{L"a", 300}, {L"b", 100}, {L"c", 200}};
  auto exists = [&](const std::wstring& f) { return files.count(f) > 0; };
  auto time = [&](const std::wstring& f) { return files.at(f); };
  FileSetTimes t = TimesOf({L"a", L"b", L"c"}, exists, time);
  CHECK(t.all && t.any);
  CHECK_EQ(t.oldest, uint64_t(100));
  CHECK_EQ(t.newest, uint64_t(300));
  CHECK(!MapDataStale(t.all, t.oldest, 100));  // the game files aren't newer than the oldest table
  CHECK(MapDataStale(t.all, t.oldest, 150));   // one table is older than the game files: read again
  FileSetTimes some = TimesOf({L"a", L"missing"}, exists, time);
  CHECK(!some.all && some.any);
  CHECK(MapDataStale(some.all, some.oldest, 0));  // a table missing (an older version's data): read again
  CHECK_EQ(std::string(MapDataReason(some)), std::string("incomplete (new tables to read)"));
  FileSetTimes none = TimesOf({L"x"}, exists, time);
  CHECK(!none.all && !none.any);
  CHECK_EQ(std::string(MapDataReason(none)), std::string("not there yet"));
  CHECK_EQ(std::string(MapDataReason(t)), std::string("older than the game's files"));
  // What the app checks: the six tables it reads, against Data5, Data1 and Data0.
  CHECK_EQ(sizeof(kGeneratedTables) / sizeof(kGeneratedTables[0]), size_t(6));
  CHECK_EQ(std::wstring(kGameSourceFiles[2]), std::wstring(L"Data0.bdt"));
}

TEST(mapdata_which_python) {
  AppPaths installed = ResolveAppPaths({L"C:\\P\\WASD\\bin\\wasd-cli.exe", L"", L"C:\\L", nullptr});
  auto none = [](const wchar_t*) { return std::wstring(L"C:\\Python314\\python.exe"); };
  CHECK_EQ(ExtractorPython(installed, [](const std::wstring&) { return true; }, none),
           std::wstring(L"C:\\P\\WASD\\python\\python.exe"));  // the bundled one, never the system's
  CHECK(ExtractorPython(installed, [](const std::wstring&) { return false; }, none).empty());
  AppPaths dev = installed;
  dev.devMode = true;
  CHECK_EQ(ExtractorPython(dev, [](const std::wstring&) { return false; }, none), std::wstring(L"C:\\Python314\\python.exe"));
  CHECK_EQ(installed.ExtractorScript(), std::wstring(L"C:\\P\\WASD\\tools\\extract_treasures.py"));
}

// Closing WASD mid-read must not leave the extractor running (it held the
// install's python\ files, so uninstall left them behind -- 2026-10-07).
TEST(mapdata_child_dies_with_the_job) {
  HANDLE job = CreateKillOnCloseJob();
  CHECK(job != nullptr);
  if (!job) return;
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  bool started = StartInJob(L"cmd.exe /c ping -n 30 127.0.0.1 >nul", job, &si, &pi);
  CHECK(started);
  if (!started) {
    CloseHandle(job);
    return;
  }
  CHECK(WaitForSingleObject(pi.hProcess, 300) == WAIT_TIMEOUT);   // running
  CloseHandle(job);                                               // what exiting WASD does
  CHECK(WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0);  // and it's gone
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
}
