// src/core/relay_server.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- co-op relay server: the host's WASD is the relay ------------------
//
// One player's WASD runs the relay (host_relay=1 in settings.ini, or "Host
// the relay here" on the live page); both copies' RelayClient connect to it,
// the host's on 127.0.0.1 and the partner's over Tailscale, which carries
// them to it (`tailscale serve --bg --tcp 8790 tcp://localhost:8790` on the
// host). It listens on 127.0.0.1 only: nothing on the LAN or the internet
// can reach it, and Windows Firewall has nothing to ask.
//
// It pairs two copies by a short session code and passes their binary
// frames between them. It never reads them: what's in a frame is the
// clients' business, so the relay doesn't change when the protocol does.
//
//   GET  /                 -> 200 "WASD co-op relay"
//   POST /session          -> 201 {"code":"K7DP4X"}: a new, empty room
//   GET  /session/{code}   -> WebSocket upgrade into that room
//                             (400 bad code, 404 no such room, 409 full)
//   GET  /pair/{key}       -> WebSocket upgrade into the pairing room for
//                             that key (64 hex digits; made on first use;
//                             400 bad key, 409 full)
//
// At most two sockets a room. In a room:
//   - a binary frame (at most kRelayMaxFrame) goes to the other socket;
//   - the text frame "ping" is answered "pong" (the clients' heartbeat);
//   - any other text frame closes the socket (1003), a frame that's too
//     big too (1009);
//   - the relay tells each client about its partner with the text frames
//     "partner:joined" and "partner:left".
// A room that's never joined is forgotten after unjoinedRoomMs, an empty
// one emptyRoomMs after its last socket left. A socket that hasn't pinged
// or sent for staleMs can be replaced, so a client whose connection died
// without closing can rejoin. Nothing is kept on disk.

constexpr size_t kRelayMaxSockets = 32;     // connections at once, HTTP ones included
constexpr size_t kRelayMaxRequest = 8 * 1024;
constexpr size_t kRelayMaxQueued = 1 << 20;  // a socket this far behind is dropped
constexpr ULONGLONG kRelayHttpTimeoutMs = 10'000;

struct RelayRoomTimings {
  ULONGLONG staleMs = 30'000;            // a socket silent this long can be replaced
  ULONGLONG emptyRoomMs = 10 * 60'000;   // an empty room is forgotten after this
  ULONGLONG unjoinedRoomMs = 30 * 60'000;  // a room nobody joined, after this
};

// ---- small crypto: random bytes, SHA-1 / SHA-256, Base64 ----------------

bool RelayRandomBytes(uint8_t* out, size_t n) {
  return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

// The digest of `data` with a CNG algorithm (BCRYPT_SHA1_ALGORITHM, BCRYPT_SHA256_ALGORITHM); empty on failure.
std::vector<uint8_t> HashBytes(LPCWSTR algorithm, const std::string& data) {
  std::vector<uint8_t> digest;
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, algorithm, nullptr, 0))) return digest;
  DWORD size = 0, got = 0;
  if (BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &got, 0))) {
    digest.resize(size);
    if (!BCRYPT_SUCCESS(BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                   static_cast<ULONG>(data.size()), digest.data(), size)))
      digest.clear();
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return digest;
}

std::string HexBytes(const std::vector<uint8_t>& bytes) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  for (uint8_t b : bytes) out += kHex[b >> 4], out += kHex[b & 15];
  return out;
}

std::string Base64(const std::vector<uint8_t>& bytes) {
  static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < bytes.size(); i += 3) {
    uint32_t n = static_cast<uint32_t>(bytes[i]) << 16;
    if (i + 1 < bytes.size()) n |= static_cast<uint32_t>(bytes[i + 1]) << 8;
    if (i + 2 < bytes.size()) n |= bytes[i + 2];
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += i + 1 < bytes.size() ? kB64[(n >> 6) & 63] : '=';
    out += i + 2 < bytes.size() ? kB64[n & 63] : '=';
  }
  return out;
}

// Sec-WebSocket-Accept for a client's Sec-WebSocket-Key (RFC 6455 4.2.2).
std::string WebSocketAccept(const std::string& key) {
  return Base64(HashBytes(BCRYPT_SHA1_ALGORITHM, key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
}

// ---- session codes ------------------------------------------------------
// 6 characters from 30 that can't be mistaken for each other when read
// aloud or typed (no 0/O, 1/I/L, U), so about 729 million codes.

constexpr const char* kRelayCodeAlphabet = "23456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr size_t kRelayCodeLength = 6;

// A new random code (no modulo bias); "" if Windows has no randomness to give.
std::string NewRelayCode() {
  const size_t n = std::strlen(kRelayCodeAlphabet), limit = 256 - (256 % n);
  std::string code;
  while (code.size() < kRelayCodeLength) {
    uint8_t bytes[16];
    if (!RelayRandomBytes(bytes, sizeof(bytes))) return "";
    for (uint8_t b : bytes)
      if (b < limit && code.size() < kRelayCodeLength) code += kRelayCodeAlphabet[b % n];
  }
  return code;
}

// The code in its canonical form (any case; spaces and dashes ignored), or "" if it isn't one.
std::string RelayNormalizeCode(const std::string& text) {
  std::string code;
  for (char c : text) {
    if (c == ' ' || c == '-') continue;
    char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!std::strchr(kRelayCodeAlphabet, u) || u == 0) return "";
    code += u;
  }
  return code.size() == kRelayCodeLength ? code : "";
}

// A pairing key: 64 lower-case hex digits (a SHA-256). False for anything else.
bool RelayIsPairKey(const std::string& key) {
  if (key.size() != 64) return false;
  for (char c : key)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// ---- HTTP requests and WebSocket frames ---------------------------------

struct RelayHttpRequest {
  bool complete = false, bad = false;
  std::string method, path;
  std::vector<std::pair<std::string, std::string>> headers;  // names in lower case
  size_t length = 0;  // header and body, once complete

  std::string Header(const std::string& name) const {
    for (const auto& h : headers)
      if (h.first == name) return h.second;
    return "";
  }
};

// The request at the start of `buf`: incomplete until its headers (and a
// short body, if any) are in; bad if it can't be one.
RelayHttpRequest ParseRelayHttp(const std::string& buf) {
  RelayHttpRequest r;
  size_t end = buf.find("\r\n\r\n");
  if (end == std::string::npos) {
    r.bad = buf.size() > kRelayMaxRequest;
    return r;
  }
  size_t lineEnd = buf.find("\r\n");
  std::string line = buf.substr(0, lineEnd);
  size_t sp1 = line.find(' '), sp2 = sp1 == std::string::npos ? sp1 : line.find(' ', sp1 + 1);
  if (sp2 == std::string::npos || line.compare(sp2 + 1, 5, "HTTP/") != 0) {
    r.bad = true;
    return r;
  }
  r.method = line.substr(0, sp1);
  r.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
  size_t q = r.path.find('?');
  if (q != std::string::npos) r.path.resize(q);
  for (size_t at = lineEnd + 2; at < end;) {
    size_t next = buf.find("\r\n", at);
    std::string h = buf.substr(at, next - at);
    at = next + 2;
    size_t colon = h.find(':');
    if (colon == std::string::npos) continue;
    std::string name = h.substr(0, colon), value = h.substr(colon + 1);
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
    r.headers.emplace_back(name, value);
  }
  size_t body = 0;
  std::string cl = r.Header("content-length");
  if (!cl.empty()) {
    if (cl.size() > 5 || cl.find_first_not_of("0123456789") != std::string::npos) {
      r.bad = true;
      return r;
    }
    body = static_cast<size_t>(std::atoi(cl.c_str()));
    if (body > 4096) {
      r.bad = true;
      return r;
    }
  }
  if (buf.size() < end + 4 + body) return r;
  r.length = end + 4 + body;
  r.complete = true;
  return r;
}

// Whether a header value lists `token` (comma-separated, any case): "Connection: keep-alive, Upgrade".
bool RelayHeaderHas(const std::string& value, const std::string& token) {
  std::string v;
  for (char c : value) v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (size_t at = 0; at <= v.size();) {
    size_t comma = v.find(',', at);
    std::string part = v.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
    while (!part.empty() && part.front() == ' ') part.erase(part.begin());
    while (!part.empty() && part.back() == ' ') part.pop_back();
    if (part == token) return true;
    if (comma == std::string::npos) break;
    at = comma + 1;
  }
  return false;
}

enum WsOpcode { kWsContinuation = 0, kWsText = 1, kWsBinary = 2, kWsClose = 8, kWsPing = 9, kWsPong = 10 };

struct WsFrame {
  int status = 0;  // 0 needs more bytes, 1 a frame, -1 the socket must close (closeCode)
  bool fin = false;
  int opcode = 0;
  std::vector<uint8_t> payload;  // unmasked
  size_t used = 0;
  int closeCode = 0;
};

// One client frame from the start of `p` (clients must mask, RFC 6455 5.1).
WsFrame ParseWsFrame(const uint8_t* p, size_t n, size_t maxPayload) {
  WsFrame f;
  if (n < 2) return f;
  f.fin = (p[0] & 0x80) != 0;
  f.opcode = p[0] & 0x0F;
  bool masked = (p[1] & 0x80) != 0;
  uint64_t len = p[1] & 0x7F;
  size_t at = 2;
  if (len == 126) {
    if (n < 4) return f;
    len = (static_cast<uint64_t>(p[2]) << 8) | p[3];
    at = 4;
  } else if (len == 127) {
    if (n < 10) return f;
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
    at = 10;
  }
  auto fail = [&](int code) {
    f.status = -1, f.closeCode = code;
    return f;
  };
  if (p[0] & 0x70) return fail(1002);  // reserved bits: no extensions were agreed
  if (!masked) return fail(1002);
  bool control = (f.opcode & 0x08) != 0;
  if (control && (!f.fin || len > 125)) return fail(1002);
  if (f.opcode != kWsContinuation && f.opcode != kWsText && f.opcode != kWsBinary && !control) return fail(1002);
  if (len > maxPayload) return fail(1009);
  if (n < at + 4 + len) return f;
  const uint8_t* mask = p + at;
  at += 4;
  f.payload.resize(static_cast<size_t>(len));
  for (size_t i = 0; i < len; ++i) f.payload[i] = p[at + i] ^ mask[i & 3];
  f.used = at + static_cast<size_t>(len);
  f.status = 1;
  return f;
}

// A server frame (unmasked, unfragmented).
std::string WsFrameBytes(int opcode, const uint8_t* data, size_t n) {
  std::string out;
  out += static_cast<char>(0x80 | opcode);
  if (n < 126) {
    out += static_cast<char>(n);
  } else if (n < 65536) {
    out += static_cast<char>(126);
    out += static_cast<char>((n >> 8) & 0xFF);
    out += static_cast<char>(n & 0xFF);
  } else {
    out += static_cast<char>(127);
    for (int i = 7; i >= 0; --i) out += static_cast<char>((static_cast<uint64_t>(n) >> (8 * i)) & 0xFF);
  }
  out.append(reinterpret_cast<const char*>(data), n);
  return out;
}
std::string WsFrameBytes(int opcode, const std::string& text) {
  return WsFrameBytes(opcode, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}
std::string WsCloseFrame(int code, const std::string& reason) {
  std::string payload;
  payload += static_cast<char>((code >> 8) & 0xFF);
  payload += static_cast<char>(code & 0xFF);
  payload += reason.substr(0, 120);
  return WsFrameBytes(kWsClose, payload);
}

// ---- rooms (pure: connection ids in, what to send out) ------------------

class RelayRooms {
 public:
  struct Out {
    enum Kind { kText, kBinary, kClose } kind;
    int conn;
    std::string text;  // kText: the frame; kClose: the reason
    std::vector<uint8_t> data;
    int closeCode = 0;
  };

  explicit RelayRooms(RelayRoomTimings t = {}) : t_(t) {}

  // A new, empty room for a session; its code ("" if no free code was found).
  std::string Create(ULONGLONG now) {
    for (int tries = 0; tries < 5; ++tries) {  // a taken code is all but impossible; retry anyway
      std::string code = NewRelayCode();
      if (!code.empty() && !rooms_.count(code)) {
        Room& r = rooms_[code];
        r.created = now;
        return code;
      }
    }
    return "";
  }

  // `conn` asks to join room `name` (a code, or "pair:" + key, made here if
  // it's missing). The HTTP status: 101 in, 404 no such room, 409 full.
  // Others in the room hear "partner:joined", and so does `conn`, once its
  // handshake has gone (the caller sends `out` after it).
  int Admit(const std::string& name, int conn, ULONGLONG now, std::vector<Out>* out) {
    auto it = rooms_.find(name);
    if (it == rooms_.end()) {
      if (name.rfind("pair:", 0) != 0) return 404;
      it = rooms_.emplace(name, Room{}).first;
      it->second.created = now;
    }
    Room& r = it->second;
    if (r.conns.size() >= 2) {
      std::vector<int> keep;
      for (int c : r.conns) {
        if (now - seen_[c] > t_.staleMs) {
          out->push_back({Out::kClose, c, "replaced: no heartbeat", {}, 4000});
          roomOf_.erase(c), seen_.erase(c);
        } else {
          keep.push_back(c);
        }
      }
      r.conns = keep;
      if (r.conns.size() >= 2) return 409;
    }
    for (int c : r.conns) out->push_back({Out::kText, c, "partner:joined", {}, 0});
    if (!r.conns.empty()) out->push_back({Out::kText, conn, "partner:joined", {}, 0});
    r.conns.push_back(conn);
    r.joined = true;
    r.emptySince = 0;
    roomOf_[conn] = name;
    seen_[conn] = now;
    return 101;
  }

  // A text frame from `conn`: "ping" is answered, anything else closes it.
  void Text(int conn, const std::string& text, ULONGLONG now, std::vector<Out>* out) {
    if (!roomOf_.count(conn)) return;
    if (text == "ping") {
      seen_[conn] = now;
      out->push_back({Out::kText, conn, "pong", {}, 0});
      return;
    }
    out->push_back({Out::kClose, conn, "binary frames only", {}, 1003});
    Left(conn, now, out);
  }

  // A binary frame from `conn`: to the other socket in its room.
  void Binary(int conn, const std::vector<uint8_t>& frame, ULONGLONG now, std::vector<Out>* out) {
    auto it = roomOf_.find(conn);
    if (it == roomOf_.end()) return;
    seen_[conn] = now;
    for (int c : rooms_[it->second].conns)
      if (c != conn) out->push_back({Out::kBinary, c, "", frame, 0});
  }

  // A sign of life that isn't a frame for the room (a WebSocket ping or pong).
  void Seen(int conn, ULONGLONG now) {
    if (seen_.count(conn)) seen_[conn] = now;
  }

  // `conn` closed or dropped: its partner hears "partner:left".
  void Left(int conn, ULONGLONG now, std::vector<Out>* out) {
    auto it = roomOf_.find(conn);
    if (it == roomOf_.end()) return;
    Room& r = rooms_[it->second];
    r.conns.erase(std::remove(r.conns.begin(), r.conns.end(), conn), r.conns.end());
    for (int c : r.conns) out->push_back({Out::kText, c, "partner:left", {}, 0});
    if (r.conns.empty()) r.emptySince = now;
    roomOf_.erase(it);
    seen_.erase(conn);
  }

  // Forgets rooms whose time is up.
  void Expire(ULONGLONG now) {
    for (auto it = rooms_.begin(); it != rooms_.end();) {
      const Room& r = it->second;
      bool gone = r.conns.empty() && (r.joined ? now - r.emptySince >= t_.emptyRoomMs : now - r.created >= t_.unjoinedRoomMs);
      it = gone ? rooms_.erase(it) : std::next(it);
    }
  }

  bool HasRoom(const std::string& name) const { return rooms_.count(name) != 0; }
  size_t RoomCount() const { return rooms_.size(); }

 private:
  struct Room {
    ULONGLONG created = 0, emptySince = 0;
    bool joined = false;
    std::vector<int> conns;
  };
  RelayRoomTimings t_;
  std::map<std::string, Room> rooms_;
  std::map<int, std::string> roomOf_;
  std::map<int, ULONGLONG> seen_;
};

// ---- the server ---------------------------------------------------------

class RelayServer {
 public:
  ~RelayServer() { Stop(); }

  // Listens on 127.0.0.1:port. False (and Error() says why) if it can't.
  bool Start(int port, RelayRoomTimings timings = {}) {
    std::lock_guard<std::mutex> g(control_);
    StopLocked();
    error_.clear();
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      error_ = "Winsock isn't available";
      return false;
    }
    wsa_ = true;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1: Tailscale brings the partner here
    if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 8) != 0) {
      if (s != INVALID_SOCKET) closesocket(s);
      error_ = "port " + std::to_string(port) + " is taken";
      Log("Co-op relay host: %s -- not hosting (relay_port in settings.ini).", error_.c_str());
      WSACleanup();
      wsa_ = false;
      return false;
    }
    listener_ = s;
    port_ = port;
    rooms_ = RelayRooms(timings);
    stop_ = false;
    thread_ = std::thread([this] { Run(); });
    Log("Co-op relay host: listening on 127.0.0.1:%d.", port);
    return true;
  }

  void Stop() {
    std::lock_guard<std::mutex> g(control_);
    StopLocked();
  }

  bool Running() const { return running_; }
  // Tests only: stop reading, writing and accepting, but keep every socket
  // open, the way a host whose PC slept or whose network dropped looks to
  // its clients. false carries on.
  void Stall(bool stalled) { stalled_ = stalled; }
  int Port() const { return port_; }
  std::string Error() const {
    std::lock_guard<std::mutex> g(control_);
    return error_;
  }

 private:
  struct Conn {
    SOCKET s = INVALID_SOCKET;
    int id = 0;
    bool ws = false;           // past the handshake
    bool closeAfter = false;   // close once `out` has gone
    std::string in, out;
    std::vector<uint8_t> message;  // a fragmented message so far
    int messageOpcode = 0;
    ULONGLONG opened = 0;
  };

  void StopLocked() {
    if (thread_.joinable()) {
      stop_ = true;
      thread_.join();
      Log("Co-op relay host: stopped.");
    }
    if (listener_ != INVALID_SOCKET) closesocket(listener_);
    listener_ = INVALID_SOCKET;
    if (wsa_) WSACleanup();
    wsa_ = false;
    running_ = false;
  }

  static std::string HttpResponse(int status, const std::string& type, const std::string& body) {
    const char* reason = status == 200 ? "OK" : status == 201 ? "Created" : status == 400 ? "Bad Request"
                       : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : status == 409 ? "Conflict"
                       : status == 426 ? "Upgrade Required" : "Service Unavailable";
    return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: " + type +
           "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
  }
  static std::string JsonError(int status, const std::string& why) {
    return HttpResponse(status, "application/json", "{\"error\":" + JsonString(why) + "}");
  }

  // Routes one complete HTTP request. A WebSocket upgrade into a room
  // switches the connection over; anything else answers and closes.
  void Route(Conn& c, const RelayHttpRequest& r, ULONGLONG now) {
    auto answer = [&](const std::string& response) {
      c.out += response;
      c.closeAfter = true;
    };
    if (r.path == "/") {
      answer(HttpResponse(200, "text/plain", "WASD co-op relay\n"));
      return;
    }
    if (r.path == "/session") {
      if (r.method != "POST") return answer(JsonError(405, "use POST"));
      std::string code = rooms_.Create(now);
      if (code.empty()) return answer(JsonError(503, "no free code, try again"));
      Log("Co-op relay host: session %s made.", code.c_str());
      return answer(HttpResponse(201, "application/json", "{\"code\":\"" + code + "\"}"));
    }
    std::string room;
    if (r.path.rfind("/session/", 0) == 0) {
      if (r.method != "GET") return answer(JsonError(405, "use GET"));
      room = RelayNormalizeCode(r.path.substr(9));
      if (room.empty()) return answer(JsonError(400, "not a session code"));
    } else if (r.path.rfind("/pair/", 0) == 0) {
      if (r.method != "GET") return answer(JsonError(405, "use GET"));
      std::string key = r.path.substr(6);
      if (!RelayIsPairKey(key)) return answer(JsonError(400, "not a pairing key"));
      room = "pair:" + key;
    } else {
      return answer(JsonError(404, "not found"));
    }
    std::string key = r.Header("sec-websocket-key");
    if (!RelayHeaderHas(r.Header("upgrade"), "websocket") || key.empty())
      return answer(JsonError(426, "expected a WebSocket upgrade"));
    std::vector<RelayRooms::Out> out;
    int status = rooms_.Admit(room, c.id, now, &out);
    if (status == 404) return answer(JsonError(404, "no such session"));
    if (status == 409) return answer(JsonError(409, "session is full"));
    c.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
             WebSocketAccept(key) + "\r\n\r\n";
    c.ws = true;
    Deliver(out);
  }

  Conn* Find(int id) {
    for (auto& c : conns_)
      if (c.id == id) return &c;
    return nullptr;
  }

  void Deliver(const std::vector<RelayRooms::Out>& out) {
    for (const auto& o : out) {
      Conn* c = Find(o.conn);
      if (!c || c->closeAfter) continue;
      if (o.kind == RelayRooms::Out::kText) c->out += WsFrameBytes(kWsText, o.text);
      else if (o.kind == RelayRooms::Out::kBinary) c->out += WsFrameBytes(kWsBinary, o.data.data(), o.data.size());
      else c->out += WsCloseFrame(o.closeCode, o.text), c->closeAfter = true;
      if (c->out.size() > kRelayMaxQueued) c->out.clear(), c->closeAfter = true;  // hopelessly behind
    }
  }

  // Frames in from a WebSocket connection.
  void Frames(Conn& c, ULONGLONG now) {
    std::vector<RelayRooms::Out> out;
    size_t at = 0;
    while (!c.closeAfter) {
      WsFrame f = ParseWsFrame(reinterpret_cast<const uint8_t*>(c.in.data()) + at, c.in.size() - at, kRelayMaxFrame);
      if (f.status == 0) break;
      if (f.status < 0) {
        c.out += WsCloseFrame(f.closeCode, f.closeCode == 1009 ? "frame too big" : "protocol error");
        c.closeAfter = true;
        rooms_.Left(c.id, now, &out);
        break;
      }
      at += f.used;
      if (f.opcode == kWsClose) {
        c.out += WsFrameBytes(kWsClose, f.payload.data(), (std::min)(f.payload.size(), size_t{2}));
        c.closeAfter = true;
        rooms_.Left(c.id, now, &out);
        break;
      }
      if (f.opcode == kWsPing) {
        c.out += WsFrameBytes(kWsPong, f.payload.data(), f.payload.size());
        rooms_.Seen(c.id, now);
        continue;
      }
      if (f.opcode == kWsPong) {
        rooms_.Seen(c.id, now);
        continue;
      }
      if (f.opcode != kWsContinuation) c.message.clear(), c.messageOpcode = f.opcode;
      c.message.insert(c.message.end(), f.payload.begin(), f.payload.end());
      if (c.message.size() > kRelayMaxFrame) {
        c.out += WsCloseFrame(1009, "frame too big");
        c.closeAfter = true;
        rooms_.Left(c.id, now, &out);
        break;
      }
      if (!f.fin) continue;
      if (c.messageOpcode == kWsText) rooms_.Text(c.id, std::string(c.message.begin(), c.message.end()), now, &out);
      else rooms_.Binary(c.id, c.message, now, &out);
      c.message.clear();
    }
    c.in.erase(0, at);
    Deliver(out);
  }

  void Drop(Conn& c, ULONGLONG now) {
    std::vector<RelayRooms::Out> out;
    rooms_.Left(c.id, now, &out);
    closesocket(c.s);
    c.s = INVALID_SOCKET;
    Deliver(out);
  }

  void Run() {
    running_ = true;
    ULONGLONG lastExpire = 0;
    while (!stop_) {
      if (stalled_) {
        Sleep(50);
        continue;
      }
      fd_set readable, writable;
      FD_ZERO(&readable);
      FD_ZERO(&writable);
      FD_SET(listener_, &readable);
      for (const auto& c : conns_) {
        FD_SET(c.s, &readable);
        if (!c.out.empty()) FD_SET(c.s, &writable);
      }
      timeval tv{0, 100 * 1000};
      if (select(0, &readable, &writable, nullptr, &tv) == SOCKET_ERROR) {
        Sleep(50);
        continue;
      }
      const ULONGLONG now = GetTickCount64();
      if (FD_ISSET(listener_, &readable)) {
        SOCKET s = accept(listener_, nullptr, nullptr);
        if (s != INVALID_SOCKET) {
          if (conns_.size() >= kRelayMaxSockets) {
            closesocket(s);
          } else {
            u_long nonBlocking = 1;
            ioctlsocket(s, FIONBIO, &nonBlocking);
            Conn c;
            c.s = s, c.id = ++nextId_, c.opened = now;
            conns_.push_back(std::move(c));
          }
        }
      }
      for (auto& c : conns_) {
        if (c.s == INVALID_SOCKET) continue;
        if (FD_ISSET(c.s, &readable)) {
          char buf[16 * 1024];
          int got = recv(c.s, buf, sizeof(buf), 0);
          if (got <= 0) {
            if (got == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
              Drop(c, now);
              continue;
            }
          } else if (!c.closeAfter) {
            c.in.append(buf, static_cast<size_t>(got));
            if (!c.ws) {
              RelayHttpRequest r = ParseRelayHttp(c.in);
              if (r.bad) {
                c.out += JsonError(400, "bad request");
                c.closeAfter = true;
              } else if (r.complete) {
                c.in.erase(0, r.length);
                Route(c, r, now);
              }
            }
            if (c.ws && !c.in.empty()) Frames(c, now);
          }
        }
        if (!c.ws && !c.closeAfter && now - c.opened > kRelayHttpTimeoutMs) Drop(c, now);
      }
      for (auto& c : conns_) {
        if (c.s == INVALID_SOCKET || c.out.empty()) continue;
        int sent = send(c.s, c.out.data(), static_cast<int>((std::min)(c.out.size(), size_t{1} << 20)), 0);
        if (sent > 0) c.out.erase(0, static_cast<size_t>(sent));
        else if (WSAGetLastError() != WSAEWOULDBLOCK) Drop(c, now);
      }
      for (auto& c : conns_)
        if (c.s != INVALID_SOCKET && c.closeAfter && c.out.empty()) {
          shutdown(c.s, SD_SEND);
          Drop(c, now);
        }
      conns_.erase(std::remove_if(conns_.begin(), conns_.end(), [](const Conn& c) { return c.s == INVALID_SOCKET; }),
                   conns_.end());
      if (now - lastExpire >= 1000) {
        rooms_.Expire(now);
        lastExpire = now;
      }
    }
    for (auto& c : conns_) closesocket(c.s);
    conns_.clear();
    running_ = false;
  }

  mutable std::mutex control_;  // Start / Stop / error_
  std::string error_;
  bool wsa_ = false;
  SOCKET listener_ = INVALID_SOCKET;
  int port_ = 0;
  std::atomic<bool> stop_{false}, running_{false}, stalled_{false};
  std::thread thread_;
  // The server thread's own:
  RelayRooms rooms_;
  std::vector<Conn> conns_;
  int nextId_ = 0;
};
RelayServer g_relayServer;

// relay-host [--port N] [--seconds N] [--stale-ms N] [--empty-room-ms N]
// [--unjoined-room-ms N] [--stall-after MS --stall-for MS]: the relay on its
// own, for the tests and for trying it without the app. Prints "relay
// http://127.0.0.1:N" once it listens; runs until killed (or --seconds).
// --stall-after / --stall-for (tests): after MS it stops answering for MS,
// sockets left open (printing "stalled" and "resumed").
int RunRelayHost(int argc, wchar_t** argv) {
  int port = g_relayPort, seconds = 0, stallAfter = -1, stallFor = 0;
  RelayRoomTimings t;
  for (int i = 2; i + 1 < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"--port") port = _wtoi(argv[++i]);
    else if (a == L"--seconds") seconds = _wtoi(argv[++i]);
    else if (a == L"--stale-ms") t.staleMs = static_cast<ULONGLONG>(_wtoi(argv[++i]));
    else if (a == L"--empty-room-ms") t.emptyRoomMs = static_cast<ULONGLONG>(_wtoi(argv[++i]));
    else if (a == L"--unjoined-room-ms") t.unjoinedRoomMs = static_cast<ULONGLONG>(_wtoi(argv[++i]));
    else if (a == L"--stall-after") stallAfter = _wtoi(argv[++i]);
    else if (a == L"--stall-for") stallFor = _wtoi(argv[++i]);
  }
  if (!g_relayServer.Start(port, t)) {
    std::printf("failed %s\n", g_relayServer.Error().c_str());
    return 2;
  }
  std::printf("relay http://127.0.0.1:%d\n", port);
  std::fflush(stdout);
  const ULONGLONG start = GetTickCount64();
  bool stalled = false, resumed = false;
  for (ULONGLONG end = start + static_cast<ULONGLONG>(seconds) * 1000; !seconds || GetTickCount64() < end;) {
    Sleep(20);
    const ULONGLONG at = GetTickCount64() - start;
    if (stallAfter >= 0 && !stalled && at >= static_cast<ULONGLONG>(stallAfter)) {
      g_relayServer.Stall(true);
      stalled = true;
      std::printf("stalled\n");
      std::fflush(stdout);
    }
    if (stalled && !resumed && at >= static_cast<ULONGLONG>(stallAfter + stallFor)) {
      g_relayServer.Stall(false);
      resumed = true;
      std::printf("resumed\n");
      std::fflush(stdout);
    }
  }
  g_relayServer.Stop();
  return 0;
}
