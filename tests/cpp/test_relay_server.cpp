// The co-op relay the host's WASD runs (src/core/relay_server.cpp): the
// handshake, codes, HTTP and WebSocket framing, and the rooms' rules. The
// server itself is tested end to end by tests/relay/relay.test.mjs.

TEST(relay_server_handshake_and_base64) {
  // RFC 6455's own example.
  CHECK_EQ(WebSocketAccept("dGhlIHNhbXBsZSBub25jZQ=="), std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
  CHECK_EQ(Base64({}), std::string(""));
  CHECK_EQ(Base64({'f'}), std::string("Zg=="));
  CHECK_EQ(Base64({'f', 'o'}), std::string("Zm8="));
  CHECK_EQ(Base64({'f', 'o', 'o'}), std::string("Zm9v"));
  CHECK_EQ(HexBytes({0x00, 0xAB, 0x0F}), std::string("00ab0f"));
}

TEST(relay_server_codes) {
  CHECK_EQ(std::string(kRelayCodeAlphabet).find_first_of("01ILOU"), std::string::npos);
  std::set<std::string> seen;
  for (int i = 0; i < 50; ++i) {
    std::string code = NewRelayCode();
    CHECK_EQ(code.size(), size_t{6});
    CHECK_EQ(code.find_first_not_of(kRelayCodeAlphabet), std::string::npos);
    seen.insert(code);
  }
  CHECK(seen.size() > 45);  // random, so all but surely all different
  CHECK_EQ(RelayNormalizeCode("k7dp4x"), std::string("K7DP4X"));
  CHECK_EQ(RelayNormalizeCode("K7D-P4X"), std::string("K7DP4X"));
  CHECK_EQ(RelayNormalizeCode(" K7D P4X "), std::string("K7DP4X"));
  CHECK_EQ(RelayNormalizeCode("K7DP4O"), std::string(""));  // O isn't in the alphabet
  CHECK_EQ(RelayNormalizeCode("K7DP4"), std::string(""));
  CHECK_EQ(RelayNormalizeCode("K7DP4XY"), std::string(""));
  CHECK(RelayIsPairKey(std::string(64, 'a')));
  CHECK(!RelayIsPairKey(std::string(63, 'a')));
  CHECK(!RelayIsPairKey(std::string(64, 'A')));
  CHECK(!RelayIsPairKey(std::string(63, 'a') + "g"));
}

TEST(relay_server_http_requests) {
  CHECK(!ParseRelayHttp("GET / HTTP/1.1\r\nHost: x\r\n").complete);
  CHECK(!ParseRelayHttp("GET / HTTP/1.1\r\n").bad);
  CHECK(ParseRelayHttp(std::string(kRelayMaxRequest + 1, 'x')).bad);
  CHECK(ParseRelayHttp("NONSENSE\r\n\r\n").bad);

  const std::string get =
      "GET /session/K7DP4X?x=1 HTTP/1.1\r\nUpgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\n"
      "Sec-WebSocket-Key:  abc== \r\n\r\nextra";
  RelayHttpRequest r = ParseRelayHttp(get);
  CHECK(r.complete && !r.bad);
  CHECK_EQ(r.method, std::string("GET"));
  CHECK_EQ(r.path, std::string("/session/K7DP4X"));
  CHECK_EQ(r.Header("sec-websocket-key"), std::string("abc=="));
  CHECK_EQ(r.length, get.find("extra"));  // what follows is the WebSocket's
  CHECK(RelayHeaderHas(r.Header("upgrade"), "websocket"));
  CHECK(RelayHeaderHas(r.Header("connection"), "upgrade"));
  CHECK(!RelayHeaderHas("keep-alive", "upgrade"));

  // A body is waited for, then counted in the length.
  const std::string post = "POST /session HTTP/1.1\r\nContent-Length: 2\r\n\r\n";
  CHECK(!ParseRelayHttp(post + "x").complete);
  r = ParseRelayHttp(post + "xy");
  CHECK(r.complete);
  CHECK_EQ(r.length, post.size() + 2);
  CHECK(ParseRelayHttp("POST /session HTTP/1.1\r\nContent-Length: 99999\r\n\r\n").bad);
}

namespace relay_server_test {
// A client frame: masked, as browsers and WinHTTP send them.
std::string ClientFrame(int opcode, const std::string& payload, bool fin = true) {
  std::string f;
  f += static_cast<char>((fin ? 0x80 : 0) | opcode);
  const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
  if (payload.size() < 126) {
    f += static_cast<char>(0x80 | payload.size());
  } else {
    f += static_cast<char>(0x80 | 126);
    f += static_cast<char>(payload.size() >> 8);
    f += static_cast<char>(payload.size() & 0xFF);
  }
  f.append(reinterpret_cast<const char*>(mask), 4);
  for (size_t i = 0; i < payload.size(); ++i) f += static_cast<char>(payload[i] ^ mask[i & 3]);
  return f;
}
WsFrame Parse(const std::string& bytes, size_t max = kRelayMaxFrame) {
  return ParseWsFrame(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), max);
}
}  // namespace relay_server_test

TEST(relay_server_websocket_frames) {
  using relay_server_test::ClientFrame;
  using relay_server_test::Parse;
  std::string ping = ClientFrame(kWsText, "ping");
  WsFrame f = Parse(ping);
  CHECK_EQ(f.status, 1);
  CHECK(f.fin);
  CHECK_EQ(f.opcode, int{kWsText});
  CHECK_EQ(std::string(f.payload.begin(), f.payload.end()), std::string("ping"));
  CHECK_EQ(f.used, ping.size());
  CHECK_EQ(Parse(ping.substr(0, 5)).status, 0);  // not all there yet

  std::string big(300, 'z');
  f = Parse(ClientFrame(kWsBinary, big));
  CHECK_EQ(f.status, 1);
  CHECK_EQ(f.payload.size(), size_t{300});
  CHECK_EQ(Parse(ClientFrame(kWsBinary, big), 299).closeCode, 1009);  // too big

  std::string unmasked = "\x81\x04ping";
  CHECK_EQ(Parse(unmasked).closeCode, 1002);  // clients must mask
  CHECK_EQ(Parse(ClientFrame(kWsPing, "x", false)).closeCode, 1002);  // a control frame can't be split
  CHECK_EQ(Parse(ClientFrame(3, "x")).closeCode, 1002);  // no such opcode

  // Server frames aren't masked; lengths past 125 take two bytes.
  std::string s = WsFrameBytes(kWsText, "pong");
  CHECK_EQ(s, std::string("\x81\x04pong"));
  CHECK_EQ(WsFrameBytes(kWsBinary, std::string(200, 'a')).substr(0, 4), std::string("\x82\x7e\x00\xc8", 4));
  CHECK_EQ(WsCloseFrame(1003, "no").substr(0, 4), std::string("\x88\x04\x03\xeb", 4));
}

namespace relay_server_test {
// The texts sent to `conn` among `out`, joined by "|".
std::string Texts(const std::vector<RelayRooms::Out>& out, int conn) {
  std::string t;
  for (const auto& o : out)
    if (o.conn == conn && o.kind == RelayRooms::Out::kText) t += (t.empty() ? "" : "|") + o.text;
  return t;
}
}  // namespace relay_server_test

TEST(relay_server_rooms) {
  using relay_server_test::Texts;
  RelayRoomTimings t;
  t.staleMs = 1000, t.emptyRoomMs = 5000, t.unjoinedRoomMs = 9000;
  RelayRooms rooms(t);
  std::vector<RelayRooms::Out> out;
  CHECK_EQ(rooms.Admit("K7DP4X", 1, 0, &out), 404);  // no such session

  std::string code = rooms.Create(100);
  CHECK_EQ(code.size(), size_t{6});
  CHECK_EQ(rooms.Admit(code, 1, 200, &out), 101);
  CHECK(out.empty());  // alone: nobody to tell
  CHECK_EQ(rooms.Admit(code, 2, 300, &out), 101);
  CHECK_EQ(Texts(out, 1), std::string("partner:joined"));
  CHECK_EQ(Texts(out, 2), std::string("partner:joined"));

  // Frames go to the other socket; "ping" is answered; other text closes.
  out.clear();
  rooms.Binary(1, {9, 8, 7}, 400, &out);
  CHECK_EQ(out.size(), size_t{1});
  CHECK_EQ(out[0].conn, 2);
  CHECK(out[0].kind == RelayRooms::Out::kBinary);
  CHECK(out[0].data == std::vector<uint8_t>({9, 8, 7}));
  out.clear();
  rooms.Text(2, "ping", 400, &out);
  CHECK_EQ(Texts(out, 2), std::string("pong"));

  // A third is refused while both are alive...
  out.clear();
  CHECK_EQ(rooms.Admit(code, 3, 900, &out), 409);
  // ...but replaces one that's been silent past staleMs (1 last seen at 400; 2 pings on).
  rooms.Text(2, "ping", 1200, &out);
  out.clear();
  CHECK_EQ(rooms.Admit(code, 3, 1500, &out), 101);
  CHECK(out.size() >= 1 && out[0].kind == RelayRooms::Out::kClose && out[0].conn == 1 && out[0].closeCode == 4000);
  CHECK_EQ(Texts(out, 2), std::string("partner:joined"));
  CHECK_EQ(Texts(out, 1), std::string(""));

  out.clear();
  rooms.Text(3, "hello", 1400, &out);
  CHECK(out.size() == 2 && out[0].kind == RelayRooms::Out::kClose && out[0].closeCode == 1003);
  CHECK_EQ(Texts(out, 2), std::string("partner:left"));

  // Emptied, the room is forgotten emptyRoomMs later.
  out.clear();
  rooms.Left(2, 2000, &out);
  rooms.Expire(2000 + 4999);
  CHECK(rooms.HasRoom(code));
  rooms.Expire(2000 + 5000);
  CHECK(!rooms.HasRoom(code));

  // A room nobody joins goes after unjoinedRoomMs.
  std::string lonely = rooms.Create(10000);
  rooms.Expire(10000 + 8999);
  CHECK(rooms.HasRoom(lonely));
  rooms.Expire(10000 + 9000);
  CHECK(!rooms.HasRoom(lonely));

  // A pairing room is made by joining it.
  out.clear();
  const std::string pair = "pair:" + std::string(64, 'a');
  CHECK_EQ(rooms.Admit(pair, 7, 20000, &out), 101);
  CHECK_EQ(rooms.Admit(pair, 8, 20000, &out), 101);
  CHECK_EQ(Texts(out, 7), std::string("partner:joined"));
  CHECK_EQ(rooms.RoomCount(), size_t{1});
}
