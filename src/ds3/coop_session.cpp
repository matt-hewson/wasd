// src/ds3/coop_session.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- co-op session: the live page's Create / Join / Disconnect ---------
//
// One session at a time, for the live page (overlay mode). Its own thread
// owns the CoopLink: it ticks the link at the polling rate with this
// player's state (g_coop.mine, once a character has been read), keeps the
// partner's recent path, and ends the session when the relay says it's
// over, on another protocol version, or once the partner has been gone for
// 5 minutes. The partner's state also becomes the results page's partner
// (g_coop.partner) as it arrives, so its comparison follows the session.
// The page server only queues commands (create, join, leave, pair) and
// reads what the thread last published, so a slow relay never holds up a
// page request.
//
// Auto pairing: both players already share Seamless Co-op's session
// password (cooppassword in SeamlessCoop/ds3sc_settings.ini). With no
// session running, the thread reads it every 5 s and joins the pairing
// room named by its hash (CoopPairKey), so two copies with the same
// password and relay find each other with no code. The password never
// leaves the PC; the relay only sees the hash. A pairing room stays open
// while the partner is away. Disconnect, or a session by code, pauses auto
// pairing until "Pair automatically" (or the next start).

constexpr ULONGLONG kCoopGoneLimitMs = 5 * 60 * 1000;  // partner away this long: the session ends
constexpr ULONGLONG kCoopTrailMs = 30 * 1000;          // the partner's path on the map: the last 30 s
constexpr size_t kCoopTrailMax = 300;
constexpr ULONGLONG kCoopPairCheckMs = 5000;           // how often the Seamless password is read
constexpr ULONGLONG kCoopPairRetryMs = 60 * 1000;      // after a pairing room ended on an error
constexpr ULONGLONG kCoopTailscaleCheckMs = 30 * 1000;  // while hosting: what Tailscale says

// Seamless Co-op's session password from its settings file's text
// ("cooppassword = ..." under [PASSWORD]; spaces around it trimmed); "" if none.
std::string SeamlessPasswordFromIni(const std::string& text) {
  std::string section, line;
  for (size_t at = 0; at <= text.size();) {
    size_t end = text.find('\n', at);
    line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
    at = end == std::string::npos ? text.size() + 1 : end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
    size_t lead = line.find_first_not_of(" \t");
    if (lead == std::string::npos || line[lead] == ';' || line[lead] == '#') continue;
    line.erase(0, lead);
    if (line[0] == '[') {
      section = line;
      for (char& c : section) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos || section != "[PASSWORD]") continue;
    std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    if (_stricmp(key.c_str(), "cooppassword") != 0) continue;
    size_t v = value.find_first_not_of(" \t");
    return v == std::string::npos ? "" : value.substr(v);
  }
  return "";
}

// The password from the game folder's Seamless Co-op settings ("" when
// there's no game folder, no file or no password).
std::string ReadSeamlessPassword(const std::string& gameDir) {
  if (gameDir.empty()) return "";
  std::ifstream in(WidenUtf8(gameDir) + L"\\SeamlessCoop\\ds3sc_settings.ini", std::ios::binary);
  if (!in) return "";
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return SeamlessPasswordFromIni(text.substr(0, 64 * 1024));
}

// The pairing room's key for a password: hex SHA-256 of a fixed prefix and
// the password ("" for no password). Same password, same room.
std::string CoopPairKey(const std::string& password) {
  if (password.empty()) return "";
  return HexBytes(HashBytes(BCRYPT_SHA256_ALGORITHM, "wasd-coop-pair-1\n" + password));
}

// What the page shows about the session, from the relay's status and what
// the session remembers.
//   state: none · creating (no code yet) · hosting (your code, no partner
//     yet) · joining (their code, not in yet) · pairing (the pairing room,
//     no partner yet) · connected · reconnecting (the partner, or you,
//     dropped after you were paired)
//   error: "" · wrong-code · room-full · version · relay (can't reach it) ·
//     ended (the session closed) · no-relay (no relay hosted or set)
struct CoopSessionView {
  std::string state = "none";
  std::string error, errorText;  // errorText: the detail, for the log and the popover
  std::string code;
  bool paired = false;      // a pairing room (no code)
  int64_t openSeconds = 0;  // since the session was started
  int64_t goneSeconds = 0;  // reconnecting: since the partner was last here
  int rttMs = -1;           // to the relay; -1 unknown
  int64_t lastChangeMs = -1;  // since the partner's state last changed; -1 never
  double updatesPerSecond = 0;
  int partnerProtocol = 0;  // after a version mismatch
};

// Pure: the state of a running session. `everPartner`: a partner has been
// here this session; `goneSince`: when they (or the link) last dropped, 0
// while they're here.
CoopSessionView DeriveCoopSession(const RelayStatus& r, bool everPartner, ULONGLONG startedTick, ULONGLONG goneSince,
                                  ULONGLONG partnerUpdatedTick, ULONGLONG now) {
  CoopSessionView v;
  v.code = r.code;
  v.paired = r.paired;
  v.openSeconds = startedTick && now > startedTick ? static_cast<int64_t>((now - startedTick) / 1000) : 0;
  v.rttMs = r.rttMs;
  v.lastChangeMs = !partnerUpdatedTick ? -1 : now > partnerUpdatedTick ? static_cast<int64_t>(now - partnerUpdatedTick) : 0;
  const char* alone = r.paired ? "pairing" : r.created ? "hosting" : "joining";
  switch (r.state) {
    case RelayState::kOff: v.state = "none"; break;
    case RelayState::kCreating: v.state = "creating"; break;
    case RelayState::kConnecting: v.state = alone; break;
    case RelayState::kPaired: v.state = "connected"; break;
    case RelayState::kWaiting:
    case RelayState::kReconnecting: v.state = everPartner ? "reconnecting" : alone; break;
    case RelayState::kEnded:
    case RelayState::kFailed: v.state = "none"; break;
  }
  if (v.state == "reconnecting") {
    v.goneSeconds = goneSince && now > goneSince ? static_cast<int64_t>((now - goneSince) / 1000) : 0;
    v.rttMs = -1;
  }
  // Still trying to get in for the first time and the relay can't be reached.
  if (!r.everIn && r.state == RelayState::kReconnecting && r.problem == RelayProblem::kUnreachable) {
    v.error = "relay";
    v.errorText = "couldn't reach the relay; trying again";
  }
  return v;
}

// Pure: whether a running session is over, and how to say why.
struct CoopSessionEnd {
  bool over = false;
  std::string error, text;
};
CoopSessionEnd CoopSessionEndFor(const RelayStatus& r, bool mismatch, int partnerVersion, ULONGLONG goneSince,
                                 ULONGLONG now) {
  CoopSessionEnd e;
  if (mismatch) {
    e.over = true, e.error = "version";
    e.text = "your partner's WASD speaks protocol " + std::to_string(partnerVersion) + "; yours speaks " +
             std::to_string(kCoopProtocolVersion);
    return e;
  }
  if (r.state == RelayState::kFailed || r.state == RelayState::kEnded) {
    e.over = true, e.text = r.message;
    switch (r.problem) {
      case RelayProblem::kBadCode: e.error = "wrong-code"; break;
      case RelayProblem::kNoSession: e.error = r.everIn ? "ended" : "wrong-code"; break;
      case RelayProblem::kFull: e.error = "room-full"; break;
      case RelayProblem::kUnreachable: e.error = "relay"; break;
      default: e.error = r.state == RelayState::kEnded ? "ended" : "relay"; break;
    }
    return e;
  }
  // A pairing room waits for the partner however long they're away.
  if (!r.paired && goneSince && now > goneSince && now - goneSince >= kCoopGoneLimitMs) {
    e.over = true, e.error = "ended";
    e.text = "your partner was away for 5 minutes";
  }
  return e;
}

// A 6-character session code as typed: upper case, letters and digits only
// ("" if that doesn't leave 6). The relay checks the rest.
std::string CoopNormalizeCode(const std::string& typed) {
  std::string code;
  for (char c : typed)
    if (std::isalnum(static_cast<unsigned char>(c))) code += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return code.size() == 6 ? code : "";
}

struct CoopTrailPoint {
  float x = 0, z = 0;
  int mapKey = 0;
  ULONGLONG tick = 0;
};

// Adds the partner's position to their path when they've moved half a metre
// (or changed map), and drops what's older than kCoopTrailMs.
void CoopTrailAdd(std::vector<CoopTrailPoint>* trail, const SharedGameState& s, ULONGLONG now) {
  if (s.havePose) {
    CoopTrailPoint p{s.x, s.z, CoopMapKey(s.mapId, s.playRegionId), now};
    const CoopTrailPoint* last = trail->empty() ? nullptr : &trail->back();
    if (!last || last->mapKey != p.mapKey || (p.x - last->x) * (p.x - last->x) + (p.z - last->z) * (p.z - last->z) >= 0.25f)
      trail->push_back(p);
  }
  size_t drop = 0;
  while (drop < trail->size() && (now - (*trail)[drop].tick > kCoopTrailMs || trail->size() - drop > kCoopTrailMax)) ++drop;
  trail->erase(trail->begin(), trail->begin() + static_cast<std::ptrdiff_t>(drop));
}

// What the page needs to draw the partner, besides the session view.
struct CoopLivePartner {
  bool shown = false;  // a session with a partner now or earlier (their last values stay, dimmed)
  bool here = false;   // on the relay now
  bool have = false;   // their state has arrived
  SharedGameState state;
  std::vector<CoopTrailPoint> trail;
};

// Where the relay is and whether auto pairing can run, for the popover.
struct CoopRelayInfo {
  bool configured = false;  // a relay to connect to (hosted here, or relay_url)
  bool hosting = false, hostRunning = false;
  int port = 0;
  std::string hostError, url;  // url: relay_url, when not hosting
  bool pairAvailable = false;  // a Seamless Co-op password was found
  bool pairPaused = false;     // Disconnect or a session by code paused auto pairing
  TailscaleInfo tailscale;     // while hosting: the address to give the partner, and the forward
  bool tailscaleRead = false;  // tailscale has been read since hosting began
};

// Pure: /api/state's "coop" object. The partner's place follows the tier
// the way the results page's does (CoopPlaceView: named at Full or where
// you've been); their position and path only when they're in your map
// section; boss flags only from a boss list like yours (`bossFlags`, the
// list's flag ids in order); their item count for the area you're in only
// when their pickup list matches yours.
std::string CoopLiveJson(const CoopSessionView& v, const CoopRelayInfo& relay, const CoopLivePartner& p,
                         const SharedGameState& mine, bool haveMine, const CoopLists& lists,
                         const std::vector<uint32_t>& bossFlags, const std::vector<std::string>& areaList,
                         const std::string& myArea, const std::string& partnerArea, const std::string& partnerRegion,
                         const std::string& partnerBonfire, const std::string& fromArea, uint32_t nowUtc, int tier) {
  auto b = [](bool x) { return std::string(x ? "true" : "false"); };
  auto i = [](long long x) { return std::to_string(x); };
  std::string j = "{\"relay\":" + b(relay.configured) + ",\"host\":{\"on\":" + b(relay.hosting) + ",\"running\":" +
                  b(relay.hostRunning) + ",\"port\":" + i(relay.port) + ",\"error\":" + JsonString(relay.hostError) +
                  ",\"tailscale\":" +
                  (relay.tailscaleRead
                       ? "{\"installed\":" + b(relay.tailscale.installed) + ",\"running\":" + b(relay.tailscale.running) +
                             ",\"name\":" + JsonString(relay.tailscale.name) + ",\"ip\":" + JsonString(relay.tailscale.ip) +
                             ",\"serving\":" + b(relay.tailscale.serving) +
                             ",\"forwardError\":" + JsonString(relay.tailscale.forwardError) + "}"
                       : std::string("null")) +
                  "},\"relayUrl\":" + JsonString(relay.url) + ",\"pair\":{\"available\":" + b(relay.pairAvailable) +
                  ",\"paused\":" + b(relay.pairPaused) + "},\"state\":" + JsonString(v.state) + ",\"error\":" +
                  JsonString(v.error) + ",\"errorText\":" + JsonString(v.errorText) + ",\"code\":" + JsonString(v.code) +
                  ",\"paired\":" + b(v.paired) + ",\"openSeconds\":" + i(v.openSeconds) + ",\"goneSeconds\":" + i(v.goneSeconds) + ",\"rttMs\":" +
                  i(v.rttMs) + ",\"lastChangeMs\":" + i(v.lastChangeMs) + ",\"updatesPerSecond\":" +
                  JsonNum(v.updatesPerSecond, 1) + ",\"protocol\":" + i(kCoopProtocolVersion) + ",\"partnerProtocol\":" +
                  i(v.partnerProtocol) + ",\"partner\":";
  if (!p.shown) return j + "null}";
  j += "{\"here\":" + b(p.here) + ",\"have\":" + b(p.have);
  if (p.have) {
    const SharedGameState& s = p.state;
    CoopCompare c = BuildCoopCompare(mine, haveMine, s, true, lists, areaList, myArea, nowUtc, tier);
    CoopPlaceView(&c.partner, partnerArea, partnerRegion, partnerBonfire, &c.reached, tier, fromArea);
    j += ",\"name\":" + JsonString(s.characterName) + ",\"level\":" + i(s.level) + ",\"ng\":" + i(s.clearCount) +
         ",\"hp\":[" + i(s.hp) + "," + i(s.maxHp) + "],\"fp\":[" + i(s.fp) + "," + i(s.maxFp) + "],\"stamina\":[" +
         i(s.stamina) + "," + i(s.maxStamina) + "],\"deaths\":" + i(s.deaths);
    j += ",\"place\":{\"area\":" + JsonString(c.partner.area) + ",\"region\":" + JsonString(c.partner.region) +
         ",\"bonfire\":" + JsonString(c.partner.bonfire) + ",\"elsewhere\":" + b(c.partner.elsewhere) +
         ",\"beyond\":" + JsonString(c.partner.beyond) + "}";
    j += ",\"sameMap\":" + b(c.sameMap);
    if (c.sameMap && s.havePose) {
      j += ",\"pose\":{\"x\":" + JsonNum(s.x) + ",\"z\":" + JsonNum(s.z) + ",\"facing\":" + JsonNum(s.facing, 4) + "}";
      const int key = CoopMapKey(s.mapId, s.playRegionId);
      std::string t;
      for (const auto& pt : p.trail)
        if (pt.mapKey == key) t += (t.empty() ? "" : ",") + ("[" + JsonNum(pt.x, 1) + "," + JsonNum(pt.z, 1) + "]");
      j += ",\"trail\":[" + t + "]";
    } else {
      j += ",\"pose\":null,\"trail\":[]";
    }
    std::string items = "null";
    if (c.itemsCompared && !myArea.empty())
      for (const auto& row : c.items)
        if (row.area == myArea)
          items = "{\"area\":" + JsonString(row.area) + ",\"found\":" + i(row.partner) + ",\"total\":" + i(row.total) +
                  ",\"mine\":" + i(row.me) + "}";
    j += ",\"itemsHere\":" + items;
    std::string bosses;
    if (s.bossesDefeated.fingerprint == CoopFingerprint(bossFlags) && s.bossesDefeated.bits.size() == bossFlags.size())
      for (size_t k = 0; k < bossFlags.size(); ++k)
        if (s.bossesDefeated.bits[k]) bosses += (bosses.empty() ? "" : ",") + i(bossFlags[k]);
    j += ",\"bosses\":[" + bosses + "]";
  }
  return j + "}}";
}

class CoopSession {
 public:
  void StartThread() {
    stop_ = false;
    thread_ = std::thread([this] { Run(); });
  }
  void StopThread() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

  // Queues a command for the session thread. Returns "" or why it can't be
  // done (no relay, a code that can't be one).
  std::string Create() { return Queue({Command::kCreate, ""}); }
  std::string Join(const std::string& typed) {
    std::string code = CoopNormalizeCode(typed);
    if (code.empty()) return "a session code has 6 letters and digits";
    return Queue({Command::kJoin, code});
  }
  void Leave() { Queue({Command::kLeave, ""}); }
  void Pair() { Queue({Command::kPair, ""}); }  // turns auto pairing back on
  void Restart() { Queue({Command::kRestart, ""}); }  // the relay changed: end the session, pair again
  // Hosting: has Tailscale forward the relay's port, or stop (the host's
  // click). "" or why it can't.
  std::string Forward(bool on) {
    if (!g_hostRelay) return "not hosting the relay";
    return Queue({Command::kForward, on ? "1" : "0"});
  }

  bool Active() const { return active_; }

  // The last published view and partner (copies), and auto pairing's state.
  void Read(CoopSessionView* view, CoopLivePartner* partner) const {
    std::lock_guard<std::mutex> g(lock_);
    *view = view_;
    *partner = partner_;
  }
  void PairState(bool* available, bool* paused) const {
    std::lock_guard<std::mutex> g(lock_);
    *available = pairAvailable_;
    *paused = autoPaused_;
  }
  void TailscaleState(TailscaleInfo* info, bool* read) const {
    std::lock_guard<std::mutex> g(lock_);
    *info = tailscale_;
    *read = tailscaleRead_;
  }

 private:
  struct Command {
    enum Kind { kCreate, kJoin, kLeave, kPair, kRestart, kForward } kind;
    std::string code;
  };

  std::string Queue(Command c) {
    if ((c.kind == Command::kCreate || c.kind == Command::kJoin) && CoopRelayUrl().empty()) {
      std::lock_guard<std::mutex> g(lock_);
      view_.error = "no-relay";
      view_.errorText = "no relay: host it on this PC, or set relay_url to your partner's";
      return view_.errorText;
    }
    std::lock_guard<std::mutex> g(lock_);
    commands_.push_back(std::move(c));
    return "";
  }

  void Begin(const std::string& code, const std::string& pairKey = "") {
    const std::string url = CoopRelayUrl();
    if (pairKey.empty()) link_.Start(url, code);
    else link_.StartPair(url, pairKey);
    active_ = true;
    pairing_ = !pairKey.empty();
    pairKey_ = pairKey;
    everPartner_ = false;
    startedTick_ = GetTickCount64();
    goneSince_ = 0;
    shownTick_ = 0;
    partner_ = CoopLivePartner();
    rates_.clear();
    Log("Co-op session: %s%s (relay %s).",
        pairing_ ? "pairing by your Seamless Co-op password" : code.empty() ? "creating a session" : "joining ",
        code.c_str(), url.c_str());
  }
  void End(const std::string& error, const std::string& text) {
    link_.Stop();
    active_ = false;
    if (pairing_ && !error.empty()) retryAfter_ = GetTickCount64() + kCoopPairRetryMs;
    pairing_ = false;
    {
      std::lock_guard<std::mutex> g(g_coop.lock);
      g_coop.partnerLive = false;  // their last state stays for the results page
    }
    std::lock_guard<std::mutex> g(lock_);
    view_ = CoopSessionView();
    view_.error = error, view_.errorText = text;
    partner_ = CoopLivePartner();
    if (!error.empty()) Log("Co-op session over: %s (%s).", text.c_str(), error.c_str());
    else Log("Co-op session: left.");
  }

  void Tick() {
    std::vector<Command> commands;
    {
      std::lock_guard<std::mutex> g(lock_);
      commands.swap(commands_);
    }
    for (const Command& c : commands) {
      if (c.kind == Command::kPair) {
        SetPaused(false);
        retryAfter_ = nextPairCheck_ = 0;
        if (active_ && !pairing_) End("", "");
        continue;
      }
      if (c.kind == Command::kRestart) {
        if (active_) End("", "");
        retryAfter_ = nextPairCheck_ = nextTailscaleCheck_ = 0;
        continue;
      }
      if (c.kind == Command::kForward) {
        std::string why = TailscaleForwardRelay(g_relayPort, c.code == "1");  // returns within a second
        {
          std::lock_guard<std::mutex> g(lock_);
          tailscale_.forwardError = why;
        }
        nextTailscaleCheck_ = 0;  // read it back at once
        continue;
      }
      if (active_) End("", "");
      else if (c.kind == Command::kLeave) {
        std::lock_guard<std::mutex> g(lock_);
        view_ = CoopSessionView();  // clears an error left from the last session
      }
      SetPaused(true);  // Disconnect, or a session by code
      if (c.kind == Command::kCreate) Begin("");
      else if (c.kind == Command::kJoin) Begin(c.code);
    }
    CheckTailscale(GetTickCount64());
    AutoPair(GetTickCount64());
    if (!active_) return;

    RefreshMyCoopRecord();  // your recorded play goes with your state (summed again every 30 s at most)
    SharedGameState mine;
    bool haveMine;
    {
      std::lock_guard<std::mutex> g(g_coop.lock);
      haveMine = !g_coop.mine.characterName.empty();
      if (haveMine) mine = g_coop.mine;
    }
    std::vector<RelayEvent> events = link_.Tick(haveMine ? &mine : nullptr);
    const ULONGLONG now = GetTickCount64();  // after the tick: what arrived in it is no older than now
    for (const RelayEvent& e : events) {
      if (e.type == RelayEvent::kPartnerJoined) {
        everPartner_ = true;
        goneSince_ = 0;
      } else if ((e.type == RelayEvent::kPartnerLeft || e.type == RelayEvent::kDisconnected) && everPartner_ &&
                 !goneSince_) {
        goneSince_ = now;
      }
    }
    const RelayStatus relay = link_.Relay();
    const CoopSync& sync = link_.Sync();
    CoopSessionEnd end = CoopSessionEndFor(relay, sync.VersionMismatch(), sync.PartnerVersion(), goneSince_, now);
    if (end.over) {
      End(end.error, end.text);
      return;
    }
    const CoopSync::Partner& p = sync.PartnerState();
    rates_.push_back({now, sync.Stats().packetsReceived});
    while (rates_.size() > 1 && now - rates_.front().first > 5000) rates_.erase(rates_.begin());
    CoopSessionView v = DeriveCoopSession(relay, everPartner_, startedTick_, goneSince_, p.updatedTick, now);
    if (rates_.size() > 1 && now > rates_.front().first)
      v.updatesPerSecond = static_cast<double>(rates_.back().second - rates_.front().second) * 1000.0 /
                           static_cast<double>(now - rates_.front().first);
    std::lock_guard<std::mutex> g(lock_);
    view_ = v;
    partner_.shown = everPartner_;
    partner_.here = link_.PartnerHere();
    if (p.updatedTick && p.updatedTick != shownTick_) {
      partner_.have = true;
      partner_.state = p.state;
      shownTick_ = p.updatedTick;
      CoopTrailAdd(&partner_.trail, p.state, now);
      std::lock_guard<std::mutex> gc(g_coop.lock);
      g_coop.partner = p.state;
      g_coop.havePartner = g_coop.partnerLive = true;
    } else {
      CoopTrailAdd(&partner_.trail, SharedGameState(), now);  // only ages the path
    }
  }

  // While hosting, every 30 s (and at once after hosting starts): what
  // Tailscale says, for the popover. Not hosting: forgotten.
  void CheckTailscale(ULONGLONG now) {
    if (!g_hostRelay) {
      std::lock_guard<std::mutex> g(lock_);
      tailscale_ = TailscaleInfo();
      tailscaleRead_ = false;
      nextTailscaleCheck_ = 0;
      return;
    }
    if (now < nextTailscaleCheck_) return;
    nextTailscaleCheck_ = now + kCoopTailscaleCheckMs;
    TailscaleInfo info = ReadTailscale(g_relayPort);
    std::lock_guard<std::mutex> g(lock_);
    info.forwardError = tailscale_.forwardError;  // kept until the next click
    if (!tailscaleRead_ || info.serving != tailscale_.serving || info.name != tailscale_.name)
      Log("Co-op relay host: Tailscale %s%s%s.", !info.installed ? "isn't installed" : !info.running ? "isn't running" : "is on as ",
          info.name.c_str(), info.running ? (info.serving ? ", forwarding the relay's port" : ", not forwarding the relay's port yet") : "");
    tailscale_ = info;
    tailscaleRead_ = true;
  }

  void SetPaused(bool paused) {
    std::lock_guard<std::mutex> g(lock_);
    autoPaused_ = paused;
  }

  // Every few seconds: the Seamless password's pairing key. Starts pairing
  // when nothing else runs; a changed password moves to its new room.
  void AutoPair(ULONGLONG now) {
    if (now < nextPairCheck_) return;
    nextPairCheck_ = now + kCoopPairCheckMs;
    const std::string key = g_autoPair ? CoopPairKey(ReadSeamlessPassword(g_gameDir)) : "";
    bool paused;
    {
      std::lock_guard<std::mutex> g(lock_);
      pairAvailable_ = !key.empty();
      paused = autoPaused_;
    }
    if (active_ && pairing_ && key != pairKey_) {
      Log("Co-op session: the Seamless Co-op password changed -- leaving the old pairing room.");
      End("", "");
    }
    if (active_ || paused || key.empty() || CoopRelayUrl().empty() || now < retryAfter_) return;
    Begin("", key);
  }

  void Run() {
    while (!stop_) {
      Tick();
      Sleep(100);  // the polling rate
    }
    if (active_) link_.Stop();
  }

  mutable std::mutex lock_;  // view_, partner_, commands_, pairAvailable_, autoPaused_
  CoopSessionView view_;
  CoopLivePartner partner_;
  std::vector<Command> commands_;
  bool pairAvailable_ = false, autoPaused_ = false;
  TailscaleInfo tailscale_;
  bool tailscaleRead_ = false;
  ULONGLONG nextTailscaleCheck_ = 0;  // the session thread's
  // The session thread's own:
  CoopLink link_;
  std::atomic<bool> active_{false}, stop_{false};
  bool pairing_ = false;  // the session is a pairing room (pairKey_)
  std::string pairKey_;
  ULONGLONG nextPairCheck_ = 0, retryAfter_ = 0;
  bool everPartner_ = false;
  ULONGLONG startedTick_ = 0, goneSince_ = 0;
  ULONGLONG shownTick_ = 0;  // the partner state last copied to partner_
  std::vector<std::pair<ULONGLONG, uint64_t>> rates_;  // (tick, packets received) over the last 5 s
  std::thread thread_;
};
CoopSession g_coopSession;

// Hosts the relay on this PC (or stops), saves it, and starts the session
// over on the new relay. "" or why hosting failed (it stays on, so another
// copy already hosting on that port still works).
std::string SetHostRelay(bool on) {
  g_hostRelay = on;
  SaveSettings();
  std::string error;
  if (on && !g_relayServer.Running() && !g_relayServer.Start(g_relayPort)) error = g_relayServer.Error();
  if (!on) g_relayServer.Stop();
  g_coopSession.Restart();
  return error;
}

// The partner's side: the host's relay address as typed or pasted (see
// NormalizeRelayInput; "" clears it), saved as relay_url, and the session
// started over on it. "" or why not.
std::string SetPartnerRelay(const std::string& typed) {
  std::string url;
  if (!typed.empty()) {
    url = NormalizeRelayInput(typed);
    if (url.empty()) return "that isn't a relay address: paste the relay_url line your host sends";
  }
  SetRelayUrlSetting(url);
  SaveSettings();
  Log("Co-op relay: relay_url set to %s from the live page.", url.empty() ? "(none)" : url.c_str());
  g_coopSession.Restart();
  return "";
}

CoopRelayInfo CoopRelayInfoNow() {
  CoopRelayInfo r;
  r.configured = !CoopRelayUrl().empty();
  r.hosting = g_hostRelay;
  r.hostRunning = g_relayServer.Running();
  r.port = g_relayPort;
  r.hostError = g_hostRelay && !r.hostRunning ? g_relayServer.Error() : "";
  r.url = g_hostRelay ? "" : RelayUrlSetting();
  g_coopSession.PairState(&r.pairAvailable, &r.pairPaused);
  g_coopSession.TailscaleState(&r.tailscale, &r.tailscaleRead);
  return r;
}

bool CoopSessionActive() { return g_coopSession.Active(); }
void StartCoopSessionThread() {
  if (g_hostRelay) g_relayServer.Start(g_relayPort);
  g_coopSession.StartThread();
}
void StopCoopSessionThread() {
  g_coopSession.StopThread();
  g_relayServer.Stop();
}
