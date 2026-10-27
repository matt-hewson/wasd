// The live co-op link's packets and its sync logic (src/ds3/coop_sync.cpp),
// with no network: deltas, sequence numbers, resync.

namespace coop_sync_test {
// The sample after a moment of play: hurt, moved, a boss down, a pickup found.
SharedGameState Moved() {
  SharedGameState s = coop_test::Sample();
  s.hp = 400, s.stamina = 60;
  s.x += 3.5f, s.facing = -0.25f;
  s.bossesDefeated.bits[2] = true;
  s.pickupsFound.bits[1] = true, s.pickupsFound.bits[2] = false;
  return s;
}
CoopPacket Link(const std::vector<uint8_t>& b, const SharedGameState* base) {
  return DecodeCoopLinkPacket(b.data(), b.size(), base);
}
}  // namespace coop_sync_test

TEST(coop_delta_round_trip) {
  using namespace coop_sync_test;
  const SharedGameState a = coop_test::Sample(), b = Moved();
  CHECK(CoopDeltaPossible(a, b));
  auto d = EncodeCoopDelta(a, b, 8);
  CHECK(!d.empty());
  CHECK(d.size() <= 40);  // HP, stamina, position and three flips: 40 bytes, not a snapshot's hundreds
  CoopPacket p = Link(d, &a);
  CHECK(p.ok);
  CHECK(p.type == CoopMessage::kDelta);
  CHECK_EQ(p.sequence, 8u);
  CHECK(p.state == b);
  // Every group on its own, too.
  SharedGameState c = a;
  c.level = 43, c.attrs[4] = 15, c.souls = 0, c.deaths = 34, c.clearCount = 2, c.mapId = 0x1E000000,
  c.playRegionId = 300000, c.lastBonfireId = 1300000, c.havePose = false, c.maxHp = 950, c.fp = 1, c.maxFp = 1;
  c.keyItemsObtained.bits[1] = true, c.keyItemsHeld.bits[0] = false;
  CHECK(Link(EncodeCoopDelta(a, c, 2), &a).state == c);
}

TEST(coop_delta_only_when_something_changed) {
  const SharedGameState a = coop_test::Sample();
  CHECK(EncodeCoopDelta(a, a, 2).empty());
  SharedGameState tick = a;
  tick.capturedUtc += 5, tick.playSeconds += 5;  // only the clocks moved
  CHECK(EncodeCoopDelta(a, tick, 2).empty());
  tick.capturedUtc += 60, tick.playSeconds += 60;  // a minute: they go on their own
  auto d = EncodeCoopDelta(a, tick, 2);
  CHECK(!d.empty());
  CHECK(coop_sync_test::Link(d, &a).state == tick);
  SharedGameState hurt = a;
  hurt.hp = 1, hurt.capturedUtc += 1;  // a real change takes the clock along
  CHECK(coop_sync_test::Link(EncodeCoopDelta(a, hurt, 2), &a).state == hurt);
}

TEST(coop_delta_not_for_what_it_cant_carry) {
  const SharedGameState a = coop_test::Sample();
  SharedGameState b = a;
  b.characterName = "Other";
  CHECK(!CoopDeltaPossible(a, b));
  b = a, b.sessions.pop_back();
  CHECK(!CoopDeltaPossible(a, b));
  b = a, b.pickupsFound.bits.push_back(true);  // another list
  CHECK(!CoopDeltaPossible(a, b));
  b = a, b.totals[0] += 1;
  CHECK(!CoopDeltaPossible(a, b));
}

TEST(coop_link_packets_refused) {
  using namespace coop_sync_test;
  const SharedGameState a = coop_test::Sample(), b = Moved();
  auto d = EncodeCoopDelta(a, b, 3);
  CHECK(Link(d, nullptr).error.find("needs a full snapshot") != std::string::npos);
  {
    auto x = d;
    x[kCoopHeaderBytes] = 0, x[kCoopHeaderBytes + 1] = 0;  // empty mask...
    x.resize(kCoopHeaderBytes + 2);                          // ...and nothing after it
    CHECK_EQ(Link(x, &a).error, std::string("an empty change mask"));
  }
  {
    SharedGameState shorter = a;
    shorter.pickupsFound.bits.resize(1);  // the receiver's list is shorter: position 1 is past its end
    CHECK(Link(d, &shorter).error.find("past the end") != std::string::npos);
  }
  {
    auto x = d;
    x.pop_back();
    CHECK(Link(x, &a).error.find("truncated") == 0);
    x = d;
    x.push_back(0);
    CHECK(Link(x, &a).error.find("unexpected data") == 0);
  }
  {
    auto x = d;
    x[0] = 2;
    CoopPacket p = Link(x, &a);
    CHECK(!p.ok && p.version == 2);
    CHECK(p.error.find("different WASD version") != std::string::npos);
  }
  auto req = EncodeCoopRequestFull(0);
  CHECK(Link(req, nullptr).ok);
  CHECK(Link(req, nullptr).type == CoopMessage::kRequestFullSnapshot);
  req.push_back(1);
  CHECK(Link(req, nullptr).error.find("no payload") != std::string::npos);
  auto hello = CoopHeader(CoopMessage::kHello, 1);
  CHECK(Link(hello, nullptr).error.find("isn't used on the link") != std::string::npos);
  auto unknown = CoopHeader(CoopMessage::kHello, 1);
  unknown[1] = 9;
  CHECK(Link(unknown, nullptr).error.find("unknown message type") == 0);
  CHECK_EQ(DecodeCoopLinkPacket(nullptr, 0, &a).error, std::string("empty"));
  // A code still only takes a snapshot.
  CHECK(DecodeCoopPacket(d.data(), d.size()).error.find("not a snapshot") == 0);
}

TEST(coop_sequence_numbers) {
  CHECK(CoopCheckSequence(true, 5, 6, false) == CoopSeq::kApply);
  CHECK(CoopCheckSequence(true, 5, 5, false) == CoopSeq::kDuplicate);
  CHECK(CoopCheckSequence(true, 5, 4, false) == CoopSeq::kOld);
  CHECK(CoopCheckSequence(true, 5, 7, false) == CoopSeq::kGap);
  CHECK(CoopCheckSequence(false, 0, 1, false) == CoopSeq::kGap);  // a delta with nothing to apply it to
  CHECK(CoopCheckSequence(true, 0xFFFFFFFFu, 0, false) == CoopSeq::kApply);  // wraps
  CHECK(CoopCheckSequence(true, 9, 1, true) == CoopSeq::kApply);  // a snapshot always: the partner restarted
}

TEST(coop_sync_between_two_players) {
  using namespace coop_sync_test;
  CoopSync me, them;
  auto deliver = [&](const std::vector<uint8_t>& packet, CoopSync& to) { return to.Incoming(packet.data(), packet.size()); };
  SharedGameState s = coop_test::Sample();

  auto first = me.Outgoing(s);  // the first packet is everything
  CHECK(first.size() > 100 && first[1] == static_cast<uint8_t>(CoopMessage::kFullSnapshot));
  CHECK(deliver(first, them).empty());
  CHECK(them.PartnerState().have && them.PartnerState().state == s);
  CHECK(me.Outgoing(s).empty());  // unchanged: nothing

  SharedGameState moved = Moved();
  auto d = me.Outgoing(moved);
  CHECK(d[1] == static_cast<uint8_t>(CoopMessage::kDelta));
  deliver(d, them);
  CHECK(them.PartnerState().state == moved);
  deliver(d, them);  // a duplicate changes nothing
  CHECK(them.PartnerState().state == moved);

  moved.hp = 10;
  me.Outgoing(moved);  // lost on the way
  moved.hp = 20;
  auto afterGap = me.Outgoing(moved);
  auto request = deliver(afterGap, them);  // a gap: not applied, asks for everything
  CHECK(!request.empty() && request[1] == static_cast<uint8_t>(CoopMessage::kRequestFullSnapshot));
  CHECK(!them.PartnerState().have);
  moved.hp = 30;
  CHECK(deliver(me.Outgoing(moved), them).empty());  // still behind; it asked already, so it doesn't ask again
  CHECK(deliver(request, me).empty());
  auto full = me.Outgoing(moved);  // answered with everything...
  CHECK(full[1] == static_cast<uint8_t>(CoopMessage::kFullSnapshot));
  deliver(full, them);
  CHECK(them.PartnerState().have && them.PartnerState().state == moved);
  moved.hp = 40;
  deliver(me.Outgoing(moved), them);  // ...and deltas again after it
  CHECK(them.PartnerState().state.hp == 40);

  me.Resync();  // a reconnect: everything again, though nothing changed
  CHECK(me.Outgoing(moved)[1] == static_cast<uint8_t>(CoopMessage::kFullSnapshot));
  CHECK_EQ(me.Stats().fullSent, uint64_t(3));
  CHECK_EQ(me.Stats().deltasSent, uint64_t(5));  // the lost one counts: it was sent
  CHECK_EQ(them.Stats().requestsSent, uint64_t(1));
  CHECK_EQ(them.FullReceived(), uint64_t(2));

  them.PartnerGone();  // they left: their state isn't current, and a delta can't apply
  CHECK(!them.PartnerState().have);
}

TEST(coop_sync_stops_on_another_protocol_version) {
  CoopSync me, them;
  auto packet = me.Outgoing(coop_test::Sample());
  packet[0] = 2;
  CHECK(them.Incoming(packet.data(), packet.size()).empty());
  CHECK(them.VersionMismatch());
  CHECK(!them.PartnerState().have);
  packet[0] = kCoopProtocolVersion;  // nothing is applied after that, even a good one
  them.Incoming(packet.data(), packet.size());
  CHECK(!them.PartnerState().have);
}
