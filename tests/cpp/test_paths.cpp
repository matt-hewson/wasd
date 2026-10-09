// Program files vs. your data: where every file
// lives, from ResolveAppPaths (pure), plus a lint that keeps file
// locations inside the AppPaths section of main.cpp.

namespace paths_test {
std::function<bool(const std::wstring&)> Existing(std::vector<std::wstring> files) {
  return [files](const std::wstring& f) { return std::find(files.begin(), files.end(), f) != files.end(); };
}

AppPathsInput Input(std::wstring exe, std::vector<std::wstring> existing, std::wstring env = L"",
                    std::wstring localAppData = L"C:\\Users\\Ann\\AppData\\Local") {
  AppPathsInput in;
  in.exePath = std::move(exe);
  in.envUserDir = std::move(env);
  in.localAppData = std::move(localAppData);
  in.exists = Existing(std::move(existing));
  return in;
}

bool Under(const std::wstring& path, const std::wstring& root) {
  return path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == L'\\';
}
}  // namespace paths_test

TEST(paths_dev_checkout_keeps_everything_in_the_repo) {
  using namespace paths_test;
  auto p = ResolveAppPaths(Input(L"E:\\dev\\wasd\\build\\wasd-cli.exe",
                                 {L"E:\\dev\\wasd\\.git", L"E:\\dev\\wasd\\src\\main.cpp"}));
  CHECK(p.devMode);
  CHECK_EQ(p.assets, std::wstring(L"E:\\dev\\wasd"));
  CHECK_EQ(p.user, std::wstring(L"E:\\dev\\wasd"));  // sessions\ and settings.ini stay where they were
  CHECK_EQ(p.generated, std::wstring(L"E:\\dev\\wasd\\data\\generated"));  // where extract_treasures.py writes
  CHECK_EQ(p.userSource, std::wstring(L"dev checkout"));
}

TEST(paths_installed_copy_keeps_your_data_in_localappdata) {
  using namespace paths_test;
  auto p = ResolveAppPaths(Input(L"C:\\Users\\Ann\\AppData\\Local\\Programs\\WASD\\bin\\WASD.exe", {}));
  CHECK(!p.devMode);
  CHECK_EQ(p.assets, std::wstring(L"C:\\Users\\Ann\\AppData\\Local\\Programs\\WASD"));
  CHECK_EQ(p.user, std::wstring(L"C:\\Users\\Ann\\AppData\\Local\\WASD"));
  CHECK_EQ(p.generated, std::wstring(L"C:\\Users\\Ann\\AppData\\Local\\WASD\\generated"));
  CHECK_EQ(p.Data(L"bosses.tsv"), std::wstring(L"C:\\Users\\Ann\\AppData\\Local\\Programs\\WASD\\data\\bosses.tsv"));
  CHECK_EQ(p.Template(L"live.html"), std::wstring(L"C:\\Users\\Ann\\AppData\\Local\\Programs\\WASD\\templates\\live.html"));
  CHECK_EQ(p.Settings(), std::wstring(L"C:\\Users\\Ann\\AppData\\Local\\WASD\\settings.ini"));
  CHECK_EQ(p.Sessions(), std::wstring(L"C:\\Users\\Ann\\AppData\\Local\\WASD\\sessions"));
  CHECK_EQ(p.Reports(), std::wstring(L"C:\\Users\\Ann\\AppData\\Local\\WASD\\reports"));
  CHECK_EQ(p.Logs(), std::wstring(L"C:\\Users\\Ann\\AppData\\Local\\WASD\\logs"));
  CHECK_EQ(p.Scratch(L"candidates.txt"), std::wstring(L"C:\\Users\\Ann\\AppData\\Local\\WASD\\scratch\\candidates.txt"));
}

TEST(paths_user_files_never_land_in_the_program_folder) {
  using namespace paths_test;
  auto p = ResolveAppPaths(Input(L"C:\\Users\\Ann\\AppData\\Local\\Programs\\WASD\\bin\\WASD.exe", {}));
  for (const auto& f : {p.Settings(), p.Sessions(), p.Reports(), p.Logs(), p.Scratch(L"x"), p.Generated(L"t.tsv")}) {
    CHECK(Under(f, p.user));
    CHECK(!Under(f, p.assets));  // replaced on upgrade, removed on uninstall
  }
  for (const auto& f : {p.Data(L"x.tsv"), p.Template(L"x.html")}) CHECK(Under(f, p.assets));
}

TEST(paths_environment_override_wins) {
  using namespace paths_test;
  auto dev = ResolveAppPaths(Input(L"E:\\dev\\wasd\\build\\wasd-cli.exe",
                                   {L"E:\\dev\\wasd\\.git", L"E:\\dev\\wasd\\src\\main.cpp"}, L"D:\\tmp\\run 1\\"));
  CHECK_EQ(dev.user, std::wstring(L"D:\\tmp\\run 1"));  // trailing slash trimmed
  CHECK_EQ(dev.userSource, std::wstring(L"WASD_USER_DIR"));
  CHECK_EQ(dev.generated, std::wstring(L"E:\\dev\\wasd\\data\\generated"));  // map data stays with a dev checkout
  auto inst = ResolveAppPaths(Input(L"C:\\P\\WASD\\bin\\WASD.exe", {}, L"D:\\tmp\\t\u00e9st"));
  CHECK_EQ(inst.user, std::wstring(L"D:\\tmp\\t\u00e9st"));
  CHECK_EQ(inst.generated, std::wstring(L"D:\\tmp\\t\u00e9st\\generated"));
  CHECK_EQ(ResolveAppPaths(Input(L"C:\\P\\WASD\\bin\\WASD.exe", {}, L"D:\\")).user, std::wstring(L"D:\\"));
}

TEST(paths_dev_mode_needs_both_git_and_the_source) {
  using namespace paths_test;
  auto gitOnly = ResolveAppPaths(Input(L"E:\\x\\build\\wasd-cli.exe", {L"E:\\x\\.git"}));
  CHECK(!gitOnly.devMode);
  auto srcOnly = ResolveAppPaths(Input(L"E:\\x\\build\\wasd-cli.exe", {L"E:\\x\\src\\main.cpp"}));
  CHECK(!srcOnly.devMode);
  CHECK_EQ(gitOnly.user, std::wstring(L"C:\\Users\\Ann\\AppData\\Local\\WASD"));
}

TEST(paths_odd_locations) {
  using namespace paths_test;
  auto noLocal = ResolveAppPaths(Input(L"C:\\P\\WASD\\bin\\WASD.exe", {}, L"", L""));
  CHECK_EQ(noLocal.user, std::wstring(L"C:\\P\\WASD\\userdata"));
  CHECK_EQ(noLocal.userSource, std::wstring(L"no %LOCALAPPDATA%"));
  CHECK_EQ(ResolveAppPaths(Input(L"wasd-cli.exe", {})).assets, std::wstring(L"."));
  CHECK_EQ(ResolveAppPaths(Input(L"C:\\wasd-cli.exe", {})).assets, std::wstring(L"."));
  CHECK_EQ(ResolveAppPaths(Input(L"", {})).assets, std::wstring(L"."));
  auto spaced = ResolveAppPaths(Input(L"C:\\My Games\\W\u00c4SD tools\\bin\\WASD.exe", {}));
  CHECK_EQ(spaced.assets, std::wstring(L"C:\\My Games\\W\u00c4SD tools"));
  CHECK_EQ(ParentDir(L"C:\\a\\b"), std::wstring(L"C:\\a"));
  CHECK_EQ(ParentDir(L"name"), std::wstring(L"."));
}

TEST(paths_this_test_build_runs_as_a_dev_checkout) {
  CHECK(Paths().devMode);
  CHECK_EQ(Paths().user, Paths().assets);
  CHECK(GetFileAttributesW(Paths().Data(L"bosses.tsv").c_str()) != INVALID_FILE_ATTRIBUTES);
}

TEST(paths_ensure_dir_creates_nested_folders) {
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  std::wstring root = std::wstring(tmp) + L"wasd ensure t\u00e9st " + std::to_wstring(GetCurrentProcessId());
  std::wstring deep = root + L"\\a\\b c\\d";
  CHECK(EnsureDir(deep));
  CHECK((GetFileAttributesW(deep.c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0);
  CHECK(EnsureDir(deep));  // already there
  RemoveDirectoryW(deep.c_str());
  RemoveDirectoryW((root + L"\\a\\b c").c_str());
  RemoveDirectoryW((root + L"\\a").c_str());
  RemoveDirectoryW(root.c_str());
  CHECK(GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES);
}

TEST(paths_utf8_for_printing) {
  CHECK_EQ(Utf8(L"C:\\t\u00e9st"), std::string("C:\\t\xC3\xA9st"));
  CHECK_EQ(Utf8(L""), std::string(""));
}

// Lint: outside the [AppPaths begin] ... [AppPaths end] section, main.cpp
// may not spell out where files live -- it asks Paths(). Catches a new
// feature writing next to the exe (Program Files) or to the old repo paths.
TEST(paths_lint_file_locations_only_in_app_paths) {
  std::string src = ReadWholeFile(Paths().assets + L"\\src\\main.cpp");
  CHECK(!src.empty());
  size_t begin = src.find("// [AppPaths begin]"), end = src.find("// [AppPaths end]");
  CHECK(begin != std::string::npos && end != std::string::npos && begin < end);
  if (begin == std::string::npos || end == std::string::npos || begin >= end) return;
  std::string outside = src.substr(0, begin) + src.substr(end);
  const char* forbidden[] = {
      "RepoRootPath",          "GetModuleFileNameW(nullptr", "L\"\\\\sessions",  "L\"\\\\settings.ini",
      "L\"\\\\reports",        "L\"\\\\logs",                "L\"\\\\scratch",   "L\"\\\\data\\\\",
      "L\"\\\\templates\\\\",  "\"build\\\\",               "L\"data\\\\",      "L\"templates\\\\",
  };
  for (const char* f : forbidden) {
    size_t at = outside.find(f);
    if (at != std::string::npos) {
      size_t lineStart = outside.rfind('\n', at) + 1;
      CHECK_EQ(outside.substr(lineStart, outside.find('\n', at) - lineStart), std::string("(no ") + f + ")");
    }
  }
}
