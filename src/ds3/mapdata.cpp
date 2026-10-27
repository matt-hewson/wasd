// src/ds3/mapdata.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- map data: finding the game, running the extractor
//
// treasures.tsv, enemies.tsv and the name tables (item, bonfire and place
// names) come from the user's own DS3 install, through
// tools\extract_treasures.py: with the system Python in a dev checkout, with
// the bundled embeddable Python (python\) in an install.
// `wasd-cli.exe extract [--game <folder>]` runs it; `overlay` runs it before
// starting when a table is missing or older than the game's files (the game
// was patched): kGeneratedTables, kGameSourceFiles.

// Steam's steamapps\libraryfolders.vdf, a KeyValues text file. Minimal
// reader: quoted strings (with \\ \" escapes), braces, // comments.
namespace vdf {
struct Node {
  bool object = false;
  std::string text;
  std::vector<std::pair<std::string, Node>> kids;
  const Node* Get(const std::string& key) const {
    for (const auto& kv : kids)
      if (_stricmp(kv.first.c_str(), key.c_str()) == 0) return &kv.second;
    return nullptr;
  }
};

struct Reader {
  const std::string& s;
  size_t i = 0;
  bool ok = true;
  // '{', '}', '"' (a string, in *tok) or 0 at the end.
  char Next(std::string* tok) {
    for (;;) {
      while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
      if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
        while (i < s.size() && s[i] != '\n') ++i;
        continue;
      }
      break;
    }
    if (i >= s.size()) return 0;
    char c = s[i++];
    if (c == '{' || c == '}') return c;
    tok->clear();
    if (c != '"') {  // unquoted token
      --i;
      while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '{' && s[i] != '}') *tok += s[i++];
      return '"';
    }
    while (i < s.size() && s[i] != '"') {
      if (s[i] == '\\' && i + 1 < s.size()) {
        char e = s[++i];
        *tok += e == 'n' ? '\n' : e == 't' ? '\t' : e;  // \\ and \" (and anything else) as the character
        ++i;
      } else {
        *tok += s[i++];
      }
    }
    if (i >= s.size()) ok = false;  // unterminated string
    ++i;
    return '"';
  }
  void ReadPairs(Node* into, bool braced) {
    into->object = true;
    for (;;) {
      std::string key, value;
      char t = Next(&key);
      if (t == 0) {
        if (braced) ok = false;
        return;
      }
      if (t == '}') {
        if (!braced) ok = false;
        return;
      }
      if (t != '"') { ok = false; return; }
      char u = Next(&value);
      if (u == '{') {
        Node child;
        ReadPairs(&child, true);
        into->kids.emplace_back(key, std::move(child));
        if (!ok) return;
      } else if (u == '"') {
        Node leaf;
        leaf.text = value;
        into->kids.emplace_back(key, std::move(leaf));
      } else {
        ok = false;
        return;
      }
    }
  }
};
}  // namespace vdf

struct SteamLibrary {
  std::wstring path;
  std::vector<std::string> apps;  // installed app ids; empty in the pre-2021 format
};

// Libraries listed in libraryfolders.vdf. Current format: "N" { "path" ...
// "apps" { "<id>" ... } }; pre-2021: "N" "<path>". Broken file: empty.
std::vector<SteamLibrary> ParseLibraryFolders(const std::string& text) {
  std::vector<SteamLibrary> out;
  vdf::Node root;
  vdf::Reader r{text};
  r.ReadPairs(&root, false);
  const vdf::Node* lf = root.Get("libraryfolders");
  if (!r.ok || !lf || !lf->object) return out;
  for (const auto& [key, node] : lf->kids) {
    if (key.empty() || key.find_first_not_of("0123456789") != std::string::npos) continue;
    SteamLibrary lib;
    if (node.object) {
      const vdf::Node* path = node.Get("path");
      if (!path || path->object) continue;
      lib.path = WidenUtf8(path->text);
      if (const vdf::Node* apps = node.Get("apps"))
        for (const auto& app : apps->kids) lib.apps.push_back(app.first);
    } else {
      lib.path = WidenUtf8(node.text);
    }
    if (!lib.path.empty()) out.push_back(std::move(lib));
  }
  return out;
}

// Backslashes, no trailing separator (Steam's registry value uses "c:/...").
std::wstring NormalizeDir(std::wstring d) {
  for (auto& c : d)
    if (c == L'/') c = L'\\';
  while (d.size() > 3 && d.back() == L'\\') d.pop_back();
  return d;
}

constexpr char kDs3SteamAppId[] = "374320";

struct GameDirSearch {
  std::wstring explicitDir;     // --game
  std::wstring runningGameExe;  // the running DS3's exe ("" when not running, or not Seamless Co-op)
  std::wstring savedDir;        // game_dir from settings.ini
  std::wstring steamPath;       // HKCU\Software\Valve\Steam\SteamPath ("" without Steam)
  std::function<bool(const std::wstring&)> exists;
  std::function<std::string(const std::wstring&)> readFile;
};
struct GameDirFound {
  std::wstring dir;  // "" = not found
  std::string source;
  std::vector<std::wstring> tried;  // every folder looked at, for the "not found" message
};

// Where the DS3 "Game" folder (the one holding Data5.bhd) is, in this
// order: --game (no fallback when given), the running game, the saved
// folder, then every Steam library (those listing app 374320 first).
GameDirFound FindDs3GameDir(const GameDirSearch& s) {
  GameDirFound f;
  auto check = [&](std::wstring dir, const char* source) {
    dir = NormalizeDir(dir);
    if (dir.empty()) return false;
    f.tried.push_back(dir);
    if (!s.exists(dir + L"\\Data5.bhd")) return false;
    f.dir = dir;
    f.source = source;
    return true;
  };
  if (!s.explicitDir.empty()) {
    check(s.explicitDir, "--game");
    return f;
  }
  if (!s.runningGameExe.empty() && check(ParentDir(s.runningGameExe), "the running game")) return f;
  if (!s.savedDir.empty() && check(s.savedDir, "settings.ini")) return f;
  if (s.steamPath.empty()) return f;
  std::wstring steam = NormalizeDir(s.steamPath);
  auto libs = ParseLibraryFolders(s.readFile(steam + L"\\steamapps\\libraryfolders.vdf"));
  if (std::none_of(libs.begin(), libs.end(), [&](const SteamLibrary& l) { return _wcsicmp(NormalizeDir(l.path).c_str(), steam.c_str()) == 0; }))
    libs.push_back({steam, {}});
  std::stable_partition(libs.begin(), libs.end(), [](const SteamLibrary& l) {
    return std::find(l.apps.begin(), l.apps.end(), kDs3SteamAppId) != l.apps.end();
  });
  for (const auto& lib : libs)
    if (check(lib.path + L"\\steamapps\\common\\DARK SOULS III\\Game", "Steam")) return f;
  return f;
}

// One argument for a Windows command line, quoted the way CommandLineToArgvW
// and the C runtime split it back (backslashes only double before a quote).
std::wstring QuoteArg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
  std::wstring out = L"\"";
  for (size_t i = 0;; ++i) {
    size_t slashes = 0;
    while (i < a.size() && a[i] == L'\\') {
      ++i;
      ++slashes;
    }
    if (i == a.size()) {
      out.append(slashes * 2, L'\\');
      break;
    }
    if (a[i] == L'"') {
      out.append(slashes * 2 + 1, L'\\');
    } else {
      out.append(slashes, L'\\');
    }
    out += a[i];
  }
  return out + L"\"";
}

std::wstring ExtractorCommandLine(const std::wstring& python, const std::wstring& script, const std::wstring& gameDir,
                                  const std::wstring& outDir) {
  // -I: ignore PYTHON* variables and user site-packages; -B: write no
  // __pycache__ beside the scripts (that would be the program folder).
  // -u: unbuffered, so each progress line reaches the log as it's printed.
  return QuoteArg(python) + L" -I -B -u " + QuoteArg(script) + L" " + QuoteArg(gameDir) + L" --out " + QuoteArg(outDir);
}

// Re-read the map data? Yes when it's missing, or older than the game's
// files it's read from (a game patch rewrites them). Times: FILETIME as
// 64-bit ticks.
bool MapDataStale(bool haveData, uint64_t dataTime, uint64_t gameDataTime) {
  return !haveData || gameDataTime > dataTime;
}

// The generated tables the app reads, and the game files they come from:
// maps from Data5 (and the DLCs), text from Data1, the regulation Data0.bdt.
// All tables must be there; the oldest of them is
// compared with the newest game file.
const wchar_t* const kGeneratedTables[] = {L"treasures.tsv", L"enemies.tsv", L"item_names.tsv", L"bonfire_names.tsv",
                                           L"regions.tsv", L"boss_names.tsv"};
const wchar_t* const kGameSourceFiles[] = {L"Data5.bdt", L"Data1.bdt", L"Data0.bdt"};

struct FileSetTimes {
  bool all = true, any = false;
  uint64_t oldest = 0, newest = 0;  // over the files that exist
};

FileSetTimes TimesOf(const std::vector<std::wstring>& files, const std::function<bool(const std::wstring&)>& exists,
                     const std::function<uint64_t(const std::wstring&)>& time) {
  FileSetTimes t;
  for (const auto& f : files) {
    if (!exists(f)) {
      t.all = false;
      continue;
    }
    uint64_t ft = time(f);
    t.oldest = t.any ? (std::min)(t.oldest, ft) : ft;
    t.newest = t.any ? (std::max)(t.newest, ft) : ft;
    t.any = true;
  }
  return t;
}

// The Python to run the extractor with: the bundled one in an install, the
// system one (PATH) in a dev checkout. "" when there is none.
std::wstring ExtractorPython(const AppPaths& p, const std::function<bool(const std::wstring&)>& exists,
                             const std::function<std::wstring(const wchar_t*)>& searchPath) {
  if (!p.devMode) return exists(p.BundledPython()) ? p.BundledPython() : std::wstring();
  return searchPath(L"python.exe");
}

bool FileExists(const std::wstring& f) { return GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES; }

uint64_t FileTime(const std::wstring& f) {
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &d)) return 0;
  return (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}

FileSetTimes GeneratedDataTimes() {
  std::vector<std::wstring> files;
  for (const wchar_t* t : kGeneratedTables) files.push_back(Paths().Generated(t));
  return TimesOf(files, FileExists, FileTime);
}

// The newest of the game files the generated data comes from (0: none found).
uint64_t GameFilesTime(const std::wstring& gameDir) {
  std::vector<std::wstring> files;
  for (const wchar_t* f : kGameSourceFiles) files.push_back(gameDir + L"\\" + f);
  return TimesOf(files, FileExists, FileTime).newest;
}

// For the log: why the generated data is being read.
const char* MapDataReason(const FileSetTimes& data) {
  return !data.any ? "not there yet" : !data.all ? "incomplete (new tables to read)" : "older than the game's files";
}

std::wstring SearchPathFor(const wchar_t* exe) {
  wchar_t buf[MAX_PATH];
  DWORD n = SearchPathW(nullptr, exe, nullptr, MAX_PATH, buf, nullptr);
  return n > 0 && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

std::wstring SteamPathFromRegistry() {
  wchar_t buf[MAX_PATH];
  DWORD size = sizeof(buf);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) !=
      ERROR_SUCCESS)
    return L"";
  return buf;
}

// The running DS3's exe path, from a module snapshot (no handle on the game
// is opened), and only when Seamless Co-op is loaded -- the same rule as
// attaching. "" otherwise.
std::wstring RunningGameExePath() {
  DWORD pid = FindProcessIdByName(kTargetProcessName);
  if (!pid || !HasModule(ListModuleNames(pid), kSeamlessCoopModuleMarker)) return L"";
  std::wstring path;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE) return L"";
  MODULEENTRY32W e{};
  e.dwSize = sizeof(e);
  if (Module32FirstW(snap, &e)) {
    do {
      if (_wcsicmp(e.szModule, kTargetProcessName) == 0) path = e.szExePath;
    } while (path.empty() && Module32NextW(snap, &e));
  }
  CloseHandle(snap);
  return path;
}

GameDirFound FindGameDirNow(const std::wstring& explicitDir) {
  GameDirSearch s;
  s.explicitDir = explicitDir;
  if (explicitDir.empty()) s.runningGameExe = RunningGameExePath();
  s.savedDir = WidenUtf8(g_gameDir);
  s.steamPath = SteamPathFromRegistry();
  s.exists = FileExists;
  s.readFile = ReadWholeFile;
  return FindDs3GameDir(s);
}

// A job object whose processes Windows ends when the last handle to it
// closes -- i.e. when WASD exits, however it exits. The extractor runs in
// it, so closing WASD mid-read never leaves a Python process behind holding
// the install's files (found 2026-10-07 by the installer test: uninstall
// left python\ behind). Null if the job couldn't be made.
HANDLE CreateKillOnCloseJob() {
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (!job) return nullptr;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
  info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
    CloseHandle(job);
    return nullptr;
  }
  return job;
}

HANDLE ChildProcessJob() {
  static HANDLE job = CreateKillOnCloseJob();  // closed by Windows when this process ends
  return job;
}

// Starts cmd suspended, puts it in the job (if any), then lets it run.
bool StartInJob(std::wstring cmd, HANDLE job, STARTUPINFOW* si, PROCESS_INFORMATION* pi) {
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(0);
  if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                      nullptr, si, pi))
    return false;
  if (job) AssignProcessToJobObject(job, pi->hProcess);
  ResumeThread(pi->hThread);
  return true;
}

// Runs the extractor on gameDir into Paths().generated, its output going to
// this process's console / stdout. Exit codes: the extractor's (0 done,
// 2 no game folder, 3 no archives, 4 unreadable archive), 5 no Python or it
// couldn't start.
int RunExtractor(const std::wstring& gameDir) {
  const AppPaths& p = Paths();
  std::wstring python = ExtractorPython(p, FileExists, SearchPathFor);
  if (python.empty()) {
    if (p.devMode) Log("Map data: no Python on PATH (a dev checkout runs tools\\extract_treasures.py with it).");
    else Log("Map data: the bundled Python is missing (%ls) -- reinstall WASD.", p.BundledPython().c_str());
    return 5;
  }
  EnsureDir(p.generated);
  std::wstring cmd = ExtractorCommandLine(python, p.ExtractorScript(), gameDir, p.generated);
  Log("Map data: reading the map files in %ls (about 10 s)...", gameDir.c_str());
  // Its output comes back through a pipe into Log(): WASD.exe has no
  // console, and CREATE_NO_WINDOW keeps one from flashing up.
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE readEnd = nullptr, writeEnd = nullptr;
  if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return 5;
  SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nullptr;
  si.hStdOutput = writeEnd;
  si.hStdError = writeEnd;
  PROCESS_INFORMATION pi{};
  bool started = StartInJob(cmd, ChildProcessJob(), &si, &pi);
  DWORD startError = GetLastError();
  CloseHandle(writeEnd);
  if (!started) {
    CloseHandle(readEnd);
    Log("Map data: couldn't start %ls (error %lu).", python.c_str(), startError);
    return 5;
  }
  std::string pending;
  char chunk[512];
  DWORD got = 0;
  while (ReadFile(readEnd, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
    pending.append(chunk, got);
    for (size_t nl; (nl = pending.find('\n')) != std::string::npos; pending.erase(0, nl + 1)) {
      std::string line = pending.substr(0, nl);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) Log("  %s", line.c_str());
    }
  }
  if (!pending.empty()) Log("  %s", pending.c_str());
  CloseHandle(readEnd);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 5;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (code == 0) Log("Map data: done (%ls).", p.generated.c_str());
  else Log("Map data: the extractor stopped with exit code %lu.", code);
  return static_cast<int>(code);
}

void RememberGameDir(const GameDirFound& g) {
  std::string dir = Utf8(g.dir);
  if (dir == g_gameDir) return;
  g_gameDir = dir;
  SaveSettings();
}

// wasd-cli.exe extract [--game <folder>]
int RunExtractCommand(int argc, wchar_t** argv) {
  std::wstring explicitDir;
  for (int i = 2; i < argc; ++i)
    if (std::wstring(argv[i]) == L"--game" && i + 1 < argc) explicitDir = argv[++i];
  GameDirFound g = FindGameDirNow(explicitDir);
  if (g.dir.empty()) {
    Log("Couldn't find Dark Souls III's \"Game\" folder (the one with Data5.bhd).");
    for (const auto& t : g.tried) Log("  looked in %ls", t.c_str());
    Log("Run:  wasd-cli.exe extract --game \"<...>\\DARK SOULS III\\Game\"");
    return 2;
  }
  Log("Game folder: %ls (from %s)", g.dir.c_str(), g.source.c_str());
  RememberGameDir(g);
  return RunExtractor(g.dir);
}

// `overlay`'s first-run step: read the map data when it's missing or older
// than the game's archives. Never stops the guide from starting.
void EnsureMapDataAtStart() {
  FileSetTimes data = GeneratedDataTimes();
  GameDirFound g = FindGameDirNow(L"");
  if (g.dir.empty()) {
    if (!data.all)
      Log("Map data: DS3's Game folder not found, so item positions, names and boss weaknesses stay off. "
          "Run:  wasd-cli.exe extract --game \"<...>\\DARK SOULS III\\Game\"");
    return;
  }
  if (!MapDataStale(data.all, data.oldest, GameFilesTime(g.dir))) return;
  Log("Map data: %s -- reading it from your DS3 install (first run, about 20 s).", MapDataReason(data));
  RememberGameDir(g);
  if (RunExtractor(g.dir) != 0)
    Log("Map data: not read; the guide runs without item positions. Retry with  wasd-cli.exe extract");
}

