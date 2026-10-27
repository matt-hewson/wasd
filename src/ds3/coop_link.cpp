// src/ds3/coop_link.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- co-op link: our state to the partner and theirs to us, live ------
//
// Ties the relay client (connection, partner presence) to CoopSync
// (what to send, what arrived). Tick() runs at the polling rate (10 Hz)
// with our current state: it sends a full snapshot whenever the link is
// new (connected, reconnected, a new partner, a request), a delta when
// something changed, nothing otherwise; and applies what the partner sent.
// The partner's state stays separate from ours (Sync().PartnerState()).
// Another protocol version: nothing is applied, and the link closes.

class CoopLink {
 public:
  ~CoopLink() { Stop(); }

  bool Start(const std::string& relayUrl, const std::string& code, RelayTimings timings = {}) {
    Reset();
    return relay_.Start(relayUrl, code, timings);
  }
  // The pairing room for `key` instead of a session code.
  bool StartPair(const std::string& relayUrl, const std::string& key, RelayTimings timings = {}) {
    Reset();
    return relay_.StartPair(relayUrl, key, timings);
  }

  void Stop() {
    relay_.Stop();
    sync_.PartnerGone();
    partner_ = false;
  }

  // Handles what happened on the relay since the last call, then sends our
  // state if there's anything new for the partner. Returns the relay events
  // it handled (for a caller that shows them).
  std::vector<RelayEvent> Tick(const SharedGameState* local) {
    std::vector<RelayEvent> events = relay_.TakeEvents();
    for (const RelayEvent& e : events) {
      switch (e.type) {
        case RelayEvent::kConnected: sync_.Resync(); break;
        case RelayEvent::kPartnerJoined:
          partner_ = true;
          sync_.Resync();
          break;
        case RelayEvent::kPartnerLeft:
        case RelayEvent::kDisconnected:
        case RelayEvent::kEnded:
        case RelayEvent::kFailed:
          partner_ = false;
          sync_.PartnerGone();
          break;
        case RelayEvent::kFrame: Received(e.frame); break;
      }
    }
    if (sync_.VersionMismatch() && !mismatchLogged_) {
      mismatchLogged_ = true;
      Log("Co-op link: your partner's WASD speaks another protocol version (%s) -- disconnecting.",
          sync_.LastError().c_str());
      relay_.Stop();
      partner_ = false;
    }
    if (local && partner_ && !mismatchLogged_) {
      uint64_t fullBefore = sync_.Stats().fullSent;
      std::vector<uint8_t> out = sync_.Outgoing(*local);
      if (!out.empty()) {
        if (!relay_.Send(out)) sync_.Resync();  // not connected after all: all of it next time
        else if (sync_.Stats().fullSent > fullBefore) Log("Co-op link: full snapshot sent (%zu bytes).", out.size());
      }
    }
    return events;
  }

  RelayStatus Relay() const { return relay_.Status(); }
  const CoopSync& Sync() const { return sync_; }
  bool PartnerHere() const { return partner_; }

 private:
  void Reset() {
    Stop();
    sync_ = CoopSync();
    partner_ = false;
    mismatchLogged_ = false;
  }

  void Received(const std::vector<uint8_t>& frame) {
    uint64_t full = sync_.FullReceived(), rejected = sync_.Stats().rejected, asked = sync_.RequestsReceived();
    std::vector<uint8_t> reply = sync_.Incoming(frame.data(), frame.size());
    if (sync_.FullReceived() > full) Log("Co-op link: full snapshot received.");
    if (sync_.Stats().rejected > rejected && !sync_.VersionMismatch())
      Log("Co-op link: packet rejected (%s).", sync_.LastError().c_str());
    if (sync_.RequestsReceived() > asked) Log("Co-op link: your partner asked for a full snapshot.");
    if (!reply.empty()) {
      Log("Co-op link: asking your partner for a full snapshot (%s).", sync_.LastError().c_str());
      relay_.Send(reply);
    }
  }

  RelayClient relay_;
  CoopSync sync_;
  bool partner_ = false, mismatchLogged_ = false;
};

// relay create|<code> --sync [--still] [--name NAME] ...: a live link with a
// made-up character instead of the game, for trying the link (and two
// copies on one PC) without DS3. Unless --still, its HP drains and refills,
// it walks in a circle and finds a pickup every few seconds. Prints the
// partner's state as it changes ("partner-state seq 7 name Ashen hp 400/500
// found 2/3") and the link's counts at the end.
int RunRelaySync(int argc, wchar_t** argv) {
  std::string target = argc > 2 ? Utf8(argv[2]) : "", url = CoopRelayUrl(), name = "Test";
  int seconds = 60;
  bool still = false;
  RelayTimings timings;
  for (int i = 3; i < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"--still") still = true;
    else if (i + 1 >= argc) break;
    else if (a == L"--url") ParseRelayUrl(Utf8(argv[++i]), &url);
    else if (a == L"--name") name = Utf8(argv[++i]);
    else if (a == L"--seconds") seconds = _wtoi(argv[++i]);
    else if (a == L"--heartbeat") timings.heartbeatMs = (std::max)(100, _wtoi(argv[++i]));
    else if (a == L"--dead-ms") timings.deadMs = (std::max)(500, _wtoi(argv[++i]));
  }
  if (target.empty() || url.empty()) {
    std::printf("usage: relay create|<code>|pair:<key> --sync [--still] [--name NAME] [--url URL] [--seconds N]\n");
    return 1;
  }
  SharedGameState me;
  me.characterName = name;
  me.level = 30, me.maxHp = 500, me.hp = 500, me.maxFp = 100, me.fp = 100, me.maxStamina = 100, me.stamina = 100;
  me.mapId = 0x28000000, me.playRegionId = 400110, me.lastBonfireId = 1400000;
  me.havePose = true;
  me.pickupsFound = CoopFlagSet{CoopFingerprint({1, 2, 3}), {false, false, false}};
  CoopLink link;
  bool started = target.rfind("pair:", 0) == 0 ? link.StartPair(url, target.substr(5), timings)
                                               : link.Start(url, target == "create" ? "" : target, timings);
  if (!started) return 1;
  bool printedCode = false;
  uint32_t shownSeq = 0;
  bool shownAny = false;
  int exitCode = 0;
  const ULONGLONG start = GetTickCount64(), end = start + static_cast<ULONGLONG>(seconds) * 1000;
  for (int tick = 0; GetTickCount64() < end; ++tick) {
    Sleep(100);  // the polling rate
    me.capturedUtc = static_cast<uint32_t>(std::time(nullptr));
    me.playSeconds = static_cast<uint32_t>((GetTickCount64() - start) / 1000);
    if (!still) {
      me.hp = static_cast<uint16_t>(500 - (tick % 50) * 10);
      me.x = 10.0f * std::cos(tick * 0.05f), me.z = 10.0f * std::sin(tick * 0.05f);
      me.pickupsFound.bits[(tick / 30) % 3] = (tick / 90) % 2 == 0;
    }
    bool done = false;
    for (const RelayEvent& e : link.Tick(&me)) {
      int code = PrintRelayEvent(e, false);
      if (code >= 0) done = true, exitCode = code;
    }
    RelayStatus st = link.Relay();
    if (!printedCode && !st.code.empty() && st.state != RelayState::kCreating) {
      std::printf("code %s\n", st.code.c_str());
      printedCode = true;
    }
    const CoopSync::Partner& p = link.Sync().PartnerState();
    if (p.have && (!shownAny || p.sequence != shownSeq)) {
      std::printf("partner-state seq %u name %s hp %u/%u found %d/%zu\n", p.sequence, p.state.characterName.c_str(),
                  p.state.hp, p.state.maxHp, p.state.pickupsFound.CountOn(), p.state.pickupsFound.bits.size());
      shownSeq = p.sequence, shownAny = true;
    }
    if (link.Sync().VersionMismatch()) done = true, exitCode = 4;
    std::fflush(stdout);
    if (done) break;
  }
  link.Stop();
  const CoopSyncStats& s = link.Sync().Stats();
  std::printf("sync sent %llu packets %llu bytes (%llu full, %llu deltas, %llu requests), received %llu packets "
              "(%llu full), rejected %llu\n",
              static_cast<unsigned long long>(s.packetsSent), static_cast<unsigned long long>(s.bytesSent),
              static_cast<unsigned long long>(s.fullSent), static_cast<unsigned long long>(s.deltasSent),
              static_cast<unsigned long long>(s.requestsSent), static_cast<unsigned long long>(s.packetsReceived),
              static_cast<unsigned long long>(link.Sync().FullReceived()), static_cast<unsigned long long>(s.rejected));
  return exitCode;
}
