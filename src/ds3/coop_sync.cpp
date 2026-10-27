// src/ds3/coop_sync.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- co-op over the relay: deltas, sequence numbers, resync -----------
//
// On the live link (the relay; core/relay_client.cpp) each packet has the
// same 6-byte header as a code's (version, message type, sequence). A code
// only ever holds a FullSnapshot; the link also uses, in protocol 1:
//
//   Delta (3): what changed since the packet before it. Payload, bit-packed
//   like a snapshot's: a change mask (16 bits), then for each set bit, in
//   bit order:
//     0  captured at, Unix seconds: 32
//     1  soul level 16, attributes 9 x 8
//     2  HP, max HP: 16 each        3  FP, max FP        4  stamina, max stamina
//     5  souls held 32              6  deaths 32         7  play time, seconds 32
//     8  NG cycle 8                 9  map id 32, play region id 32
//     10 last bonfire id 32         11 position known 1; if set x, y, z, facing (f32)
//     12-15 the four flag sets (bosses defeated, key items obtained, key items
//        held, world pickups found): a count 12, then that many positions
//        12 bits each, every one of them flipped
//   then zero bits up to the byte boundary. The mask is never 0. A delta
//   applies only to the state of the packet just before it: its sequence
//   number must be exactly one more.
//
//   RequestFullSnapshot (5): no payload, sequence 0 (it isn't part of the
//   state stream); "send me everything", after a gap or a bad delta.
//
// Everything else in the state (the name, the flag lists themselves, areas,
// levels, totals, sessions, boss records, records) changes rarely and goes
// as a FullSnapshot. So does the first packet after a (re)connect or a
// request, and a delta that would be bigger than kCoopMaxPacketBytes.
// Sending is change-driven: an unchanged state sends nothing. The clock
// fields (captured at, play time) tick every second, so on their own they
// go at most once a minute; otherwise they ride along with a real change.

constexpr uint32_t kCoopClockEverySec = 60;

enum CoopDeltaBit {
  kDeltaCaptured = 0, kDeltaLevel, kDeltaHp, kDeltaFp, kDeltaStamina, kDeltaSouls, kDeltaDeaths, kDeltaPlayTime,
  kDeltaNg, kDeltaPlace, kDeltaBonfire, kDeltaPose, kDeltaFlags0  // 12-15
};

const CoopFlagSet* CoopFlagSetAt(const SharedGameState& s, int k) {
  const CoopFlagSet* sets[] = {&s.bossesDefeated, &s.keyItemsObtained, &s.keyItemsHeld, &s.pickupsFound};
  return sets[k];
}
CoopFlagSet* CoopFlagSetAt(SharedGameState& s, int k) {
  return const_cast<CoopFlagSet*>(CoopFlagSetAt(static_cast<const SharedGameState&>(s), k));
}

// Whether `to` can be sent as a delta on `from`: everything a delta can't
// carry is the same.
bool CoopDeltaPossible(const SharedGameState& from, const SharedGameState& to) {
  for (int k = 0; k < 4; ++k) {
    const CoopFlagSet *a = CoopFlagSetAt(from, k), *b = CoopFlagSetAt(to, k);
    if (a->fingerprint != b->fingerprint || a->bits.size() != b->bits.size() || b->bits.size() > kCoopMaxFlags)
      return false;
  }
  return from.characterName == to.characterName && from.areaListFingerprint == to.areaListFingerprint &&
         from.areas == to.areas && from.levels == to.levels && std::equal(from.totals, from.totals + 6, to.totals) &&
         from.sessionAreaBits == to.sessionAreaBits && from.sessionWeapons == to.sessionWeapons &&
         from.sessions == to.sessions && from.bossRecords == to.bossRecords && from.lifeSec == to.lifeSec &&
         from.lifeSession == to.lifeSession && from.lifeArea == to.lifeArea && from.firstDeathAt == to.firstDeathAt &&
         from.firstDeathSession == to.firstDeathSession && from.firstDeathArea == to.firstDeathArea;
}

std::vector<uint8_t> CoopHeader(CoopMessage type, uint32_t sequence) {
  std::vector<uint8_t> out = {kCoopProtocolVersion, static_cast<uint8_t>(type)};
  for (int k = 0; k < 4; ++k) out.push_back(static_cast<uint8_t>(sequence >> (8 * k)));
  return out;
}

// The delta from `from` to `to` (CoopDeltaPossible must hold), or nothing
// when nothing worth sending changed.
std::vector<uint8_t> EncodeCoopDelta(const SharedGameState& from, const SharedGameState& to, uint32_t sequence) {
  auto poseDiffers = [&] {
    if (from.havePose != to.havePose) return true;
    return to.havePose && (from.x != to.x || from.y != to.y || from.z != to.z || from.facing != to.facing);
  };
  uint32_t mask = 0;
  if (from.level != to.level || !std::equal(from.attrs, from.attrs + 9, to.attrs)) mask |= 1u << kDeltaLevel;
  if (from.hp != to.hp || from.maxHp != to.maxHp) mask |= 1u << kDeltaHp;
  if (from.fp != to.fp || from.maxFp != to.maxFp) mask |= 1u << kDeltaFp;
  if (from.stamina != to.stamina || from.maxStamina != to.maxStamina) mask |= 1u << kDeltaStamina;
  if (from.souls != to.souls) mask |= 1u << kDeltaSouls;
  if (from.deaths != to.deaths) mask |= 1u << kDeltaDeaths;
  if (from.clearCount != to.clearCount) mask |= 1u << kDeltaNg;
  if (from.mapId != to.mapId || from.playRegionId != to.playRegionId) mask |= 1u << kDeltaPlace;
  if (from.lastBonfireId != to.lastBonfireId) mask |= 1u << kDeltaBonfire;
  if (poseDiffers()) mask |= 1u << kDeltaPose;
  for (int k = 0; k < 4; ++k)
    if (CoopFlagSetAt(from, k)->bits != CoopFlagSetAt(to, k)->bits) mask |= 1u << (kDeltaFlags0 + k);
  // The clocks: along with a real change, or on their own once a minute.
  bool clocksDue = to.playSeconds >= from.playSeconds + kCoopClockEverySec ||
                   to.capturedUtc >= from.capturedUtc + kCoopClockEverySec;
  if (mask || clocksDue) {
    if (from.capturedUtc != to.capturedUtc) mask |= 1u << kDeltaCaptured;
    if (from.playSeconds != to.playSeconds) mask |= 1u << kDeltaPlayTime;
  }
  if (!mask) return {};
  std::vector<uint8_t> out = CoopHeader(CoopMessage::kDelta, sequence);
  CoopBitWriter w;
  auto f32 = [&](float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    w.Write(bits, 32);
  };
  w.Write(mask, 16);
  if (mask & (1u << kDeltaCaptured)) w.Write(to.capturedUtc, 32);
  if (mask & (1u << kDeltaLevel)) {
    w.Write(to.level, 16);
    for (uint8_t a : to.attrs) w.Write(a, 8);
  }
  if (mask & (1u << kDeltaHp)) w.Write(to.hp, 16), w.Write(to.maxHp, 16);
  if (mask & (1u << kDeltaFp)) w.Write(to.fp, 16), w.Write(to.maxFp, 16);
  if (mask & (1u << kDeltaStamina)) w.Write(to.stamina, 16), w.Write(to.maxStamina, 16);
  if (mask & (1u << kDeltaSouls)) w.Write(to.souls, 32);
  if (mask & (1u << kDeltaDeaths)) w.Write(to.deaths, 32);
  if (mask & (1u << kDeltaPlayTime)) w.Write(to.playSeconds, 32);
  if (mask & (1u << kDeltaNg)) w.Write(to.clearCount, 8);
  if (mask & (1u << kDeltaPlace)) w.Write(to.mapId, 32), w.Write(static_cast<uint32_t>(to.playRegionId), 32);
  if (mask & (1u << kDeltaBonfire)) w.Write(static_cast<uint32_t>(to.lastBonfireId), 32);
  if (mask & (1u << kDeltaPose)) {
    w.WriteBool(to.havePose);
    if (to.havePose) f32(to.x), f32(to.y), f32(to.z), f32(to.facing);
  }
  for (int k = 0; k < 4; ++k) {
    if (!(mask & (1u << (kDeltaFlags0 + k)))) continue;
    const auto &a = CoopFlagSetAt(from, k)->bits, &b = CoopFlagSetAt(to, k)->bits;
    std::vector<uint32_t> flips;
    for (size_t i = 0; i < b.size(); ++i)
      if (a[i] != b[i]) flips.push_back(static_cast<uint32_t>(i));
    w.Write(static_cast<uint32_t>(flips.size()), 12);
    for (uint32_t i : flips) w.Write(i, 12);
  }
  out.insert(out.end(), w.Bytes().begin(), w.Bytes().end());
  return out;
}

std::vector<uint8_t> EncodeCoopRequestFull(uint32_t sequence) {
  return CoopHeader(CoopMessage::kRequestFullSnapshot, sequence);
}

// A packet from the live link. A FullSnapshot decodes as from a code; a
// Delta is applied to `base` (the partner's state so far, which it needs)
// and gives the whole new state; a RequestFullSnapshot carries nothing.
// Strict, like the snapshot decoder: refused with a reason, never half-applied.
CoopPacket DecodeCoopLinkPacket(const uint8_t* data, size_t size, const SharedGameState* base) {
  if (size >= kCoopHeaderBytes && size <= kCoopMaxPacketBytes && data[0] == kCoopProtocolVersion &&
      data[1] == static_cast<uint8_t>(CoopMessage::kFullSnapshot))
    return DecodeCoopPacket(data, size);
  CoopPacket p = DecodeCoopPacket(data, size);  // the header checks and their reasons
  if (p.error.rfind("not a snapshot", 0) != 0) return p;
  p.error.clear();
  if (p.type == CoopMessage::kRequestFullSnapshot) {
    if (size != kCoopHeaderBytes) p.error = "a request carries no payload";
    p.ok = p.error.empty();
    return p;
  }
  if (p.type != CoopMessage::kDelta) {
    p.error = "message type " + std::to_string(data[1]) + " isn't used on the link";
    return p;
  }
  if (!base) {
    p.error = "a delta needs a full snapshot first";
    return p;
  }
  CoopBitReader r(data + kCoopHeaderBytes, size - kCoopHeaderBytes);
  auto f32 = [&]() {
    uint32_t bits = r.Read(32);
    float f = 0;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
  };
  SharedGameState s = *base;
  const uint32_t mask = r.Read(16);
  if (r.Ok() && mask == 0) {
    p.error = "an empty change mask";
    return p;
  }
  if (mask & (1u << kDeltaCaptured)) s.capturedUtc = r.Read(32);
  if (mask & (1u << kDeltaLevel)) {
    s.level = static_cast<uint16_t>(r.Read(16));
    for (uint8_t& a : s.attrs) a = static_cast<uint8_t>(r.Read(8));
  }
  if (mask & (1u << kDeltaHp)) s.hp = static_cast<uint16_t>(r.Read(16)), s.maxHp = static_cast<uint16_t>(r.Read(16));
  if (mask & (1u << kDeltaFp)) s.fp = static_cast<uint16_t>(r.Read(16)), s.maxFp = static_cast<uint16_t>(r.Read(16));
  if (mask & (1u << kDeltaStamina))
    s.stamina = static_cast<uint16_t>(r.Read(16)), s.maxStamina = static_cast<uint16_t>(r.Read(16));
  if (mask & (1u << kDeltaSouls)) s.souls = r.Read(32);
  if (mask & (1u << kDeltaDeaths)) s.deaths = r.Read(32);
  if (mask & (1u << kDeltaPlayTime)) s.playSeconds = r.Read(32);
  if (mask & (1u << kDeltaNg)) s.clearCount = static_cast<uint8_t>(r.Read(8));
  if (mask & (1u << kDeltaPlace)) s.mapId = r.Read(32), s.playRegionId = static_cast<int32_t>(r.Read(32));
  if (mask & (1u << kDeltaBonfire)) s.lastBonfireId = static_cast<int32_t>(r.Read(32));
  if (mask & (1u << kDeltaPose)) {
    s.havePose = r.ReadBool();
    if (s.havePose) {
      s.x = f32(), s.y = f32(), s.z = f32(), s.facing = f32();
      if (r.Ok() && (!std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(s.z) || !std::isfinite(s.facing))) {
        p.error = "position isn't a finite number";
        return p;
      }
    }
  }
  for (int k = 0; k < 4; ++k) {
    if (!(mask & (1u << (kDeltaFlags0 + k)))) continue;
    std::vector<bool>& bits = CoopFlagSetAt(s, k)->bits;
    uint32_t n = r.Read(12);
    for (uint32_t i = 0; i < n && r.Ok(); ++i) {
      uint32_t at = r.Read(12);
      if (r.Ok() && at >= bits.size()) {
        p.error = "a flag position past the end of its list";
        return p;
      }
      if (r.Ok()) bits[at] = !bits[at];
    }
  }
  if (!r.Ok()) {
    p.error = "truncated (the delta ends early)";
    return p;
  }
  if (!r.AtCleanEnd()) {
    p.error = "unexpected data after the delta";
    return p;
  }
  p.state = std::move(s);
  p.ok = true;
  return p;
}

// What to do with a packet's sequence number. A FullSnapshot always applies
// (the partner may have restarted and begun again from 1); a Delta only on
// exactly the state before it.
enum class CoopSeq { kApply, kDuplicate, kOld, kGap };
CoopSeq CoopCheckSequence(bool haveState, uint32_t last, uint32_t incoming, bool full) {
  if (full) return CoopSeq::kApply;
  if (!haveState) return CoopSeq::kGap;
  if (incoming == last) return CoopSeq::kDuplicate;
  if (static_cast<int32_t>(incoming - last) < 0) return CoopSeq::kOld;
  return incoming == last + 1 ? CoopSeq::kApply : CoopSeq::kGap;
}

struct CoopSyncStats {
  uint64_t packetsSent = 0, packetsReceived = 0, bytesSent = 0, bytesReceived = 0;
  uint64_t deltasSent = 0, fullSent = 0, requestsSent = 0, rejected = 0;
  size_t lastPacketSize = 0;
};

// One side of a live co-op link, with no network in it: what to send for
// our state, and the partner's state from what arrives. CoopLink feeds it
// from the relay.
class CoopSync {
 public:
  struct Partner {
    bool have = false;         // a state has arrived (and is still current)
    SharedGameState state;
    uint32_t sequence = 0;     // of the last packet applied
    ULONGLONG updatedTick = 0; // when it was last applied
  };

  // The next packet sends everything (a (re)connect, a new partner, a request).
  void Resync() { needFull_ = true; }

  // What to send for our state now: a full snapshot, a delta, or nothing
  // when nothing changed.
  std::vector<uint8_t> Outgoing(const SharedGameState& now) {
    std::vector<uint8_t> packet;
    bool full = needFull_ || !haveSent_ || !CoopDeltaPossible(lastSent_, now);
    if (!full) {
      packet = EncodeCoopDelta(lastSent_, now, seq_ + 1);
      if (packet.empty()) return {};
      if (packet.size() > kCoopMaxPacketBytes) full = true;
    }
    if (full) {
      std::string code = FitCoopCode(now, seq_ + 1, nullptr), error;
      packet.clear();
      if (!CoopFromText(code, &packet, &error)) return {};
      ++stats_.fullSent;
      needFull_ = false;
    } else {
      ++stats_.deltasSent;
    }
    ++seq_;
    lastSent_ = now;
    haveSent_ = true;
    Count(packet);
    return packet;
  }

  // A packet from the partner. Returns a packet to send back (a request for
  // everything after a gap or a bad delta), or nothing.
  std::vector<uint8_t> Incoming(const uint8_t* data, size_t size) {
    ++stats_.packetsReceived;
    stats_.bytesReceived += size;
    if (mismatch_) return {};
    bool full = size >= 2 && data[1] == static_cast<uint8_t>(CoopMessage::kFullSnapshot);
    bool delta = size >= 2 && data[1] == static_cast<uint8_t>(CoopMessage::kDelta);
    if (delta && size >= kCoopHeaderBytes && data[0] == kCoopProtocolVersion) {
      uint32_t seq = static_cast<uint32_t>(data[2]) | static_cast<uint32_t>(data[3]) << 8 |
                     static_cast<uint32_t>(data[4]) << 16 | static_cast<uint32_t>(data[5]) << 24;
      CoopSeq check = CoopCheckSequence(partner_.have, partner_.sequence, seq, false);
      if (check == CoopSeq::kDuplicate || check == CoopSeq::kOld) return {};
      if (check == CoopSeq::kGap) return RequestFull("sequence gap");
    }
    CoopPacket p = DecodeCoopLinkPacket(data, size, partner_.have ? &partner_.state : nullptr);
    if (!p.ok) {
      ++stats_.rejected;
      lastError_ = p.error;
      if (p.version && p.version != kCoopProtocolVersion) {
        mismatch_ = true;
        partnerVersion_ = p.version;
        return {};
      }
      return delta || full ? RequestFull("bad packet: " + p.error) : std::vector<uint8_t>{};
    }
    if (p.type == CoopMessage::kRequestFullSnapshot) {
      Resync();
      ++requestsReceived_;
      return {};
    }
    partner_.have = true;
    partner_.state = std::move(p.state);
    partner_.sequence = p.sequence;
    partner_.updatedTick = GetTickCount64();
    if (full) ++fullReceived_, requestPending_ = false;
    return {};
  }

  // The partner left or the link dropped: their state is no longer current,
  // and the next one must be a full snapshot.
  void PartnerGone() {
    partner_.have = false;
    requestPending_ = false;
  }

  const Partner& PartnerState() const { return partner_; }
  bool VersionMismatch() const { return mismatch_; }
  uint8_t PartnerVersion() const { return partnerVersion_; }  // after a mismatch: the version they sent
  const std::string& LastError() const { return lastError_; }
  const CoopSyncStats& Stats() const { return stats_; }
  uint64_t FullReceived() const { return fullReceived_; }
  uint64_t RequestsReceived() const { return requestsReceived_; }

 private:
  std::vector<uint8_t> RequestFull(const std::string& why) {
    lastError_ = why;
    partner_.have = false;  // don't build on a state we know is behind
    if (requestPending_) return {};
    requestPending_ = true;
    ++stats_.requestsSent;
    std::vector<uint8_t> packet = EncodeCoopRequestFull(0);  // no sequence: our deltas keep counting
    Count(packet);
    return packet;
  }
  void Count(const std::vector<uint8_t>& packet) {
    ++stats_.packetsSent;
    stats_.bytesSent += packet.size();
    stats_.lastPacketSize = packet.size();
  }

  SharedGameState lastSent_;
  bool haveSent_ = false, needFull_ = true, requestPending_ = false, mismatch_ = false;
  uint32_t seq_ = 0;
  uint8_t partnerVersion_ = 0;
  Partner partner_;
  CoopSyncStats stats_;
  uint64_t fullReceived_ = 0, requestsReceived_ = 0;
  std::string lastError_;
};
