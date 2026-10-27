// src/core/overlay.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

std::wstring Widen(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

struct WindowTheme {
  COLORREF background = RGB(22, 22, 26);
  COLORREF text = RGB(225, 225, 230);
  COLORREF dim = RGB(140, 140, 152);
  COLORREF barTrack = RGB(48, 48, 56);
  COLORREF hp = RGB(178, 42, 42);
  COLORREF fp = RGB(52, 96, 196);
  COLORREF stamina = RGB(58, 150, 72);
  COLORREF paused = RGB(214, 150, 40);
  COLORREF warn = RGB(220, 90, 70);
};

// ---- overlay (Milestone 4) -------------------------------------------
//
// A compact HUD panel drawn over the top-right of the game's client area
// (empty in DS3's own HUD), tracking the game window as it moves or
// resizes. Requires the game windowed or borderless: in exclusive
// fullscreen no other window can appear over it (see docs/TECHNICAL.md).
//
//   - Click-through and never focused: WS_EX_LAYERED | WS_EX_TRANSPARENT
//     | WS_EX_NOACTIVATE, so mouse and keyboard always reach the game.
//   - Panel-sized, not a transparent sheet over the whole game: DWM only
//     composites a few hundred pixels square at 10Hz. Rounded corners
//     use a colour key; the panel itself is drawn at the opacity setting.
//   - Tracking is event-driven: an out-of-context WinEvent hook (no
//     injection -- events are delivered to this thread's message loop)
//     for the game window's location changes and for foreground
//     changes, plus a cheap recheck on every snapshot as a fallback.
//   - Shown only while the game is the foreground window and not
//     minimized, so it doesn't float over other apps after alt-tab.
//   - F10 hides it AND pauses polling (brief: "same hotkey toggles
//     overlay visibility + polling together"); F11 toggles the perf
//     figures, hidden by default for actual play sessions.
constexpr wchar_t kOverlayClassName[] = L"WasdOverlay";
constexpr COLORREF kOverlayColorKey = RGB(255, 0, 255);  // fully transparent
constexpr double kOverlayMargin = 16.0;                   // from the game's client edge
// Opacity, width, text size and corner are live settings (g_ov).

struct OverlayState {
  HWND hwnd = nullptr;
  HWND game = nullptr;
  DWORD gamePid = 0;
  HWINEVENTHOOK foregroundHook = nullptr, locationHook = nullptr;
  int lineCount = 1;      // of the current content (the panel's height follows it; 0 = hidden)
  int fontPx = 0;         // size g_window.font was made at
  int charsPerLine = 38;  // measured from the font at start
};
OverlayState g_overlay;

int OverlayLineCount() { return g_overlay.lineCount; }

// Wraps at " | " breaks first (the boss hint's sections), then at spaces.
std::vector<std::wstring> WrapOverlayText(const std::wstring& text, size_t maxChars) {
  std::vector<std::wstring> segments;
  for (size_t pos = 0;;) {
    size_t sep = text.find(L" | ", pos);
    segments.push_back(text.substr(pos, sep == std::wstring::npos ? std::wstring::npos : sep - pos));
    if (sep == std::wstring::npos) break;
    pos = sep + 3;
  }
  std::vector<std::wstring> lines;
  for (const auto& seg : segments) {
    if (!lines.empty() && lines.back().size() + 3 + seg.size() <= maxChars) {
      lines.back() += L" | " + seg;
      continue;
    }
    std::wstring rest = seg;
    while (rest.size() > maxChars) {
      size_t cut = rest.rfind(L' ', maxChars);
      if (cut == std::wstring::npos || cut == 0) cut = maxChars;
      lines.push_back(rest.substr(0, cut));
      rest = rest.substr(cut + (rest[cut] == L' ' ? 1 : 0));
    }
    lines.push_back(rest);
  }
  return lines;
}

struct OverlayLine {
  std::wstring text;
  COLORREF color;
};

// What the overlay shows -- only what you'd act on mid-fight (user's
// choice, 2026-10-06): the spoiler tier with the nearest unfound item,
// bosses done / total here, and the nearest boss's weaknesses and
// resistances as far as the tier allows. Everything else is on the live
// page.
std::vector<OverlayLine> OverlayLines(const WindowSnapshot& snap, const WindowTheme& theme) {
  std::vector<OverlayLine> out;
  const size_t width = static_cast<size_t>((std::max)(16, g_overlay.charsPerLine));
  auto add = [&](const std::string& text, COLORREF color) {
    for (const auto& l : WrapOverlayText(Widen(text), width)) out.push_back({l, color});
  };
  if (!snap.status.empty() && !snap.haveValues) {
    add(snap.status, theme.warn);
  } else {
    const int tier = SpoilerTier();
    if (g_ov.nearest) {
      // Vague: only how many are left -- no direction or distance, like the page.
      std::string nearText = tier == kTierOff ? "hints off (F9)"
                         : tier == kTierVague
                             ? (snap.itemsFound >= 0 ? std::to_string(snap.itemsTotal - snap.itemsFound) +
                                                           " unfound items here"
                                                     : std::string("--"))
                             : snap.nearestShort.empty() ? std::string("--") : snap.nearestShort;
      add("[" + std::string(SpoilerTierName(tier)) + "] " + nearText, theme.text);
    }
    if (g_ov.bosses)
      add((snap.bossesTotal > 0 ? "Bosses " + std::to_string(snap.bossesDefeated) + " / " +
                                      std::to_string(snap.bossesTotal) + " defeated here"
                                : std::string(snap.bossesLater ? "No bosses here yet" : "No bosses here")) +
              (snap.bossesLater ? " (+" + std::to_string(snap.bossesLater) + " later)" : ""),
          theme.dim);
    if (g_ov.bossHint && !snap.bossHint.empty()) add(snap.bossHint, theme.text);
    if (g_ov.missable && !snap.missableAlert.empty()) add("! " + snap.missableAlert, theme.warn);
    if (g_ov.route && !snap.routeHint.empty()) add("Route: " + snap.routeHint, theme.dim);
  }
  if (g_ov.perf) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "read %s  %.1f us  %.2f Hz", snap.lastReadTime.c_str(), snap.lastLatencyMicros,
                  snap.perf.valid ? snap.perf.rateHz : 0.0);
    add(buf, theme.dim);
    std::snprintf(buf, sizeof(buf), "CPU %.2f%%   mem %.1f MB", snap.perf.valid ? snap.perf.cpuPercentOfCore : 0.0,
                  snap.perf.valid ? snap.perf.workingSetMB : 0.0);
    add(buf, theme.dim);
  }
  return out;
}

double OverlayLineHeight() { return g_ov.text * 4.0 / 3.0; }  // 20 px at the default 15

int OverlayHeightPx() {
  const double scale = g_window.scale;
  return static_cast<int>((14.0 + OverlayLineCount() * OverlayLineHeight() + 10.0) * scale + 0.5);
}

struct FindGameWindowCtx {
  DWORD pid;
  HWND best;
  LONG bestArea;
};

BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM param) {
  auto* ctx = reinterpret_cast<FindGameWindowCtx*>(param);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != ctx->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr)
    return TRUE;
  RECT r;
  GetClientRect(hwnd, &r);
  LONG area = r.right * r.bottom;
  if (area > ctx->bestArea) {
    ctx->best = hwnd;
    ctx->bestArea = area;
  }
  return TRUE;
}

// The game's main window: its largest visible, unowned top-level window.
HWND FindGameWindow(DWORD pid) {
  FindGameWindowCtx ctx{pid, nullptr, 0};
  EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&ctx));
  return ctx.best;
}

void CALLBACK OnOverlayWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG,
                                DWORD, DWORD);
bool OverlayPreviewActive();

// Show/hide/move the overlay to match the game window. Cheap enough to
// call on every event and every snapshot.
void RepositionOverlay() {
  if (!g_overlay.hwnd) return;
  if (g_overlay.gamePid != 0 && (!g_overlay.game || !IsWindow(g_overlay.game))) {
    g_overlay.game = FindGameWindow(g_overlay.gamePid);
  }
  // Normally only while the game is focused; while the live page's overlay
  // settings are open (preview), also when it isn't, so changes show live.
  bool visible = g_overlay.game && !g_window.paused && g_ov.visible && g_overlay.lineCount > 0 &&
                 !IsIconic(g_overlay.game) &&
                 (GetForegroundWindow() == g_overlay.game || OverlayPreviewActive());
  if (!visible) {
    if (IsWindowVisible(g_overlay.hwnd)) ShowWindow(g_overlay.hwnd, SW_HIDE);
    return;
  }
  RECT client;
  GetClientRect(g_overlay.game, &client);
  POINT topLeft{client.left, client.top}, bottomRight{client.right, client.bottom};
  ClientToScreen(g_overlay.game, &topLeft);
  ClientToScreen(g_overlay.game, &bottomRight);
  const double scale = g_window.scale;
  int w = static_cast<int>(g_ov.width * scale + 0.5);
  int h = OverlayHeightPx();
  int margin = static_cast<int>(kOverlayMargin * scale + 0.5);
  bool left = g_ov.corner == 1 || g_ov.corner == 3, bottom = g_ov.corner >= 2;
  int x = left ? topLeft.x + margin : bottomRight.x - w - margin;
  int y = bottom ? bottomRight.y - h - margin : topLeft.y + margin;
  RECT want{x, y, x + w, y + h};
  RECT have;
  GetWindowRect(g_overlay.hwnd, &have);
  if (IsWindowVisible(g_overlay.hwnd) && EqualRect(&want, &have)) return;  // nothing to do
  SetWindowPos(g_overlay.hwnd, HWND_TOPMOST, want.left, want.top, w, h,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void CALLBACK OnOverlayWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG,
                                DWORD, DWORD) {
  if (event == EVENT_OBJECT_LOCATIONCHANGE && (hwnd != g_overlay.game || idObject != OBJID_WINDOW))
    return;  // the game also reports cursor/caret moves; only its window matters
  RepositionOverlay();
}

// Location changes are hooked per game process, so they can only be
// installed once the worker has attached and published a PID.
void EnsureOverlayGameHooks(DWORD pid) {
  if (pid == 0 || g_overlay.gamePid == pid) return;
  g_overlay.gamePid = pid;
  g_overlay.game = FindGameWindow(pid);
  if (g_overlay.locationHook) UnhookWinEvent(g_overlay.locationHook);
  g_overlay.locationHook =
      SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                      OnOverlayWinEvent, pid, 0, WINEVENT_OUTOFCONTEXT);
  if (!g_overlay.game) Log("Overlay: game window not found yet -- will keep looking.");
}

void PaintOverlay(HWND hwnd, HDC target) {
  WindowSnapshot snap;
  EnterCriticalSection(&g_window.lock);
  snap = g_window.snapshot;
  LeaveCriticalSection(&g_window.lock);

  RECT client;
  GetClientRect(hwnd, &client);
  int width = client.right, height = client.bottom;
  HDC dc = CreateCompatibleDC(target);
  HBITMAP bmp = CreateCompatibleBitmap(target, width, height);
  HGDIOBJ oldBmp = SelectObject(dc, bmp);
  const WindowTheme theme;
  const double scale = g_window.scale;
  auto px = [&](double v) { return static_cast<int>(v * scale + 0.5); };

  // Colour-keyed background, then a rounded dark panel.
  HBRUSH key = CreateSolidBrush(kOverlayColorKey);
  FillRect(dc, &client, key);
  DeleteObject(key);
  HBRUSH panel = CreateSolidBrush(theme.background);
  HPEN border = CreatePen(PS_SOLID, 1, theme.barTrack);
  HGDIOBJ oldBrush = SelectObject(dc, panel);
  HGDIOBJ oldPen = SelectObject(dc, border);
  RoundRect(dc, 0, 0, width, height, px(10), px(10));
  SelectObject(dc, oldBrush);
  SelectObject(dc, oldPen);
  DeleteObject(panel);
  DeleteObject(border);

  HGDIOBJ oldFont = SelectObject(dc, g_window.font);
  SetBkMode(dc, TRANSPARENT);
  const int margin = px(12), lineH = px(OverlayLineHeight());
  int y = px(10);
  auto line = [&](const std::wstring& s, COLORREF color) {
    SetTextColor(dc, color);
    TextOutW(dc, margin, y, s.c_str(), static_cast<int>(s.size()));
    y += lineH;
  };
  for (const auto& l : OverlayLines(snap, theme)) line(l.text, l.color);

  BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
  SelectObject(dc, oldFont);
  SelectObject(dc, oldBmp);
  DeleteObject(bmp);
  DeleteDC(dc);
}

LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_NCHITTEST:
      return HTTRANSPARENT;  // belt and braces alongside WS_EX_TRANSPARENT
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      PaintOverlay(hwnd, dc);
      EndPaint(hwnd, &ps);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Everything the overlay draws, as one string: it only repaints (and
// resizes) when this changes -- most ticks, nothing does.
std::string OverlayContentKey(const WindowSnapshot& snap) {
  std::string key = snap.status + "|" + (snap.haveValues ? "1" : "0") + "|" + std::to_string(SpoilerTier()) + "|" +
                    snap.nearestShort + "|" + std::to_string(snap.bossesDefeated) + "/" +
                    std::to_string(snap.bossesTotal) + "+" + std::to_string(snap.bossesLater) + "|" + snap.bossHint + "|" + snap.routeHint + "|" + snap.missableAlert + "|" +
                    std::to_string(snap.itemsFound) +
                    "/" + std::to_string(snap.itemsTotal);
  for (const auto& d : kOverlaySettingDefs) key += "|" + std::to_string(*d.value);
  if (g_ov.perf) key += snap.lastReadTime;  // perf lines change every tick
  return key;
}
std::string g_lastOverlayContentKey;

// Re-lays out the overlay from the latest snapshot: line count (height),
// position, repaint -- only when its content changed (or force).
void RefreshOverlay(bool force) {
  if (!g_overlay.hwnd) return;
  EnterCriticalSection(&g_window.lock);
  DWORD pid = g_window.snapshot.pid;
  std::string key = OverlayContentKey(g_window.snapshot);
  int lines = static_cast<int>(OverlayLines(g_window.snapshot, WindowTheme()).size());
  LeaveCriticalSection(&g_window.lock);
  EnsureOverlayGameHooks(pid);
  g_overlay.lineCount = lines;  // 0 (every line switched off) hides the panel
  RepositionOverlay();
  if (force || key != g_lastOverlayContentKey) {
    g_lastOverlayContentKey = key;
    InvalidateRect(g_overlay.hwnd, nullptr, FALSE);
  }
}

// Tier set from the live page (POST /api/tier): page server -> host window.
constexpr UINT kMsgSpoilerTierSet = WM_APP + 5;
// Overlay settings changed from the live page (POST /api/overlay).
constexpr UINT kMsgOverlaySettings = WM_APP + 6;

// Font size, wrap length and opacity from the settings (UI thread).
void ApplyOverlayLook() {
  const double scale = g_window.scale;
  if (g_overlay.fontPx != g_ov.text || !g_window.font) {
    if (g_window.font) DeleteObject(g_window.font);
    g_window.font = CreateFontW(-static_cast<int>(g_ov.text * scale + 0.5), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                FIXED_PITCH | FF_MODERN, L"Consolas");
    g_overlay.fontPx = g_ov.text;
  }
  // Consolas is fixed-pitch: one character's width sets the wrap length.
  HDC dc = GetDC(nullptr);
  HGDIOBJ old = SelectObject(dc, g_window.font);
  SIZE em{};
  GetTextExtentPoint32W(dc, L"M", 1, &em);
  SelectObject(dc, old);
  ReleaseDC(nullptr, dc);
  if (em.cx > 0) g_overlay.charsPerLine = static_cast<int>((g_ov.width - 24.0) * scale / em.cx);
  if (g_overlay.hwnd)
    SetLayeredWindowAttributes(g_overlay.hwnd, kOverlayColorKey, static_cast<BYTE>(g_ov.opacity * 255 / 100),
                               LWA_COLORKEY | LWA_ALPHA);
}

// Messages for the hidden host window (hotkeys, snapshots, the page).
void RefreshMainWindow();  // the app window (WASD.exe), defined after the map-data section

bool HandleSharedMessage(HWND, UINT msg) {
  switch (msg) {
    case kMsgSnapshot:
      RefreshOverlay(false);
      RefreshMainWindow();
      return true;
    case kMsgTogglePause:
      InterlockedExchange(&g_window.paused, g_window.paused ? 0 : 1);
      SetEvent(g_window.wakeEvent);
      RepositionOverlay();  // hides it while paused
      return true;
    case kMsgCycleSpoilerTier:
      Log("Spoiler tier: %s (F9).", SpoilerTierName(CycleSpoilerTier()));
      SetEvent(g_window.wakeEvent);  // worker refreshes the hint lines now
      RefreshOverlay(true);
      return true;
    case kMsgSpoilerTierSet:
      Log("Spoiler tier: %s (live page).", SpoilerTierName(SpoilerTier()));
      SetEvent(g_window.wakeEvent);
      RefreshOverlay(true);
      return true;
    case kMsgTogglePerfHud:
      InterlockedExchange(&g_ov.perf, g_ov.perf ? 0 : 1);
      SaveSettings();
      Log("Overlay perf figures %s (F11).", g_ov.perf ? "shown" : "hidden");
      RefreshOverlay(true);  // height changes
      return true;
    case kMsgOverlaySettings:
      ApplyOverlayLook();
      RefreshOverlay(true);
      return true;
  }
  return false;
}

bool StartLiveServer(int* portOut);
void StopLiveServer();
bool LivePageRecentlyPolled();
wchar_t g_liveUrl[64] = L"";
constexpr UINT_PTR kOpenPageTimerId = 1;

// The overlay's owner is a hidden message target (there's no visible
// window to close); Ctrl+C reaches it via the console handler (WM_CLOSE
// posted to it).
constexpr UINT kMsgMapData = WM_APP + 7;       // map-data thread -> host window: finished
constexpr UINT kMsgShowYourself = WM_APP + 8;  // a second WASD.exe -> the app window: come to the front

LRESULT CALLBACK OverlayHostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (HandleSharedMessage(hwnd, msg)) return 0;
  if (msg == kMsgMapData) {
    RefreshMainWindow();
    return 0;
  }
  if (msg == WM_TIMER && wParam == kOpenPageTimerId) {
    KillTimer(hwnd, kOpenPageTimerId);
    if (EnvVar(L"WASD_NO_BROWSER") == L"1") {  // tests
      Log("Not opening the live page (WASD_NO_BROWSER).");
    } else if (LivePageRecentlyPolled()) {
      Log("Live page already open in a browser -- not opening another tab.");
    } else {
      Log("Opening the live page in your browser.");
      ShellExecuteW(nullptr, L"open", g_liveUrl, nullptr, nullptr, SW_SHOWNOACTIVATE);
    }
    return 0;
  }
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

BOOL WINAPI OnWindowModeConsoleCtrl(DWORD ctrlType) {
  if (ctrlType != CTRL_C_EVENT && ctrlType != CTRL_BREAK_EVENT && ctrlType != CTRL_CLOSE_EVENT)
    return FALSE;
  PostMessageW(g_window.hwnd, WM_CLOSE, 0, 0);
  return TRUE;
}


// The Milestone 4 HUD over the game plus the live page server, sharing
// one polling worker and one hotkey hook. (The Milestone 3 companion
// window was replaced by the live page, 2026-10-06.)
void EnsureMapDataAtStart();  // defined with the map-data section
void CreateMainWindow(HINSTANCE instance);
void StartFirstRunMapData();
HWND g_mainWindow = nullptr;  // the app window (WASD.exe only)

// app: WASD.exe -- adds the app window, reads first-run map data in the
// background instead of before starting, and has no console to handle.
int RunOverlay(bool app = false) {
  if (!app) EnsureMapDataAtStart();  // first run: read the map data
  SetProcessDPIAware();
  InitializeCriticalSection(&g_window.lock);
  g_window.wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HINSTANCE instance = GetModuleHandleW(nullptr);

  HDC screen = GetDC(nullptr);
  double scale = GetDeviceCaps(screen, LOGPIXELSY) / 96.0;
  ReleaseDC(nullptr, screen);
  g_window.scale = scale;
  ApplyOverlayLook();  // font + wrap length from the saved settings
  InitializeCriticalSection(&g_trail.lock);
  {
    WindowSnapshot starting;
    starting.status = "Attaching to Dark Souls III...";
    g_window.snapshot = starting;
  }

  {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = OverlayHostProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"WasdOverlayHost";
    RegisterClassExW(&wc);
    g_window.hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                    nullptr, instance, nullptr);
  }
  if (!g_window.hwnd) {
    Log("Couldn't create the window (error %lu).", GetLastError());
    return 4;
  }

  {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = OverlayWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kOverlayClassName;
    RegisterClassExW(&wc);
    g_overlay.hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kOverlayClassName, L"WASD overlay", WS_POPUP, 0, 0,
        static_cast<int>(g_ov.width * scale + 0.5), OverlayHeightPx(), nullptr, nullptr,
        instance, nullptr);
    if (!g_overlay.hwnd) {
      Log("Couldn't create the overlay (error %lu).", GetLastError());
      return 4;
    }
    ApplyOverlayLook();  // opacity, now that the window exists
    g_overlay.foregroundHook =
        SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, nullptr,
                        OnOverlayWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_perfHudHotkeyEnabled = true;
  }
  if (app) {
    CreateMainWindow(instance);
    if (!g_mainWindow) {
      Log("Couldn't create the app window (error %lu).", GetLastError());
      return 4;
    }
    StartFirstRunMapData();
  } else {
    SetConsoleCtrlHandler(OnWindowModeConsoleCtrl, TRUE);
  }

  // F10 (and F11 in overlay mode) hook, delivering to the target window.
  g_hotkeyTargetWindow = g_window.hwnd;
  HotkeyThreadState hotkeyState;
  hotkeyState.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  DWORD hotkeyThreadId = 0;
  HANDLE hotkeyThread =
      CreateThread(nullptr, 0, HotkeyThreadMain, &hotkeyState, 0, &hotkeyThreadId);
  if (hotkeyThread) WaitForSingleObject(hotkeyState.ready, INFINITE);
  CloseHandle(hotkeyState.ready);
  if (!hotkeyThread || !hotkeyState.hooked) {
    Log("WARNING: couldn't install the keyboard hook -- no F10/F11 this run.");
  }

  HANDLE worker = CreateThread(nullptr, 0, WindowPollWorker, nullptr, 0, nullptr);
  Log("%s %s", WASD_APP_NAME, WASD_VERSION_STRING);
  Log("Your data: %ls (%ls)", Paths().user.c_str(), Paths().userSource.c_str());
  Log("Overlay running over the game window's top-right (game must be windowed or borderless). "
      "F10 hides it + pauses polling, F11 shows/hides perf figures, F9 cycles the spoiler tier. %s",
      app ? "Close the WASD window to exit." : "Ctrl+C to exit.");
  StartCoopSessionThread();  // hosts the relay if host_relay=1; pairs by the Seamless password, or waits for the page
  int port = 0;
  if (StartLiveServer(&port)) {
    std::swprintf(g_liveUrl, 64, L"http://localhost:%d/", port);
    Log("Live page: %ls (this PC only).", g_liveUrl);
    if (g_hostRelay) Log("Co-op relay: hosted on this PC, 127.0.0.1:%d (host_relay in settings.ini).", g_relayPort);
    else if (!RelayUrlSetting().empty()) Log("Co-op relay: %s (relay_url in settings.ini).", RelayUrlSetting().c_str());
    RefreshMainWindow();
    // A page left open from an earlier run reconnects within a second or so
    // (Chrome slows timers in background tabs to once a second);
    // only open a new tab if none has asked for data by then.
    SetTimer(g_window.hwnd, kOpenPageTimerId, 2500, nullptr);
  }

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  InterlockedExchange(&g_window.stop, 1);
  SetEvent(g_window.wakeEvent);
  if (worker) {
    WaitForSingleObject(worker, 3000);
    CloseHandle(worker);
  }
  if (hotkeyThread) {
    PostThreadMessageW(hotkeyThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(hotkeyThread, 2000);
    CloseHandle(hotkeyThread);
  }
  StopLiveServer();
  StopCoopSessionThread();
  if (g_overlay.foregroundHook) UnhookWinEvent(g_overlay.foregroundHook);
  if (g_overlay.locationHook) UnhookWinEvent(g_overlay.locationHook);
  if (g_overlay.hwnd) DestroyWindow(g_overlay.hwnd);
  if (g_mainWindow) DestroyWindow(g_mainWindow);
  g_mainWindow = nullptr;
  CloseHandle(g_window.wakeEvent);
  DeleteObject(g_window.font);
  // (Each game run logs its own session summary when it ends.)
  DeleteCriticalSection(&g_window.lock);
  DeleteCriticalSection(&g_trail.lock);
  Log("Closed.");
  return 0;
}


