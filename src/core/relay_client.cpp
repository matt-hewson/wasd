// src/core/relay_client.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- co-op relay client (WinHTTP WebSockets) ---------------------------
//
// Talks to the co-op relay (src/core/relay_server.cpp, run by one player's
// WASD; where it is: CoopRelayUrl()). Creates a session (POST /session),
// joins one by code (GET /session/{code}, a WebSocket) or a pairing room
// by key (GET /pair/{key}), then passes binary frames both ways.
// It knows nothing about what's in a frame: the co-op code's packets go
// through it as bytes.
//
// Two threads per client: one only receives; the other connects, sends
// (queued frames and the heartbeat) and reconnects. The heartbeat is the
// text "ping" every 10 s (the relay answers "pong" without waking the
// room); nothing heard for 30 s means the connection is dead. A dropped
// connection reconnects to the same code with backoff (1, 2, 4 ... 30 s).
// The relay answers 404 once a room is forgotten (or the host's WASD was
// restarted): that ends the session instead of retrying forever. A pairing
// room is made again on the next try, so it never ends that way. The rest of WASD reads what happened from
// TakeEvents() and Status(); nothing received can do anything but queue
// an event.

constexpr size_t kRelayMaxFrame = 64 * 1024;  // the relay's limit too

struct RelayEndpoint {
  bool ok = false;
  bool secure = false;
  std::wstring host;
  INTERNET_PORT port = 0;
  std::wstring basePath;  // "" or "/something", no trailing slash
};

// relay_url ("http(s)://host[:port][/path]") as WinHTTP wants it.
RelayEndpoint ParseRelayEndpoint(const std::string& url) {
  RelayEndpoint e;
  std::string rest;
  if (url.rfind("https://", 0) == 0) e.secure = true, rest = url.substr(8);
  else if (url.rfind("http://", 0) == 0) rest = url.substr(7);
  else return e;
  size_t slash = rest.find('/');
  std::string hostPort = rest.substr(0, slash), path = slash == std::string::npos ? "" : rest.substr(slash);
  while (!path.empty() && path.back() == '/') path.pop_back();
  std::string host = hostPort, port;
  if (!hostPort.empty() && hostPort[0] == '[') {  // [IPv6]:port
    size_t close = hostPort.find(']');
    if (close == std::string::npos) return e;
    host = hostPort.substr(1, close - 1);
    if (close + 1 < hostPort.size()) {
      if (hostPort[close + 1] != ':') return e;
      port = hostPort.substr(close + 2);
    }
  } else if (size_t colon = hostPort.find(':'); colon != std::string::npos) {
    host = hostPort.substr(0, colon), port = hostPort.substr(colon + 1);
  }
  if (host.empty()) return e;
  e.port = e.secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
  if (!port.empty() || hostPort.back() == ':') {
    if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return e;
    int p = std::atoi(port.c_str());
    if (p < 1 || p > 65535) return e;
    e.port = static_cast<INTERNET_PORT>(p);
  }
  e.host = WidenUtf8(host);
  e.basePath = WidenUtf8(path);
  e.ok = true;
  return e;
}

// Wait before reconnect attempt `attempt` (0 = the first): 1, 2, 4, 8, 16, then 30 s.
int RelayBackoffMs(int attempt) {
  if (attempt < 0) attempt = 0;
  return attempt >= 5 ? 30'000 : 1000 << attempt;
}

// What the relay's answer to a join means (src/core/relay_server.cpp).
enum class RelayJoin { kOk, kBadCode, kNoSession, kFull, kFailed };
RelayJoin RelayJoinFromStatus(int httpStatus) {
  switch (httpStatus) {
    case 101: return RelayJoin::kOk;
    case 400: return RelayJoin::kBadCode;
    case 404: return RelayJoin::kNoSession;
    case 409: return RelayJoin::kFull;
    default: return RelayJoin::kFailed;
  }
}

// The code in POST /session's answer, {"code":"K7DP4X"}; "" if there isn't one.
std::string RelayCodeFromJson(const std::string& body) {
  size_t at = body.find("\"code\"");
  if (at == std::string::npos) return "";
  size_t open = body.find('"', body.find(':', at) + 1);
  size_t close = open == std::string::npos ? std::string::npos : body.find('"', open + 1);
  if (close == std::string::npos) return "";
  std::string code = body.substr(open + 1, close - open - 1);
  for (char c : code)
    if (!std::isalnum(static_cast<unsigned char>(c))) return "";
  return code.size() == 6 ? code : "";
}

enum class RelayState { kOff, kCreating, kConnecting, kWaiting, kPaired, kReconnecting, kEnded, kFailed };

const char* RelayStateName(RelayState s) {
  switch (s) {
    case RelayState::kOff: return "off";
    case RelayState::kCreating: return "creating a session";
    case RelayState::kConnecting: return "connecting";
    case RelayState::kWaiting: return "waiting for partner";
    case RelayState::kPaired: return "partner connected";
    case RelayState::kReconnecting: return "reconnecting";
    case RelayState::kEnded: return "session ended";
    case RelayState::kFailed: return "failed";
  }
  return "?";
}

struct RelayEvent {
  enum Type { kConnected, kPartnerJoined, kPartnerLeft, kFrame, kDisconnected, kEnded, kFailed } type;
  std::vector<uint8_t> frame;  // kFrame only
  std::string text;            // kFailed / kEnded / kDisconnected: why
};

// Why a session couldn't start or go on, for the page to explain.
enum class RelayProblem { kNone, kUnreachable, kBadCode, kNoSession, kFull, kOther };

struct RelayStatus {
  RelayState state = RelayState::kOff;
  std::string code;     // the session code, once known
  std::string message;  // why it ended or failed
  RelayProblem problem = RelayProblem::kNone;  // set with message; kUnreachable also while retrying
  bool created = false;  // we made this session (rather than joined it)
  bool paired = false;   // a pairing room (by key, no code)
  bool everIn = false;   // the relay let us into the room at least once
  bool partner = false;
  int reconnects = 0;
  int rttMs = -1;        // the last ping's round trip to the relay; -1 = not measured yet
  uint64_t framesSent = 0, framesReceived = 0, bytesSent = 0, bytesReceived = 0;
  size_t lastFrameSize = 0;
  ULONGLONG lastReceivedTick = 0;  // any frame, pong included
};

struct RelayTimings {
  int heartbeatMs = 10'000;  // send "ping" this often
  int deadMs = 30'000;       // nothing heard for this long: reconnect
};

class RelayClient {
 public:
  ~RelayClient() { Stop(); }

  // Creates a session (code "") or joins one. Returns at once; watch Status().
  bool Start(const std::string& relayUrl, const std::string& code, RelayTimings timings = {}) {
    return Begin(relayUrl, code, "", timings);
  }
  // Joins the pairing room for `key` (64 hex digits). Returns at once.
  bool StartPair(const std::string& relayUrl, const std::string& key, RelayTimings timings = {}) {
    return Begin(relayUrl, "", key, timings);
  }

  bool Begin(const std::string& relayUrl, const std::string& code, const std::string& pairKey, RelayTimings timings) {
    Stop();
    endpoint_ = ParseRelayEndpoint(relayUrl);
    if (!endpoint_.ok) {
      SetFailed("relay_url isn't an http:// or https:// address");
      return false;
    }
    timings_ = timings;
    {
      std::lock_guard<std::mutex> g(lock_);
      status_ = RelayStatus{};
      status_.code = code;
      status_.created = code.empty() && pairKey.empty();
      status_.paired = !pairKey.empty();
      status_.state = status_.created ? RelayState::kCreating : RelayState::kConnecting;
      events_.clear();
      outbox_.clear();
      stop_ = false;
    }
    pairKey_ = pairKey;
    runner_ = std::thread([this] { Run(); });
    return true;
  }

  // Leaves the session: closes the socket and stops both threads.
  void Stop() {
    {
      std::lock_guard<std::mutex> g(lock_);
      stop_ = true;
    }
    wake_.notify_all();
    if (runner_.joinable()) runner_.join();
  }

  // Queues a frame for the partner. False when not connected or too big.
  bool Send(const std::vector<uint8_t>& frame) {
    if (frame.empty() || frame.size() > kRelayMaxFrame) return false;
    {
      std::lock_guard<std::mutex> g(lock_);
      if (status_.state != RelayState::kWaiting && status_.state != RelayState::kPaired) return false;
      outbox_.push_back(frame);
    }
    wake_.notify_all();
    return true;
  }

  std::vector<RelayEvent> TakeEvents() {
    std::lock_guard<std::mutex> g(lock_);
    std::vector<RelayEvent> out;
    out.swap(events_);
    return out;
  }

  RelayStatus Status() const {
    std::lock_guard<std::mutex> g(lock_);
    return status_;
  }

 private:
  void Push(RelayEvent e) {
    std::lock_guard<std::mutex> g(lock_);
    events_.push_back(std::move(e));
  }
  void SetState(RelayState s) {
    std::lock_guard<std::mutex> g(lock_);
    status_.state = s;
  }
  void SetFailed(const std::string& why, RelayProblem problem = RelayProblem::kOther) {
    {
      std::lock_guard<std::mutex> g(lock_);
      status_.state = RelayState::kFailed;
      status_.message = why;
      status_.problem = problem;
    }
    Push({RelayEvent::kFailed, {}, why});
    Log("Co-op relay: %s.", why.c_str());
  }
  bool Stopping() {
    std::lock_guard<std::mutex> g(lock_);
    return stop_;
  }
  // Sleeps up to `ms`, or until Stop(). True if stopping.
  bool WaitOrStop(int ms) {
    std::unique_lock<std::mutex> g(lock_);
    return wake_.wait_for(g, std::chrono::milliseconds(ms), [this] { return stop_; });
  }

  // POST /session -> the new code ("" and *why on failure).
  std::string CreateSession(HINTERNET session, std::string* why, bool* unreachable) {
    std::string code;
    HINTERNET conn = WinHttpConnect(session, endpoint_.host.c_str(), endpoint_.port, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"POST", (endpoint_.basePath + L"/session").c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              endpoint_.secure ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
    DWORD status = 0, size = sizeof(status);
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr) &&
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX)) {
      std::string body;
      char buf[512];
      DWORD got = 0;
      while (body.size() < 4096 && WinHttpReadData(req, buf, sizeof(buf), &got) && got) body.append(buf, got);
      code = status == 201 ? RelayCodeFromJson(body) : "";
      if (code.empty()) *why = "the relay didn't make a session (HTTP " + std::to_string(status) + ")";
    } else {
      *why = "couldn't reach the relay (error " + std::to_string(GetLastError()) + ")";
      *unreachable = true;
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    return code;
  }

  // GET /session/{code} (or /pair/{key}) upgraded to a WebSocket. The HTTP
  // status (101 when in; 0 when the relay couldn't be reached); *ws and
  // *conn set on 101.
  int Join(HINTERNET session, const std::string& code, HINTERNET* conn, HINTERNET* ws) {
    *conn = WinHttpConnect(session, endpoint_.host.c_str(), endpoint_.port, 0);
    const std::wstring path = pairKey_.empty() ? L"/session/" + WidenUtf8(code) : L"/pair/" + WidenUtf8(pairKey_);
    HINTERNET req = *conn ? WinHttpOpenRequest(*conn, L"GET", (endpoint_.basePath + path).c_str(),
                                               nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               endpoint_.secure ? WINHTTP_FLAG_SECURE : 0)
                          : nullptr;
    DWORD status = 0, size = sizeof(status);
    if (req && WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
        WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr) &&
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX)) {
      if (status == 101) {
        *ws = WinHttpWebSocketCompleteUpgrade(req, 0);
        if (!*ws) status = 0;
      }
    }
    if (req) WinHttpCloseHandle(req);
    if (status != 101 && *conn) {
      WinHttpCloseHandle(*conn);
      *conn = nullptr;
    }
    return static_cast<int>(status);
  }

  // The receiving thread: frames and notices until the socket closes.
  void Receive(HINTERNET ws) {
    std::vector<uint8_t> message, buf(16 * 1024);
    for (;;) {
      DWORD got = 0;
      WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
      DWORD err = WinHttpWebSocketReceive(ws, buf.data(), static_cast<DWORD>(buf.size()), &got, &type);
      if (err != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
      message.insert(message.end(), buf.begin(), buf.begin() + got);
      if (message.size() > kRelayMaxFrame) break;  // the relay never sends one this big
      if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
        continue;
      ULONGLONG now = GetTickCount64();
      if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
        {
          std::lock_guard<std::mutex> g(lock_);
          status_.lastReceivedTick = now;
          ++status_.framesReceived;
          status_.bytesReceived += message.size();
        }
        Push({RelayEvent::kFrame, message, ""});
      } else {
        std::string text(message.begin(), message.end());
        bool joined = text == "partner:joined", left = text == "partner:left";
        {
          std::lock_guard<std::mutex> g(lock_);
          status_.lastReceivedTick = now;
          if (text == "pong" && pingSentTick_) {
            status_.rttMs = static_cast<int>(now - pingSentTick_);
            pingSentTick_ = 0;
          }
          if (joined || left) {
            status_.partner = joined;
            status_.state = joined ? RelayState::kPaired : RelayState::kWaiting;
          }
        }
        if (joined) {
          Log("Co-op relay: partner connected.");
          Push({RelayEvent::kPartnerJoined, {}, ""});
        } else if (left) {
          Log("Co-op relay: partner left.");
          Push({RelayEvent::kPartnerLeft, {}, ""});
        }
        // "pong" and anything unknown: nothing to do (seen above).
      }
      message.clear();
    }
    wake_.notify_all();  // the sender notices the socket is gone
    std::lock_guard<std::mutex> g(lock_);
    receiverDone_ = true;
  }

  bool SendNow(HINTERNET ws, const std::vector<uint8_t>& bytes, bool binary) {
    return WinHttpWebSocketSend(ws, binary ? WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE
                                           : WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                const_cast<uint8_t*>(bytes.data()), static_cast<DWORD>(bytes.size())) == NO_ERROR;
  }

  // One connected socket: send and keep the heartbeat until it drops or
  // Stop(). True if it ended because of Stop().
  bool Serve(HINTERNET ws) {
    {
      std::lock_guard<std::mutex> g(lock_);
      receiverDone_ = false;
      status_.lastReceivedTick = GetTickCount64();
    }
    std::thread receiver([this, ws] { Receive(ws); });
    static const std::vector<uint8_t> kPing = {'p', 'i', 'n', 'g'};
    // The first ping goes at once, so the round trip is known straight away.
    ULONGLONG lastPing = GetTickCount64() - static_cast<ULONGLONG>(timings_.heartbeatMs);
    bool stopped = false;
    std::string why;
    for (;;) {
      std::vector<std::vector<uint8_t>> out;
      ULONGLONG lastHeard;
      {
        std::unique_lock<std::mutex> g(lock_);
        wake_.wait_for(g, std::chrono::milliseconds(200), [this] { return stop_ || receiverDone_ || !outbox_.empty(); });
        if (stop_) { stopped = true; break; }
        if (receiverDone_) { why = "the connection closed"; break; }
        out.swap(outbox_);
        lastHeard = status_.lastReceivedTick;
      }
      bool ok = true;
      for (const auto& frame : out) {
        ok = ok && SendNow(ws, frame, true);
        if (ok) {
          std::lock_guard<std::mutex> g(lock_);
          ++status_.framesSent;
          status_.bytesSent += frame.size();
          status_.lastFrameSize = frame.size();
        }
      }
      ULONGLONG now = GetTickCount64();
      if (ok && now - lastPing >= static_cast<ULONGLONG>(timings_.heartbeatMs)) {
        {
          std::lock_guard<std::mutex> g(lock_);
          pingSentTick_ = now;
        }
        ok = SendNow(ws, kPing, false);
        lastPing = now;
      }
      if (!ok) { why = "sending failed"; break; }
      if (now - lastHeard > static_cast<ULONGLONG>(timings_.deadMs)) {
        why = "nothing heard for " + std::to_string(timings_.deadMs / 1000) + " s";
        break;
      }
    }
    if (stopped) WinHttpWebSocketShutdown(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    WinHttpCloseHandle(ws);  // also ends a receive that's still waiting
    receiver.join();
    if (!stopped) {
      Log("Co-op relay: disconnected (%s).", why.c_str());
      {
        std::lock_guard<std::mutex> g(lock_);
        status_.partner = false;
        outbox_.clear();
      }
      Push({RelayEvent::kDisconnected, {}, why});
    }
    return stopped;
  }

  void Run() {
    const std::wstring agent = L"WASD/" + WidenUtf8(WASD_VERSION_STRING);
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
      SetFailed("WinHTTP isn't available");
      return;
    }
    WinHttpSetTimeouts(session, 10'000, 10'000, 10'000, 15'000);
    std::string code = Status().code;
    if (code.empty() && pairKey_.empty()) {
      std::string why;
      bool unreachable = false;
      code = CreateSession(session, &why, &unreachable);
      if (code.empty()) {
        SetFailed(why, unreachable ? RelayProblem::kUnreachable : RelayProblem::kOther);
        WinHttpCloseHandle(session);
        return;
      }
      {
        std::lock_guard<std::mutex> g(lock_);
        status_.code = code;
        status_.state = RelayState::kConnecting;
      }
      Log("Co-op relay: session %s created.", code.c_str());
    }
    // A pairing room is named in the log by the start of its key only.
    const std::string what = pairKey_.empty() ? "session " + code : "pairing room " + pairKey_.substr(0, 8);
    bool everIn = false;
    for (int attempt = 0; !Stopping();) {
      if (everIn || attempt) SetState(RelayState::kReconnecting);
      Log("Co-op relay: connecting to %s%s.", what.c_str(), attempt ? " again" : "");
      HINTERNET conn = nullptr, ws = nullptr;
      int status = Join(session, code, &conn, &ws);
      RelayJoin result = RelayJoinFromStatus(status);
      if (result == RelayJoin::kOk) {
        {
          std::lock_guard<std::mutex> g(lock_);
          status_.state = RelayState::kWaiting;
          status_.partner = false;
          status_.everIn = true;
          status_.problem = RelayProblem::kNone;
          if (everIn) ++status_.reconnects;
        }
        Log("Co-op relay: in %s, waiting for partner.", what.c_str());
        Push({RelayEvent::kConnected, {}, ""});
        everIn = true;
        attempt = 0;
        bool stopped = Serve(ws);
        WinHttpCloseHandle(conn);
        if (stopped) break;
        continue;  // reconnect at once; backoff only after a failed attempt
      }
      if (result == RelayJoin::kNoSession) {
        std::string why = "session " + code + " is no longer on the relay";
        {
          std::lock_guard<std::mutex> g(lock_);
          status_.state = RelayState::kEnded;
          status_.message = why;
          status_.problem = RelayProblem::kNoSession;
        }
        Log("Co-op relay: %s.", why.c_str());
        Push({RelayEvent::kEnded, {}, why});
        break;
      }
      if (result == RelayJoin::kBadCode) {
        SetFailed("\"" + code + "\" isn't a session code", RelayProblem::kBadCode);
        break;
      }
      if (result == RelayJoin::kFull && !everIn) {
        SetFailed(what + " already has two players", RelayProblem::kFull);
        break;
      }
      // Unreachable, or full while our old socket times out: try again later.
      int wait = RelayBackoffMs(attempt++);
      Log("Co-op relay: %s; trying again in %d s.",
          status ? ("HTTP " + std::to_string(status)).c_str() : "couldn't reach the relay", wait / 1000);
      {
        std::lock_guard<std::mutex> g(lock_);
        status_.state = RelayState::kReconnecting;
        if (!status) status_.problem = RelayProblem::kUnreachable;
      }
      if (WaitOrStop(wait)) break;
    }
    WinHttpCloseHandle(session);
    std::lock_guard<std::mutex> g(lock_);
    if (status_.state != RelayState::kEnded && status_.state != RelayState::kFailed) status_.state = RelayState::kOff;
    status_.partner = false;
  }

  RelayEndpoint endpoint_;
  RelayTimings timings_;
  mutable std::mutex lock_;
  std::condition_variable wake_;
  RelayStatus status_;
  std::vector<RelayEvent> events_;
  std::vector<std::vector<uint8_t>> outbox_;
  bool stop_ = false;
  bool receiverDone_ = false;
  ULONGLONG pingSentTick_ = 0;  // the ping waiting for its pong (0: none)
  std::string pairKey_;         // a pairing room's key ("" for a session by code)
  std::thread runner_;
};

// One line per event for `relay`. The exit code when the event ends the
// session (3 ended, 2 failed), else -1.
int PrintRelayEvent(const RelayEvent& e, bool frames) {
  switch (e.type) {
    case RelayEvent::kConnected: std::printf("connected\n"); break;
    case RelayEvent::kPartnerJoined: std::printf("partner-joined\n"); break;
    case RelayEvent::kPartnerLeft: std::printf("partner-left\n"); break;
    case RelayEvent::kFrame:
      if (frames) {
        std::printf("frame ");
        for (uint8_t b : e.frame) std::printf("%02x", b);
        std::printf("\n");
      }
      break;
    case RelayEvent::kDisconnected: std::printf("disconnected %s\n", e.text.c_str()); break;
    case RelayEvent::kEnded: std::printf("ended %s\n", e.text.c_str()); return 3;
    case RelayEvent::kFailed: std::printf("failed %s\n", e.text.c_str()); return 2;
  }
  return -1;
}

// relay create|<code>|pair:<key> [--url URL] [--send HEX] [--seconds N] [--heartbeat MS]:
// a session from the command line, for trying the relay without the app.
// Prints "code K7DP4X" once a session is made, then one line per event
// ("partner-joined", "frame 0a0b", "disconnected ...", ...). --send sends
// those bytes once the partner is there.
int RunRelay(int argc, wchar_t** argv) {
  std::string target = argc > 2 ? Utf8(argv[2]) : "", url = CoopRelayUrl(), sendHex;
  int seconds = 60;
  RelayTimings timings;
  for (int i = 3; i + 1 < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"--url") ParseRelayUrl(Utf8(argv[++i]), &url);
    else if (a == L"--send") sendHex = Utf8(argv[++i]);
    else if (a == L"--seconds") seconds = _wtoi(argv[++i]);
    else if (a == L"--heartbeat") timings.heartbeatMs = (std::max)(100, _wtoi(argv[++i]));
    else if (a == L"--dead-ms") timings.deadMs = (std::max)(500, _wtoi(argv[++i]));
  }
  if (target.empty() || url.empty()) {
    std::printf("usage: relay create|<code>|pair:<key> [--url http://127.0.0.1:8790] [--send HEX] [--seconds N]\n"
                "(the relay is the one in settings.ini unless --url is given)\n");
    return 1;
  }
  std::vector<uint8_t> toSend;
  for (size_t i = 0; i + 1 < sendHex.size(); i += 2)
    toSend.push_back(static_cast<uint8_t>(std::strtoul(sendHex.substr(i, 2).c_str(), nullptr, 16)));
  RelayClient client;
  bool started = target.rfind("pair:", 0) == 0 ? client.StartPair(url, target.substr(5), timings)
                                               : client.Start(url, target == "create" ? "" : target, timings);
  if (!started) return 1;
  bool printedCode = false, sent = false;
  int exitCode = 0;
  for (ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000; GetTickCount64() < end;) {
    Sleep(50);
    RelayStatus st = client.Status();
    if (!printedCode && !st.code.empty() && st.state != RelayState::kCreating) {
      std::printf("code %s\n", st.code.c_str());
      printedCode = true;
    }
    bool done = false;
    for (const auto& e : client.TakeEvents()) {
      if (e.type == RelayEvent::kPartnerJoined && !toSend.empty() && !sent) sent = client.Send(toSend);
      int code = PrintRelayEvent(e, true);
      if (code >= 0) done = true, exitCode = code;
    }
    std::fflush(stdout);
    if (done) break;
  }
  client.Stop();
  RelayStatus st = client.Status();
  std::printf("stats sent %llu frames %llu bytes, received %llu frames %llu bytes, reconnects %d\n",
              static_cast<unsigned long long>(st.framesSent), static_cast<unsigned long long>(st.bytesSent),
              static_cast<unsigned long long>(st.framesReceived), static_cast<unsigned long long>(st.bytesReceived),
              st.reconnects);
  return exitCode;
}
