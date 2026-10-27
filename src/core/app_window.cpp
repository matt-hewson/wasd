// src/core/app_window.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- the app window (WASD.exe) ---------------------
//
// WASD.exe is the same program as `wasd-cli.exe overlay` plus this window:
// it has its own taskbar button (pin it to start WASD from the taskbar),
// shows what the guide is doing, and holds the few controls that matter
// while playing. Closing it quits the guide, as the page's Quit button
// does. Its text comes from MainWindowModel (pure, unit-tested).

void BumpMapDataVersion();  // defined with the map-data tables
int RunReport(bool openInBrowser);

constexpr wchar_t kAppUserModelId[] = L"AmishGoose.WASD";  // shared with the Start menu shortcut (part 5)

enum MapDataStatus : LONG { kMapChecking = 0, kMapReading, kMapReady, kMapNoGame, kMapFailed };
volatile LONG g_mapDataStatus = kMapChecking;
bool g_appMode = false;

struct MainWindowText {
  std::string title, status, details, mapData, livePage;
  bool mapDataBusy = false;
};

MainWindowText MainWindowModel(const WindowSnapshot& s, LONG mapStatus, const std::string& liveUrl) {
  MainWindowText t;
  t.title = std::string(WASD_APP_SHORT_NAME) + " " + WASD_VERSION_STRING;
  if (s.paused) t.status = "Paused (F10): no memory reads until you press F10 again";
  else if (s.haveValues) t.status = "Attached to Dark Souls III (Seamless Co-op)";
  else if (!s.status.empty()) t.status = s.status;
  else t.status = "Waiting for Dark Souls III with Seamless Co-op";
  if (s.haveValues) {
    t.details = s.characterName.empty() ? std::string("Your character") : s.characterName;
    if (s.level > 0) t.details += ", level " + std::to_string(s.level);
    if (!s.currentArea.empty()) t.details += "  \xC2\xB7  " + s.currentArea;
    t.details += "  \xC2\xB7  session " + FormatDuration(s.sessionSeconds);
  } else {
    t.details = "Start the game through Seamless Co-op; WASD attaches by itself.";
  }
  switch (mapStatus) {
    case kMapReading:
      t.mapData = "Map data: reading it from your DS3 install (about 10 s)...";
      t.mapDataBusy = true;
      break;
    case kMapReady: t.mapData = "Map data: ready"; break;
    case kMapNoGame: t.mapData = "Map data: DS3's Game folder not found. Use \"Read map data\" to choose it."; break;
    case kMapFailed: t.mapData = "Map data: couldn't be read (the log says why)."; break;
    default: t.mapData = "Map data: checking..."; break;
  }
  t.livePage = liveUrl.empty() ? "Live page: starting..." : "Live page: " + liveUrl;
  return t;
}

// WASD_INSTANCE (tests): a separate identity -- mutex and window class --
// so a test copy never meets a real one. Letters, digits and '-' only.
std::wstring SanitizeInstance(const std::wstring& v) {
  std::wstring out;
  for (wchar_t c : v)
    if (iswalnum(c) || c == L'-') out += c;
  return out.substr(0, 40);
}
std::wstring InstanceMutexName(const std::wstring& suffix) {
  return L"Local\\AmishGoose.WASD" + (suffix.empty() ? std::wstring() : L"." + suffix);
}
std::wstring MainWindowClassName(const std::wstring& suffix) {
  return L"WasdMainWindow" + (suffix.empty() ? std::wstring() : L"." + suffix);
}

// -- background map data ---------------------------------------------------
std::wstring g_mapThreadGameDir;

DWORD WINAPI MapDataThread(LPVOID) {
  int code = RunExtractor(g_mapThreadGameDir);
  if (code == 0) BumpMapDataVersion();  // the poll worker reloads the tables at its next tick
  InterlockedExchange(&g_mapDataStatus, code == 0 ? kMapReady : kMapFailed);
  PostMessageW(g_window.hwnd, kMsgMapData, 0, 0);
  return 0;
}

void StartMapDataThread(const std::wstring& gameDir) {
  if (g_mapDataStatus == kMapReading) return;
  g_mapThreadGameDir = gameDir;
  InterlockedExchange(&g_mapDataStatus, kMapReading);
  HANDLE t = CreateThread(nullptr, 0, MapDataThread, nullptr, 0, nullptr);
  if (t) CloseHandle(t);
  else InterlockedExchange(&g_mapDataStatus, kMapFailed);
  RefreshMainWindow();
}

// What `overlay` does before starting (EnsureMapDataAtStart), but in the
// background so the window and the live page come up straight away.
void StartFirstRunMapData() {
  FileSetTimes data = GeneratedDataTimes();
  GameDirFound g = FindGameDirNow(L"");
  if (g.dir.empty()) {
    InterlockedExchange(&g_mapDataStatus, data.all ? kMapReady : kMapNoGame);
    if (!data.all) Log("Map data: DS3's Game folder not found; choose it with \"Read map data\" in the WASD window.");
    return;
  }
  if (!MapDataStale(data.all, data.oldest, GameFilesTime(g.dir))) {
    InterlockedExchange(&g_mapDataStatus, kMapReady);
    return;
  }
  Log("Map data: %s -- reading it from your DS3 install in the background.", MapDataReason(data));
  RememberGameDir(g);
  StartMapDataThread(g.dir);
}

std::wstring PickFolder(HWND owner, const wchar_t* title) {
  std::wstring out;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dlg->SetTitle(title);
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        out = path;
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

// "Read map data": the found game folder, else ask for it.
void OnReadMapData(HWND hwnd) {
  GameDirFound g = FindGameDirNow(L"");
  if (g.dir.empty()) {
    std::wstring dir = PickFolder(hwnd, L"Choose the DARK SOULS III \"Game\" folder (the one with Data5.bdt)");
    if (dir.empty()) return;
    g = FindGameDirNow(dir);
    if (g.dir.empty()) {
      MessageBoxW(hwnd, L"That folder has no Data5.bhd. Choose the \"Game\" folder inside DARK SOULS III.",
                  L"WASD", MB_OK | MB_ICONINFORMATION);
      return;
    }
  }
  RememberGameDir(g);
  StartMapDataThread(g.dir);
}

// -- the window itself -------------------------------------------------------
enum MainControl : int {
  kIdStatus = 101, kIdDetails, kIdMapData, kIdReadMapData, kIdLivePage, kIdOpenPage,
  kIdOverlay, kIdTierLabel, kIdTier, kIdResults, kIdDataFolder, kIdQuit
};
HFONT g_mainFont = nullptr, g_mainBoldFont = nullptr;

void SetTextIfChanged(HWND w, const std::wstring& text) {
  wchar_t buf[512];
  GetWindowTextW(w, buf, 512);
  if (text != buf) SetWindowTextW(w, text.c_str());
}

void RefreshMainWindow() {
  if (!g_mainWindow) return;
  WindowSnapshot snap;
  EnterCriticalSection(&g_window.lock);
  snap = g_window.snapshot;
  LeaveCriticalSection(&g_window.lock);
  MainWindowText t = MainWindowModel(snap, g_mapDataStatus, Utf8(g_liveUrl));
  SetTextIfChanged(g_mainWindow, WidenUtf8(t.title));
  SetTextIfChanged(GetDlgItem(g_mainWindow, kIdStatus), WidenUtf8(t.status));
  SetTextIfChanged(GetDlgItem(g_mainWindow, kIdDetails), WidenUtf8(t.details));
  SetTextIfChanged(GetDlgItem(g_mainWindow, kIdMapData), WidenUtf8(t.mapData));
  SetTextIfChanged(GetDlgItem(g_mainWindow, kIdLivePage), WidenUtf8(t.livePage));
  EnableWindow(GetDlgItem(g_mainWindow, kIdReadMapData), !t.mapDataBusy);
  EnableWindow(GetDlgItem(g_mainWindow, kIdOpenPage), g_liveUrl[0] != 0);
  CheckDlgButton(g_mainWindow, kIdOverlay, g_ov.visible ? BST_CHECKED : BST_UNCHECKED);
  HWND tier = GetDlgItem(g_mainWindow, kIdTier);
  if (!SendMessageW(tier, CB_GETDROPPEDSTATE, 0, 0) && SendMessageW(tier, CB_GETCURSEL, 0, 0) != SpoilerTier())
    SendMessageW(tier, CB_SETCURSEL, SpoilerTier(), 0);
}

LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_COMMAND: {
      int id = LOWORD(wParam), code = HIWORD(wParam);
      if (id == kIdOpenPage && g_liveUrl[0]) ShellExecuteW(nullptr, L"open", g_liveUrl, nullptr, nullptr, SW_SHOWNORMAL);
      else if (id == kIdReadMapData) OnReadMapData(hwnd);
      else if (id == kIdResults && g_liveUrl[0])  // served, so its co-op card works
        ShellExecuteW(nullptr, L"open", (std::wstring(g_liveUrl) + L"results").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      else if (id == kIdResults) RunReport(true);
      else if (id == kIdDataFolder) {
        EnsureDir(Paths().user);
        ShellExecuteW(nullptr, L"open", Paths().user.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      } else if (id == kIdQuit) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
      } else if (id == kIdOverlay && code == BN_CLICKED) {
        InterlockedExchange(&g_ov.visible, IsDlgButtonChecked(hwnd, kIdOverlay) == BST_CHECKED ? 1 : 0);
        SaveSettings();
        PostMessageW(g_window.hwnd, kMsgOverlaySettings, 0, 0);
      } else if (id == kIdTier && code == CBN_SELCHANGE) {
        LRESULT sel = SendMessageW(reinterpret_cast<HWND>(lParam), CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel <= 3) {
          InterlockedExchange(&g_spoilerTier, static_cast<LONG>(sel));
          SaveSettings();
          PostMessageW(g_window.hwnd, kMsgSpoilerTierSet, 0, 0);
        }
      }
      return 0;
    }
    case kMsgShowYourself:
      ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
      SetForegroundWindow(hwnd);
      return 0;
    case WM_CLOSE:
      Log("Quit from the WASD window.");
      if (g_window.hwnd) PostMessageW(g_window.hwnd, WM_CLOSE, 0, 0);  // same shutdown as the page's Quit
      else DestroyWindow(hwnd);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HFONT MessageFont(bool bold) {
  NONCLIENTMETRICSW m{};
  m.cbSize = sizeof(m);
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(m), &m, 0);
  if (bold) m.lfMessageFont.lfWeight = FW_SEMIBOLD;
  return CreateFontIndirectW(&m.lfMessageFont);
}

void CreateMainWindow(HINSTANCE instance) {
  std::wstring cls = MainWindowClassName(SanitizeInstance(EnvVar(L"WASD_INSTANCE")));
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = MainWindowProc;
  wc.hInstance = instance;
  wc.lpszClassName = cls.c_str();
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW (this build has no UNICODE define)
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                           GetSystemMetrics(SM_CYICON), 0));
  wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                             GetSystemMetrics(SM_CYSMICON), 0));
  RegisterClassExW(&wc);
  double sc = g_window.scale > 0 ? g_window.scale : 1.0;
  auto px = [sc](int v) { return static_cast<int>(v * sc + 0.5); };
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT r{0, 0, px(560), px(232)};
  AdjustWindowRectEx(&r, style, FALSE, WS_EX_APPWINDOW);
  std::wstring title = WidenUtf8(std::string(WASD_APP_SHORT_NAME) + " " + WASD_VERSION_STRING);
  g_mainWindow = CreateWindowExW(WS_EX_APPWINDOW, cls.c_str(), title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                                 r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, nullptr);
  if (!g_mainWindow) return;
  if (!g_mainFont) g_mainFont = MessageFont(false);
  if (!g_mainBoldFont) g_mainBoldFont = MessageFont(true);
  auto add = [&](const wchar_t* cls2, const wchar_t* text, DWORD st, int x, int y, int w, int h, int id, bool bold = false) {
    HWND c = CreateWindowExW(0, cls2, text, WS_CHILD | WS_VISIBLE | st, px(x), px(y), px(w), px(h), g_mainWindow,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(bold ? g_mainBoldFont : g_mainFont), TRUE);
    return c;
  };
  add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 16, 14, 528, 22, kIdStatus, true);
  add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 16, 40, 528, 20, kIdDetails);
  add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 16, 76, 372, 20, kIdMapData);
  add(L"BUTTON", L"Read map data...", BS_PUSHBUTTON | WS_TABSTOP, 400, 70, 144, 28, kIdReadMapData);
  add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 16, 112, 372, 20, kIdLivePage);
  add(L"BUTTON", L"Open live page", BS_PUSHBUTTON | WS_TABSTOP, 400, 106, 144, 28, kIdOpenPage);
  add(L"BUTTON", L"Show overlay", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 150, 150, 24, kIdOverlay);
  add(L"STATIC", L"Spoiler tier:", SS_RIGHT, 220, 154, 90, 20, kIdTierLabel);
  HWND tier = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 318, 149, 120, 200, kIdTier);
  for (int t = 0; t <= 3; ++t) SendMessageW(tier, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(WidenUtf8(SpoilerTierName(t)).c_str()));
  add(L"BUTTON", L"Results page", BS_PUSHBUTTON | WS_TABSTOP, 16, 192, 130, 28, kIdResults);
  add(L"BUTTON", L"Open data folder", BS_PUSHBUTTON | WS_TABSTOP, 156, 192, 150, 28, kIdDataFolder);
  add(L"BUTTON", L"Quit", BS_PUSHBUTTON | WS_TABSTOP, 444, 192, 100, 28, kIdQuit);
  RefreshMainWindow();
  ShowWindow(g_mainWindow, SW_SHOWNORMAL);
  UpdateWindow(g_mainWindow);
}

// `WASD.exe --smoke-test`: the window comes up as a real app window, with no
// game, overlay or live page, then closes. Prints "ok"/"FAIL" lines (to a
// pipe when a test runs it) and exits 0 when everything holds.
int RunSmokeTest() {
  SetProcessDPIAware();
  InitializeCriticalSection(&g_window.lock);
  HDC screen = GetDC(nullptr);
  g_window.scale = GetDeviceCaps(screen, LOGPIXELSY) / 96.0;
  ReleaseDC(nullptr, screen);
  CreateMainWindow(GetModuleHandleW(nullptr));
  int failures = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
  };
  check(g_mainWindow && IsWindowVisible(g_mainWindow), "window shown");
  LONG_PTR ex = g_mainWindow ? GetWindowLongPtrW(g_mainWindow, GWL_EXSTYLE) : 0;
  check((ex & WS_EX_APPWINDOW) && !(ex & WS_EX_TOOLWINDOW) && g_mainWindow && !GetWindow(g_mainWindow, GW_OWNER),
        "taskbar button (WS_EX_APPWINDOW, not a tool window, no owner)");
  PWSTR id = nullptr;
  bool aumid = SUCCEEDED(GetCurrentProcessExplicitAppUserModelID(&id)) && id && std::wstring(id) == kAppUserModelId;
  if (id) CoTaskMemFree(id);
  check(aumid, "AppUserModelID AmishGoose.WASD");
  wchar_t title[128] = {};
  if (g_mainWindow) GetWindowTextW(g_mainWindow, title, 128);
  check(Utf8(title) == std::string(WASD_APP_SHORT_NAME) + " " + WASD_VERSION_STRING, "title \"WASD <version>\"");
  check(FileExists(Paths().Data(L"bosses.tsv")) && FileExists(Paths().Template(L"live.html")), "assets found");
  check(GetDlgItem(g_mainWindow, kIdQuit) && GetDlgItem(g_mainWindow, kIdTier) && GetDlgItem(g_mainWindow, kIdReadMapData),
        "controls");
  if (g_mainWindow) DestroyWindow(g_mainWindow);
  g_mainWindow = nullptr;
  std::fflush(stdout);
  return failures ? 1 : 0;
}

// WASD.exe: single instance, the taskbar identity, a log file, then the
// overlay machinery with the app window. Exit codes: 0, 3 already running
// (the running window was brought to the front), 4 couldn't create windows.
int RunApp(const std::wstring& args) {
  SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);
  LoadSettings();
  if (args.find(L"--smoke-test") != std::wstring::npos) return RunSmokeTest();
  std::wstring suffix = SanitizeInstance(EnvVar(L"WASD_INSTANCE"));
  HANDLE mutex = CreateMutexW(nullptr, TRUE, InstanceMutexName(suffix).c_str());
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND w = FindWindowW(MainWindowClassName(suffix).c_str(), nullptr)) {
      PostMessageW(w, kMsgShowYourself, 0, 0);
      SetForegroundWindow(w);  // allowed: this process was just started by the user
    }
    CloseHandle(mutex);
    return 3;
  }
  OpenLogFile();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  g_appMode = true;
  int code = RunOverlay(true);
  CoUninitialize();
  if (mutex) {
    ReleaseMutex(mutex);
    CloseHandle(mutex);
  }
  if (g_logFile) std::fclose(g_logFile);
  g_logFile = nullptr;
  return code;
}

std::vector<std::string> SplitStr(const std::string& s, char sep);  // defined with the route

// A session's perf.csv (kPerfCsvHeader) as a JSON array of rows,
// [["time", session_t, window_s, ...], ...], or "" when it has none. The
// header and any row that isn't 10 fields -- a time, then 9 finite
// numbers -- are skipped (e.g. a row cut off by a crash mid-write).
std::string PerfCsvToJson(const std::string& csv) {
  std::string perf;
  for (size_t p = 0; p < csv.size();) {
    size_t eol = csv.find('\n', p);
    std::string line = csv.substr(p, eol == std::string::npos ? std::string::npos : eol - p);
    p = eol == std::string::npos ? csv.size() : eol + 1;
    while (!line.empty() && line.back() == '\r') line.pop_back();
    auto fields = SplitStr(line, ',');
    if (fields.size() != 10 || fields[0].empty() ||
        fields[0].find_first_not_of("0123456789-:T") != std::string::npos)
      continue;
    std::string row = "[\"" + fields[0] + "\"";
    bool ok = true;
    for (size_t f = 1; f < fields.size() && ok; ++f) {
      char* end = nullptr;
      double v = std::strtod(fields[f].c_str(), &end);
      ok = !fields[f].empty() && end && *end == '\0' && std::isfinite(v);
      row += "," + fields[f];
    }
    if (!ok) continue;
    perf += (perf.empty() ? "" : ",") + row + "]";
  }
  return perf.empty() ? "" : "[" + perf + "]";
}

// The results page from its template: the sessions JSON goes in place of
// the last __SESSIONS_JSON__ (the script's data line; the header comment
// mentions the token too), with "</" escaped so the data can't close the
// <script> early, and every __APP_VERSION__ becomes the app's version.
// "" when the template has no data token.
std::string BuildReportHtml(std::string tmpl, std::string sessionsJson) {
  const std::string token = "__SESSIONS_JSON__", version = "__APP_VERSION__";
  if (tmpl.rfind(token) == std::string::npos) return "";
  // The version first, so session data that happens to contain the token
  // (a character name, say) is left alone.
  for (size_t p = 0; (p = tmpl.find(version, p)) != std::string::npos; p += std::strlen(WASD_VERSION_STRING))
    tmpl.replace(p, version.size(), WASD_VERSION_STRING);
  for (size_t p = 0; (p = sessionsJson.find("</", p)) != std::string::npos; p += 3) sessionsJson.replace(p, 2, "<\\/");
  size_t at = tmpl.rfind(token);
  tmpl.replace(at, token.size(), sessionsJson);
  return tmpl;
}

// A fingerprint of the saved sessions: every session and perf file's name,
// size and last write time, hashed (FNV-1a). It changes whenever a session
// is recorded to, so the results page can tell when to fetch them again
// without WASD reading them. GetFileAttributesExW, not the directory
// listing's figures, which NTFS updates late for a file still open.
std::string SessionsStamp() {
  uint64_t h = 1469598103934665603ULL;
  auto mix = [&](const void* p, size_t n) {
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const unsigned char*>(p)[i]) * 1099511628211ULL;
  };
  WIN32_FIND_DATAW player;
  HANDLE players = FindFirstFileW((Paths().Sessions() + L"\\*").c_str(), &player);
  if (players != INVALID_HANDLE_VALUE) {
    do {
      if (!(player.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || player.cFileName[0] == L'.') continue;
      std::wstring dir = Paths().Sessions() + L"\\" + player.cFileName;
      WIN32_FIND_DATAW file;
      HANDLE files = FindFirstFileW((dir + L"\\*.*").c_str(), &file);
      if (files == INVALID_HANDLE_VALUE) continue;
      do {
        std::wstring name = file.cFileName;
        auto ends = [&](const wchar_t* tail) {
          size_t n = std::wcslen(tail);
          return name.size() >= n && name.compare(name.size() - n, n, tail) == 0;
        };
        if (!ends(L".jsonl") && !ends(L".perf.csv")) continue;
        std::wstring path = dir + L"\\" + name;
        WIN32_FILE_ATTRIBUTE_DATA a = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) continue;
        mix(path.data(), path.size() * sizeof(wchar_t));
        mix(&a.nFileSizeLow, sizeof a.nFileSizeLow);
        mix(&a.nFileSizeHigh, sizeof a.nFileSizeHigh);
        mix(&a.ftLastWriteTime, sizeof a.ftLastWriteTime);
      } while (FindNextFileW(files, &file));
      FindClose(files);
    } while (FindNextFileW(players, &player));
    FindClose(players);
  }
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

// Every saved session as the results page's JSON array: {"file", "events",
// "perf"?} per session file.
std::string SessionsJson(int* sessionsOut, int* eventsOut) {
  std::string json = "[";
  int sessionCount = 0, eventCount = 0;
  WIN32_FIND_DATAW player;
  HANDLE players = FindFirstFileW((Paths().Sessions() + L"\\*").c_str(), &player);
  if (players != INVALID_HANDLE_VALUE) {
    do {
      if (!(player.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || player.cFileName[0] == L'.')
        continue;
      std::wstring dir = Paths().Sessions() + L"\\" + player.cFileName;
      WIN32_FIND_DATAW file;
      HANDLE files = FindFirstFileW((dir + L"\\*.jsonl").c_str(), &file);
      if (files == INVALID_HANDLE_VALUE) continue;
      do {
        std::string text = ReadWholeFile(dir + L"\\" + file.cFileName);
        std::string events;
        size_t pos = 0;
        while (pos < text.size()) {
          size_t eol = text.find('\n', pos);
          std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
          pos = eol == std::string::npos ? text.size() : eol + 1;
          while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
          if (line.size() < 2 || line.front() != '{' || line.back() != '}') continue;
          if (!events.empty()) events += ",";
          events += line;
          ++eventCount;
        }
        if (events.empty()) continue;
        int n = WideCharToMultiByte(CP_UTF8, 0, file.cFileName, -1, nullptr, 0, nullptr, nullptr);
        std::string fileName(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, file.cFileName, -1, fileName.data(), n, nullptr, nullptr);
        if (sessionCount++) json += ",";
        json += "{\"file\":" + JsonString(fileName) + ",\"events\":[" + events + "]";
        // The session's perf figures (<stamp>.perf.csv, kPerfCsvHeader), if
        // any, as "perf": [["time", session_t, window_s, ...], ...]. Rows
        // that aren't 10 fields with numbers after the time are skipped.
        std::wstring stem = file.cFileName;
        stem.resize(stem.size() - 6);  // ".jsonl"
        std::string perf = PerfCsvToJson(ReadWholeFile(dir + L"\\" + stem + L".perf.csv"));
        if (!perf.empty()) json += ",\"perf\":" + perf;
        json += "}";
      } while (FindNextFileW(files, &file));
      FindClose(files);
    } while (FindNextFileW(players, &player));
    FindClose(players);
  }
  json += "]";
  if (sessionsOut) *sessionsOut = sessionCount;
  if (eventsOut) *eventsOut = eventCount;
  return json;
}

// The results page with every saved session in it, or "" (logged) when the
// template is missing. `report` writes it to a file; the live server also
// serves it at /results, where its co-op card can reach /api/coop and it
// keeps itself current from /api/sessions.
std::string ResultsPageHtml(int* sessionsOut, int* eventsOut) {
  const std::wstring templatePath = Paths().Template(L"results_template.html");
  std::string tmpl = ReadWholeFile(templatePath);
  if (BuildReportHtml(tmpl, "[]").empty()) {
    Log("Template missing or has no __SESSIONS_JSON__ token: %ls", templatePath.c_str());
    return "";
  }
  return BuildReportHtml(tmpl, SessionsJson(sessionsOut, eventsOut));
}

int RunReport(bool openInBrowser) {
  int sessionCount = 0, eventCount = 0;
  std::string tmpl = ResultsPageHtml(&sessionCount, &eventCount);
  if (tmpl.empty()) return 4;
  std::wstring outDir = Paths().Reports();
  EnsureDir(outDir);
  std::wstring outPath = outDir + L"\\results.html";
  FILE* out = nullptr;
  if (_wfopen_s(&out, outPath.c_str(), L"wb") != 0 || !out) {
    Log("Couldn't write %ls", outPath.c_str());
    return 4;
  }
  std::fwrite(tmpl.data(), 1, tmpl.size(), out);
  std::fclose(out);
  Log("Wrote %ls -- %d session(s), %d event(s).", outPath.c_str(), sessionCount, eventCount);
  if (openInBrowser) ShellExecuteW(nullptr, L"open", outPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return 0;
}

