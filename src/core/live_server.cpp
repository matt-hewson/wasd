// src/core/live_server.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

struct LiveServer {
  SOCKET listener = INVALID_SOCKET;
  HANDLE thread = nullptr;
  int port = 0;
  bool wsa = false;
  volatile ULONGLONG lastStatePoll = 0;  // GetTickCount64 of the last /api/state request
  volatile ULONGLONG previewUntil = 0;   // overlay preview (settings panel open) until this tick
};
LiveServer g_live;

// The page renews the preview every 1.5s while its overlay settings panel
// is open; if it stops (tab closed, page gone) the preview lapses in 4s.
bool OverlayPreviewActive() { return GetTickCount64() < g_live.previewUntil; }

bool LivePageRecentlyPolled() {
  ULONGLONG last = g_live.lastStatePoll;
  return last != 0 && GetTickCount64() - last < 2500;
}

void SendAll(SOCKET c, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    int n = send(c, data.data() + sent, static_cast<int>((std::min)(data.size() - sent, size_t{1} << 20)), 0);
    if (n <= 0) return;
    sent += static_cast<size_t>(n);
  }
}

void SendResponse(SOCKET c, const char* status, const char* type, const std::string& body) {
  std::string head = std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type +
                     "\r\nContent-Length: " + std::to_string(body.size()) +
                     "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
  SendAll(c, head + body);
}

// Where a player is, by name (area, region, bonfire), and the area the
// route enters theirs from: what CoopPlaceView filters by the tier.
struct CoopPlaceNames {
  std::string area, region, bonfire, from;
};
CoopPlaceNames CoopPlaceNamesOf(const SharedGameState& s) {
  static const AreaInfo areaInfo;
  static const std::vector<RouteStep> route = LoadRoute();
  const AreaNames names;
  CoopPlaceNames n;
  n.area = areaInfo.AreaOf(s.playRegionId);
  n.region = s.playRegionId ? names.Region(s.playRegionId) : "";
  n.bonfire = s.lastBonfireId > 0 ? names.Bonfire(s.lastBonfireId) : "";
  for (const auto& st : route)
    if (st.area == n.area && st.from != "-") {
      n.from = st.from;
      break;
    }
  return n;
}

// A query parameter as the page sent it (encodeURIComponent): %XX and + decoded.
std::string UrlDecode(const std::string& v) {
  auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == '+') out += ' ';
    else if (v[i] == '%' && i + 2 < v.size() && hex(v[i + 1]) >= 0 && hex(v[i + 2]) >= 0)
      out += static_cast<char>(hex(v[i + 1]) * 16 + hex(v[i + 2])), i += 2;
    else out += v[i];
  }
  return out;
}

// /api/state's "coop": the live session and the partner, at the tier.
std::string LiveCoopJson() {
  CoopSessionView view;
  CoopLivePartner partner;
  g_coopSession.Read(&view, &partner);
  SharedGameState mine;
  CoopLists lists;
  bool haveMine = false;
  if (partner.have) {
    RefreshMyCoopRecord();  // the areas you've reached include your recorded ones
    std::lock_guard<std::mutex> guard(g_coop.lock);
    mine = g_coop.mine;
    lists = g_coop.lists;
    haveMine = !mine.characterName.empty();
  }
  std::vector<uint32_t> bossFlags = lists.bossFlags;
  if (bossFlags.empty())
    for (const auto& b : Bosses()) bossFlags.push_back(b.flag);
  static const AreaInfo areaInfo;
  const CoopPlaceNames place = partner.have ? CoopPlaceNamesOf(partner.state) : CoopPlaceNames();
  return CoopLiveJson(view, CoopRelayInfoNow(), partner, mine, haveMine, lists, bossFlags, CoopAreaList(),
                      haveMine ? areaInfo.AreaOf(mine.playRegionId) : "", place.area, place.region, place.bonfire,
                      place.from, static_cast<uint32_t>(std::time(nullptr)), SpoilerTier());
}

void HandleLiveRequest(SOCKET c) {
  std::string req;
  char buf[4096];
  while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
    int n = recv(c, buf, sizeof(buf), 0);
    if (n <= 0) return;
    req.append(buf, static_cast<size_t>(n));
  }
  size_t sp1 = req.find(' '), sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
  if (sp2 == std::string::npos) return;
  std::string method = req.substr(0, sp1), target = req.substr(sp1 + 1, sp2 - sp1 - 1);
  // Host must be this machine's name for the page (DNS-rebinding guard).
  std::string host;
  size_t h = req.find("\r\nHost:");
  if (h == std::string::npos) h = req.find("\r\nhost:");
  if (h != std::string::npos) {
    size_t e = req.find("\r\n", h + 2);
    host = req.substr(h + 7, e - h - 7);
    host.erase(0, host.find_first_not_of(' '));
  }
  std::string port = ":" + std::to_string(g_live.port);
  if (host != "localhost" + port && host != "127.0.0.1" + port) {
    SendResponse(c, "403 Forbidden", "text/plain", "localhost only");
    return;
  }
  std::string path = target.substr(0, target.find('?'));
  std::string query = target.find('?') == std::string::npos ? "" : target.substr(target.find('?') + 1);
  auto param = [&](const char* name) -> std::string {
    std::string key = std::string(name) + "=";
    size_t p = ("&" + query).find("&" + key);
    if (p == std::string::npos) return "";
    size_t start = p + key.size();
    return query.substr(start, query.find('&', start) - start);
  };
  if (method == "GET" && (path == "/" || path == "/index.html")) {
    std::ifstream in(Paths().Template(L"live.html"), std::ios::binary);
    if (!in) {
      SendResponse(c, "404 Not Found", "text/plain", "templates/live.html not found");
      return;
    }
    std::string page((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    SendResponse(c, "200 OK", "text/html; charset=utf-8", page);
  } else if (method == "GET" && path == "/results") {
    // The results page (as `report` builds it), built fresh from the saved
    // sessions; served here so its co-op card can swap codes.
    std::string page = ResultsPageHtml(nullptr, nullptr);
    if (page.empty())
      SendResponse(c, "404 Not Found", "text/plain", "templates/results_template.html not found");
    else
      SendResponse(c, "200 OK", "text/html; charset=utf-8", page);
  } else if (method == "GET" && path == "/api/sessions") {
    // The saved sessions for the results page to redraw from as you play:
    // {"stamp", "sessions"}, or only {"stamp"} when `have` is already the
    // stamp (nothing recorded since), so polling reads no files.
    std::string stamp = SessionsStamp();
    if (param("have") == stamp)
      SendResponse(c, "200 OK", "application/json", "{\"stamp\":\"" + stamp + "\"}");
    else
      SendResponse(c, "200 OK", "application/json", "{\"stamp\":\"" + stamp + "\",\"sessions\":" + SessionsJson(nullptr, nullptr) + "}");
  } else if (method == "GET" && path == "/api/state") {
    g_live.lastStatePoll = GetTickCount64();
    WindowSnapshot snap;
    EnterCriticalSection(&g_window.lock);
    snap = g_window.snapshot;
    LeaveCriticalSection(&g_window.lock);
    std::string from = param("trail");
    std::string json = BuildLiveJson(snap, from.empty() ? 0 : static_cast<size_t>(std::strtoull(from.c_str(), nullptr, 10)));
    json.insert(json.size() - 1, ",\"coop\":" + LiveCoopJson());
    SendResponse(c, "200 OK", "application/json", json);
  } else if (method == "POST" && path == "/api/tier") {
    std::string t = param("t");
    if (t.size() != 1 || t[0] < '0' || t[0] > '3') {
      SendResponse(c, "400 Bad Request", "text/plain", "t must be 0-3");
      return;
    }
    InterlockedExchange(&g_spoilerTier, t[0] - '0');
    SaveSettings();
    PostMessageW(g_window.hwnd, kMsgSpoilerTierSet, 0, 0);
    SendResponse(c, "200 OK", "application/json", "{\"tier\":" + t + "}");
  } else if (method == "POST" && path == "/api/missable") {
    // ?id=<missable id>&state=done|ignore|clear -- the player's own mark, per character
    std::string id = param("id"), state = param("state");
    EnterCriticalSection(&g_window.lock);
    std::string character = g_window.snapshot.characterName;
    LeaveCriticalSection(&g_window.lock);
    bool ok = !character.empty() && SetMissableMark(character, id, state);
    if (ok) SetEvent(g_window.wakeEvent);
    SendResponse(c, ok ? "200 OK" : "400 Bad Request", "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  } else if (method == "POST" && path == "/api/page") {
    // ?theme=a|b|c and/or ?muted=char,map,... (the whole list; empty = none muted)
    std::string theme = param("theme");
    bool hasMuted = ("&" + query).find("&muted=") != std::string::npos;  // sent unencoded: a,b,c
    bool ok = !theme.empty() || hasMuted;
    if (!theme.empty()) ok = theme.size() == 1 && theme[0] >= 'a' && theme[0] <= 'c';
    if (ok && hasMuted) ok = SetPageMuted(param("muted"));
    if (ok && !theme.empty()) InterlockedExchange(&g_pageTheme, theme[0] - 'a');
    if (ok) SaveSettings();
    SendResponse(c, ok ? "200 OK" : "400 Bad Request", "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  } else if (method == "POST" && path == "/api/quit") {
    // The page's Quit button: same clean shutdown as Ctrl+C (session saved).
    SendResponse(c, "200 OK", "application/json", "{\"quitting\":true}");
    Log("Quit requested from the live page.");
    PostMessageW(g_window.hwnd, WM_CLOSE, 0, 0);
  } else if (method == "POST" && path == "/api/preview") {
    bool on = param("on") == "1";
    bool was = OverlayPreviewActive();
    g_live.previewUntil = on ? GetTickCount64() + 4000 : 0;
    if (on != was) PostMessageW(g_window.hwnd, kMsgOverlaySettings, 0, 0);  // show/hide now
    SendResponse(c, "200 OK", "application/json", on ? "{\"preview\":true}" : "{\"preview\":false}");
  } else if (method == "POST" && path == "/api/overlay") {
    // ?key=value[&key=value...]: overlay settings (validated; all or nothing reported)
    bool ok = !query.empty();
    for (size_t pos = 0; ok && pos < query.size();) {
      size_t amp = query.find('&', pos);
      std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
      size_t eq = pair.find('=');
      ok = eq != std::string::npos && SetOverlaySetting(pair.substr(0, eq), pair.substr(eq + 1));
      pos = amp == std::string::npos ? query.size() : amp + 1;
    }
    SaveSettings();
    PostMessageW(g_window.hwnd, kMsgOverlaySettings, 0, 0);
    SendResponse(c, ok ? "200 OK" : "400 Bad Request", "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  } else if (method == "POST" && path == "/api/coop/session") {
    // ?do=create | do=join&code=K7DP4X | do=leave | do=pair (auto pairing
    // back on): the live co-op session. The session thread acts on it
    // within 0.1 s; the page sees the result in /api/state's "coop".
    std::string action = param("do"), error;
    if (action == "create") error = g_coopSession.Create();
    else if (action == "join") error = g_coopSession.Join(param("code"));
    else if (action == "leave") g_coopSession.Leave();
    else if (action == "pair") g_coopSession.Pair();
    else error = "do must be create, join, leave or pair";
    SendResponse(c, error.empty() ? "200 OK" : "400 Bad Request", "application/json",
                 error.empty() ? "{\"ok\":true}" : "{\"ok\":false,\"error\":" + JsonString(error) + "}");
  } else if (method == "POST" && path == "/api/coop/forward") {
    // ?on=1 (the default) | on=0: while hosting, has Tailscale forward the
    // relay's port, or stop (tailscale serve, run by the session thread).
    // /api/state's coop.host.tailscale shows "serving", or "forwardError".
    std::string error = g_coopSession.Forward(param("on") != "0");
    SendResponse(c, error.empty() ? "200 OK" : "400 Bad Request", "application/json",
                 error.empty() ? "{\"ok\":true}" : "{\"ok\":false,\"error\":" + JsonString(error) + "}");
  } else if (method == "POST" && path == "/api/coop/relay") {
    // ?url=<the host's address, or their relay_url line> (empty clears it):
    // the relay to join, saved as relay_url. A session running ends and pairs again.
    std::string error = SetPartnerRelay(UrlDecode(param("url")));
    SendResponse(c, error.empty() ? "200 OK" : "400 Bad Request", "application/json",
                 error.empty() ? "{\"ok\":true}" : "{\"ok\":false,\"error\":" + JsonString(error) + "}");
  } else if (method == "POST" && path == "/api/coop/host") {
    // ?on=1 | on=0: host the co-op relay on this PC, or stop (saved in
    // settings.ini as host_relay). A session running ends and pairs again.
    std::string on = param("on"), error;
    if (on == "1" || on == "0") error = SetHostRelay(on == "1");
    else error = "on must be 1 or 0";
    SendResponse(c, error.empty() ? "200 OK" : "400 Bad Request", "application/json",
                 error.empty() ? "{\"ok\":true}" : "{\"ok\":false,\"error\":" + JsonString(error) + "}");
  } else if (method == "GET" && path == "/api/coop/code") {
    // This player's state as a code to send the partner (a FullSnapshot),
    // with the recorded part summed from the saved sessions.
    RefreshMyCoopRecord();
    std::string code, leftOut, name;
    {
      std::lock_guard<std::mutex> guard(g_coop.lock);
      if (g_coop.mineReady) code = FitCoopCode(g_coop.mine, ++g_coop.sequence, &leftOut);
      name = g_coop.mine.characterName;
    }
    if (code.empty())
      SendResponse(c, "409 Conflict", "application/json",
                   "{\"ok\":false,\"error\":\"Not ready yet: load into the game and keep this page open for a few "
                   "seconds.\"}");
    else
      SendResponse(c, "200 OK", "application/json",
                   "{\"ok\":true,\"code\":" + JsonString(code) + ",\"leftOut\":" + JsonString(leftOut) +
                       ",\"name\":" + JsonString(name) + "}");
  } else if (method == "GET" && path == "/api/coop") {
    // The results page's comparison of both players. Polling it counts as
    // the page being open, so your flag sets keep being read.
    g_live.lastStatePoll = GetTickCount64();
    RefreshMyCoopRecord();
    SharedGameState mine, partner;
    CoopLists lists;
    bool mineReady = false, havePartner = false, live = false;
    {
      std::lock_guard<std::mutex> guard(g_coop.lock);
      mine = g_coop.mine;
      partner = g_coop.partner;
      lists = g_coop.lists;
      mineReady = g_coop.mineReady;
      havePartner = g_coop.havePartner;
      live = g_coop.partnerLive;
    }
    // Before your own flags are read (not in the game yet), the boss list
    // comes straight from bosses.tsv: the same list, in the same order.
    if (lists.bossFlags.empty()) {
      for (const auto& b : Bosses()) {
        lists.bossNames.push_back(b.name);
        lists.bossAreas.push_back(b.area);
        lists.bossFlags.push_back(b.flag);
      }
    }
    static const AreaInfo areaInfo;
    const AreaNames names;
    const int tier = SpoilerTier();
    CoopCompare cmp = BuildCoopCompare(mine, !mine.characterName.empty(), partner, havePartner, lists,
                                       CoopAreaList(), areaInfo.AreaOf(mine.playRegionId),
                                       static_cast<uint32_t>(std::time(nullptr)), tier);
    // Where each of you is, by name; your partner's at the tier, as the live
    // page's route hints show places (at Category: "a new area beyond ...").
    static const std::vector<RouteStep> route = LoadRoute();
    auto fromOf = [&](const std::string& area) {
      for (const auto& st : route)
        if (st.area == area && st.from != "-") return st.from;
      return std::string();
    };
    auto place = [&](CoopPlayerView* v, const SharedGameState& s, const std::set<std::string>* reached) {
      const std::string area = areaInfo.AreaOf(s.playRegionId);
      CoopPlaceView(v, area, s.playRegionId ? names.Region(s.playRegionId) : "",
                    s.lastBonfireId > 0 ? names.Bonfire(s.lastBonfireId) : "", reached, tier, fromOf(area));
    };
    place(&cmp.me, mine, nullptr);
    place(&cmp.partner, partner, &cmp.reached);
    // The pair report: both players' recorded play, filtered the same way.
    CoopPair pair = BuildCoopPair(mine, !mine.characterName.empty(), partner, havePartner, lists, CoopAreaList(),
                                  cmp.reached, [](int32_t id) {
                                    const auto& table = ItemNames().weapon;
                                    auto it = table.find(std::to_string(id));
                                    return it == table.end() ? std::string() : it->second;
                                  }, tier);
    std::string json = CoopCompareJson(cmp, mineReady, havePartner, partner.characterName, tier);
    json.insert(json.size() - 1, ",\"pair\":" + CoopPairJson(pair) + ",\"live\":" + (live ? "true" : "false"));
    SendResponse(c, "200 OK", "application/json", json);
  } else if (method == "POST" && path == "/api/coop/partner") {
    // ?code=WASD-... (the partner's code, whitespace already removed) or ?clear=1.
    // While a co-op session is on, its next update replaces either.
    if (param("clear") == "1") {
      {
        std::lock_guard<std::mutex> guard(g_coop.lock);
        g_coop.havePartner = g_coop.partnerLive = false;
        g_coop.partner = SharedGameState();
      }
      Log("Co-op: partner code cleared.");
      SendResponse(c, "200 OK", "application/json", "{\"ok\":true}");
      return;
    }
    CoopPacket p = CoopDecodeText(param("code"));
    if (!p.ok) {
      Log("Co-op: partner code refused: %s", p.error.c_str());
      SendResponse(c, "400 Bad Request", "application/json", "{\"ok\":false,\"error\":" + JsonString(p.error) + "}");
      return;
    }
    bool own;
    {
      std::lock_guard<std::mutex> guard(g_coop.lock);
      own = g_coop.mineReady && CoopIsOwnCode(g_coop.mine, p.state);
      if (!own) g_coop.partner = p.state, g_coop.havePartner = true, g_coop.partnerLive = false;
    }
    if (own) {
      std::string error = "that's your own code (" + p.state.characterName + "): paste your partner's";
      Log("Co-op: partner code refused: %s", error.c_str());
      SendResponse(c, "400 Bad Request", "application/json", "{\"ok\":false,\"error\":" + JsonString(error) + "}");
      return;
    }
    Log("Co-op: partner code loaded (%s, level %u, sequence %u).", p.state.characterName.c_str(), p.state.level,
        p.sequence);
    SendResponse(c, "200 OK", "application/json", "{\"ok\":true,\"name\":" + JsonString(p.state.characterName) + "}");
  } else {
    SendResponse(c, "404 Not Found", "text/plain", "not found");
  }
}

DWORD WINAPI LiveServerMain(LPVOID) {
  while (!g_window.stop) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(g_live.listener, &set);
    timeval tv{0, 250000};  // wake 4x a second to notice shutdown
    if (select(0, &set, nullptr, nullptr, &tv) <= 0) continue;
    SOCKET c = accept(g_live.listener, nullptr, nullptr);
    if (c == INVALID_SOCKET) continue;
    DWORD timeoutMs = 2000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    HandleLiveRequest(c);
    closesocket(c);
  }
  return 0;
}

bool StartLiveServer(int* portOut) {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    Log("Live page: Winsock unavailable -- page disabled.");
    return false;
  }
  g_live.wsa = true;
  const int first = g_livePort, last = g_livePort + kLivePortSpan - 1;
  for (int port = first; port <= last; ++port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) break;
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1: this PC only
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && listen(s, 8) == 0) {
      g_live.listener = s;
      g_live.port = port;
      break;
    }
    closesocket(s);
  }
  if (g_live.listener == INVALID_SOCKET) {
    Log("Live page: no free port in %d-%d -- page disabled (live_port in settings.ini).", first, last);
    return false;
  }
  g_live.thread = CreateThread(nullptr, 0, LiveServerMain, nullptr, 0, nullptr);
  *portOut = g_live.port;
  return g_live.thread != nullptr;
}

// Call after g_window.stop is set: the server thread notices within 250ms.
void StopLiveServer() {
  if (g_live.thread) {
    WaitForSingleObject(g_live.thread, 2000);
    CloseHandle(g_live.thread);
    g_live.thread = nullptr;
  }
  if (g_live.listener != INVALID_SOCKET) closesocket(g_live.listener);
  g_live.listener = INVALID_SOCKET;
  if (g_live.wsa) WSACleanup();
  g_live.wsa = false;
}

