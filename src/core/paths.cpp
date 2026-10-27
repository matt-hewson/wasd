// src/core/paths.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- AppPaths: where files live -----------------
// [AppPaths begin] The tests/cpp lint allows file locations to be spelled
// out only between these markers; everything else asks Paths().
//
// Two roots:
//   assets -- read-only data\*.tsv and templates\: the folder above the exe
//             (repo\build\wasd-cli.exe -> repo; <install>\bin\WASD.exe -> <install>).
//   user   -- read-write: settings.ini, sessions\, reports\, logs\, scratch\.
//             WASD_USER_DIR if set (tests use it); else the repo itself in a
//             dev checkout (.git and src\main.cpp beside data\), so a
//             developer's sessions and settings stay where they were; else
//             %LOCALAPPDATA%\WASD, which an install, upgrade or uninstall of
//             the program files never touches.
// Generated data (treasures.tsv, enemies.tsv and the name tables, made from
// the user's own game install) sits with the assets in a dev checkout
// (data\generated, where tools\extract_treasures.py writes it) and under the
// user root otherwise.
struct AppPaths {
  std::wstring assets, user, generated;
  bool devMode = false;
  std::wstring userSource;  // why `user` is what it is, for `paths` and the logs

  std::wstring Data(const std::wstring& file) const { return assets + L"\\data\\" + file; }
  std::wstring Template(const std::wstring& file) const { return assets + L"\\templates\\" + file; }
  std::wstring Generated(const std::wstring& file) const { return generated + L"\\" + file; }
  std::wstring Settings() const { return user + L"\\settings.ini"; }
  std::wstring Sessions() const { return user + L"\\sessions"; }
  std::wstring Reports() const { return user + L"\\reports"; }
  std::wstring Logs() const { return user + L"\\logs"; }
  std::wstring ScratchDir() const { return user + L"\\scratch"; }  // scan / findptr results
  std::wstring Scratch(const std::wstring& file) const { return ScratchDir() + L"\\" + file; }
  // Map-data extractor: the script beside data\, and
  // the embeddable Python an install ships in python\ (a dev checkout uses
  // the system Python instead).
  std::wstring ExtractorScript() const { return assets + L"\\tools\\extract_treasures.py"; }
  std::wstring BundledPython() const { return assets + L"\\python\\python.exe"; }
};

struct AppPathsInput {
  std::wstring exePath;       // the running exe (GetModuleFileNameW)
  std::wstring envUserDir;    // WASD_USER_DIR, "" when unset
  std::wstring localAppData;  // %LOCALAPPDATA%, "" when unset
  std::function<bool(const std::wstring&)> exists;
};

// "C:\a\b" -> "C:\a"; "" or a bare name -> ".".
std::wstring ParentDir(const std::wstring& path) {
  size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
}

// Pure: the same inputs always give the same paths (unit-tested).
AppPaths ResolveAppPaths(const AppPathsInput& in) {
  auto trimmed = [](std::wstring d) {
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    return d;
  };
  AppPaths p;
  std::wstring exeDir = ParentDir(in.exePath);
  p.assets = exeDir.find_last_of(L"\\/") == std::wstring::npos ? std::wstring(L".") : ParentDir(exeDir);
  p.devMode = in.exists && in.exists(p.assets + L"\\.git") && in.exists(p.assets + L"\\src\\main.cpp");
  if (!in.envUserDir.empty()) {
    p.user = trimmed(in.envUserDir);
    p.userSource = L"WASD_USER_DIR";
  } else if (p.devMode) {
    p.user = p.assets;
    p.userSource = L"dev checkout";
  } else if (!in.localAppData.empty()) {
    p.user = trimmed(in.localAppData) + L"\\" + WidenUtf8(WASD_APP_SHORT_NAME);
    p.userSource = L"%LOCALAPPDATA%";
  } else {
    p.user = p.assets + L"\\userdata";
    p.userSource = L"no %LOCALAPPDATA%";
  }
  p.generated = p.devMode ? p.assets + L"\\data\\generated" : p.user + L"\\generated";
  return p;
}

std::wstring EnvVar(const wchar_t* name) {
  wchar_t buf[2048];
  DWORD n = GetEnvironmentVariableW(name, buf, 2048);
  return n > 0 && n < 2048 ? std::wstring(buf, n) : std::wstring();
}

// This process's paths, resolved once.
const AppPaths& Paths() {
  static const AppPaths paths = [] {
    AppPathsInput in;
    wchar_t exe[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (len != 0 && len != MAX_PATH) in.exePath.assign(exe, len);
    in.envUserDir = EnvVar(L"WASD_USER_DIR");
    in.localAppData = EnvVar(L"LOCALAPPDATA");
    in.exists = [](const std::wstring& f) { return GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES; };
    return ResolveAppPaths(in);
  }();
  return paths;
}

// Creates a folder and any missing parents (CreateDirectoryW makes one
// level only, and %LOCALAPPDATA%\WASD doesn't exist on a fresh install).
bool EnsureDir(const std::wstring& dir) {
  if (dir.empty() || dir == L".") return true;
  DWORD a = GetFileAttributesW(dir.c_str());
  if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
  std::wstring parent = ParentDir(dir);
  if (parent != dir && parent.size() > 2) EnsureDir(parent);  // stop at "C:"
  return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// UTF-8 for printing paths: wprintf goes through the console code page and
// turns e.g. "é" into "?" (found 2026-10-07).
std::string Utf8(const std::wstring& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}
// [AppPaths end]

// scan / rescan candidates (was build\candidates.txt, relative to the
// current folder, so it only worked when started from the repo root).
std::wstring CandidatesPath() { return Paths().Scratch(L"candidates.txt"); }

std::string Timestamp() {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  std::time_t t = system_clock::to_time_t(now);
  std::tm tmv;
  localtime_s(&tmv, &t);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                static_cast<int>(ms.count()));
  return buf;
}

// WASD.exe has no console, so it also logs to a file (OpenLogFile,
// logs\wasd-YYYYMMDD.log in the user data folder). nullptr = console only.
FILE* g_logFile = nullptr;

void Log(const char* fmt, ...) {
  std::string ts = Timestamp();
  std::printf("[%s] ", ts.c_str());
  va_list args;
  va_start(args, fmt);
  if (g_logFile) {
    va_list copy;
    va_copy(copy, args);
    std::fprintf(g_logFile, "[%s] ", ts.c_str());
    std::vfprintf(g_logFile, fmt, copy);
    std::fputc('\n', g_logFile);
    std::fflush(g_logFile);
    va_end(copy);
  }
  std::vprintf(fmt, args);
  va_end(args);
  std::printf("\n");
  // Redirected stdout is block-buffered; flush so a log file tails live
  // (stats --live runs indefinitely). Log lines are rare -- per change,
  // not per read -- so this costs nothing measurable.
  std::fflush(stdout);
}

