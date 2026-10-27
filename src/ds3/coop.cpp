// src/ds3/coop.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- window mode (Milestone 3) ---------------------------------------
//
// A small, separate, always-on-top companion window -- not drawn over
// the game (that's Phase 2) -- showing live values plus the brief's
// metrics: last-read time, read latency, measured poll rate, and this
// process's own CPU and memory.
//
// Two threads, so neither can stall the other:
//   - a polling worker does every ReadProcessMemory call on the same
//     fixed-deadline 10Hz schedule as `stats --live`, fills a shared
//     snapshot under a lock, and posts kMsgSnapshot to the window;
//   - the UI thread only draws the latest snapshot. Dragging the window
//     (a modal loop) therefore never pauses the readings.
// F10 (the same keyboard hook as `stats --live`) posts kMsgTogglePause
// to the window; while paused the worker blocks on an event, so it
// makes zero reads and uses no CPU.
constexpr UINT kMsgSnapshot = WM_APP + 2;  // worker -> window: new snapshot ready
constexpr wchar_t kWindowClassName[] = L"WasdWindow";

// ---- live page data (overlay mode) -------------------------------------
//
// Structured copies of what the live page shows, filled by the polling
// worker and served as JSON by the page server (see LiveServerMain).
// ---- co-op: the shared state and its binary form -----------------------
//
// What one player's tool shares with their partner's: a small, versioned
// model (SharedGameState) of values the reader already has, never the
// reader's own structs or anything from process memory as such. It travels
// as a binary packet:
//
//   byte 0      protocol version (kCoopProtocolVersion)
//   byte 1      message type (CoopMessage)
//   bytes 2-5   sequence number, u32 little-endian
//   bytes 6..   payload, bit-packed, least significant bit first
//
// For now partners swap a FullSnapshot by copy and paste: the packet plus
// its CRC-32 (little-endian), in base64, after "WASD-" (CoopToText).
// The live link is meant to send the same packets as binary frames.
//
// FullSnapshot payload, protocol 1 (field: bits):
//   captured at, Unix seconds UTC: 32
//   character name: 6 (n = bytes of UTF-8, at most 48), then n x 8
//   soul level: 16; HP, max HP, FP, max FP, stamina, max stamina: 16 each
//   attributes (Vigor, Attunement, Endurance, Vitality, Strength, Dexterity,
//     Intelligence, Faith, Luck): 8 each
//   souls held: 32; deaths (the game's counter): 32; play time, seconds: 32;
//     NG cycle (0 = first journey): 8
//   map id (mAA_BB_CC_DD packed): 32; play region id: 32; last bonfire id: 32
//   position known: 1; if set x, y, z, facing (radians): 32 each, IEEE float
//   four flag sets, in this order: bosses defeated, key items obtained,
//     key items held, world pickups found. Each: list fingerprint 32,
//     count 12, then count bits
//   recorded time and deaths per area (from the saved sessions): area list
//     fingerprint 32, count 6, then per area: index in the list 6,
//     seconds 24, deaths 16
//   level over play time: count 6, then per point: play time in minutes 20,
//     level 10
//   recorded totals: seconds, deaths, souls lost, souls recovered, souls
//     gained, longest session in seconds: 32 each
//   sessions, newest first: count 6, area bits m 6, weapons 1; then per
//     session: start (Unix minutes) 28, length in minutes 12, deaths 12,
//     souls gained 24, level at start 10, at end 10, the areas visited (m
//     bits, one per area-list entry), bosses killed 6, and if weapons:
//     main hand weapon id / 100 18, its upgrade 5, off hand the same
//   boss records: count 6, then per boss killed in a recorded session: its
//     index in the boss list 6, deaths before the kill 10, minutes from the
//     first attempt to the kill 16 (65535: no attempts), the session it fell
//     in 6 (an index into the sessions above; 63: not among them), kill
//     time (Unix minutes) 28
//   records: longest life in seconds 24, its session 6, its area 6 (63:
//     none); first death, seconds into the recorded time 24 (16777215:
//     none), its session 6, its area 6
//   zero bits up to the byte boundary, and nothing after.
// The sessions and boss records let a receiver work out the rest of the
// pair report (sessions played together, per-session figures, first-try
// and fastest kills); the code only carries what it couldn't.
// A flag set's bits follow one of the tool's lists (bosses.tsv, key_items.tsv,
// the pickups read from the game); the fingerprint (FNV-1a of the list's ids
// and length) tells the receiver whether its own list is the same one. The
// area list is region_areas.tsv's area names, sorted (CoopAreaList).
// A layout, once released, never changes: a new layout is a new version.
constexpr uint8_t kCoopProtocolVersion = 1;
std::string ClockDirection(const PlayerPose& pose, float dx, float dz);
enum class CoopMessage : uint8_t {
  kHello = 1,
  kFullSnapshot = 2,
  kDelta = 3,
  kHeartbeat = 4,
  kRequestFullSnapshot = 5,
  kDisconnect = 6,
};
constexpr size_t kCoopHeaderBytes = 6;
constexpr size_t kCoopMaxPacketBytes = 2048;
constexpr size_t kCoopMaxNameBytes = 48;
constexpr size_t kCoopMaxFlags = 4095;  // 12-bit count
constexpr size_t kCoopMaxAreas = 63;    // 6-bit count and index
constexpr size_t kCoopMaxLevelPoints = 32;
constexpr size_t kCoopMaxSessions = 40;     // newest first; FitCoopCode may send fewer
constexpr uint8_t kCoopNone6 = 63;          // a 6-bit index meaning "none"
constexpr size_t kCoopMaxCodeChars = 2000;  // one chat message (Discord's limit)
constexpr const char* kCoopTextPrefix = "WASD-";

class CoopBitWriter {
 public:
  void Write(uint32_t value, int bits) {  // the low `bits` bits of value, 1-32
    for (int k = 0; k < bits; ++k) {
      if (bitCount_ % 8 == 0) bytes_.push_back(0);
      if ((value >> k) & 1u) bytes_.back() = static_cast<uint8_t>(bytes_.back() | (1u << (bitCount_ % 8)));
      ++bitCount_;
    }
  }
  void WriteBool(bool v) { Write(v ? 1u : 0u, 1); }
  const std::vector<uint8_t>& Bytes() const { return bytes_; }
  size_t BitCount() const { return bitCount_; }

 private:
  std::vector<uint8_t> bytes_;
  size_t bitCount_ = 0;
};

class CoopBitReader {
 public:
  CoopBitReader(const uint8_t* data, size_t size) : data_(data), bits_(size * 8) {}
  // The next `bits` bits (1-32); 0 and !Ok() once past the end.
  uint32_t Read(int bits) {
    if (pos_ + static_cast<size_t>(bits) > bits_) {
      ok_ = false;
      pos_ = bits_;
      return 0;
    }
    uint32_t v = 0;
    for (int k = 0; k < bits; ++k, ++pos_)
      if ((data_[pos_ / 8] >> (pos_ % 8)) & 1u) v |= 1u << k;
    return v;
  }
  bool ReadBool() { return Read(1) != 0; }
  bool Ok() const { return ok_; }
  // Only zero padding is left, and less than a byte of it.
  bool AtCleanEnd() const {
    if (!ok_ || bits_ - pos_ >= 8) return false;
    for (size_t p = pos_; p < bits_; ++p)
      if ((data_[p / 8] >> (p % 8)) & 1u) return false;
    return true;
  }

 private:
  const uint8_t* data_;
  size_t bits_, pos_ = 0;
  bool ok_ = true;
};

// Booleans over one of the tool's lists, in list order.
struct CoopFlagSet {
  uint32_t fingerprint = 0;
  std::vector<bool> bits;
  bool operator==(const CoopFlagSet& o) const { return fingerprint == o.fingerprint && bits == o.bits; }
  int CountOn() const { return static_cast<int>(std::count(bits.begin(), bits.end(), true)); }
};

uint32_t CoopFingerprint(const std::vector<uint32_t>& ids) {
  uint32_t h = 2166136261u;
  auto mix = [&](uint32_t v) {
    for (int k = 0; k < 4; ++k) h = (h ^ ((v >> (8 * k)) & 0xFFu)) * 16777619u;
  };
  mix(static_cast<uint32_t>(ids.size()));
  for (uint32_t id : ids) mix(id);
  return h;
}

// Recorded play in one area (an index into CoopAreaList), summed over the
// saved sessions.
struct CoopAreaEntry {
  uint8_t index = 0;
  uint32_t seconds = 0;  // 24 bits on the wire
  uint16_t deaths = 0;
  bool operator==(const CoopAreaEntry& o) const {
    return index == o.index && seconds == o.seconds && deaths == o.deaths;
  }
};
struct CoopLevelPoint {
  uint32_t minutes = 0;  // play time, 20 bits on the wire
  uint16_t level = 0;    // 10 bits
  bool operator==(const CoopLevelPoint& o) const { return minutes == o.minutes && level == o.level; }
};

// One recorded session, as the code sends it.
struct CoopSessionSummary {
  uint32_t startMin = 0;  // Unix minutes (UTC); 0 = unknown
  uint16_t minutes = 0, deaths = 0;
  uint32_t gained = 0;
  uint16_t levelStart = 0, levelEnd = 0;
  uint64_t areaBits = 0;  // bit k: area-list entry k visited
  uint8_t kills = 0;
  uint32_t weapon[2] = {};  // main, off hand: the weapon held longest, param id / 100 (0 = none)
  uint8_t upgrade[2] = {};
  bool operator==(const CoopSessionSummary& o) const {
    return startMin == o.startMin && minutes == o.minutes && deaths == o.deaths && gained == o.gained &&
           levelStart == o.levelStart && levelEnd == o.levelEnd && areaBits == o.areaBits && kills == o.kills &&
           weapon[0] == o.weapon[0] && weapon[1] == o.weapon[1] && upgrade[0] == o.upgrade[0] &&
           upgrade[1] == o.upgrade[1];
  }
};
// A boss killed in a recorded session.
struct CoopBossRecord {
  uint8_t boss = 0;         // index in the boss list (the bosses-defeated flag set's)
  uint16_t deaths = 0;      // attempts before the kill
  uint16_t ttkMin = 0xFFFF;  // minutes from the first attempt to the kill; 0xFFFF = no attempts
  uint8_t session = kCoopNone6;  // index into sessions; kCoopNone6 = not among them
  uint32_t killMin = 0;     // Unix minutes
  bool operator==(const CoopBossRecord& o) const {
    return boss == o.boss && deaths == o.deaths && ttkMin == o.ttkMin && session == o.session && killMin == o.killMin;
  }
};

struct SharedGameState {
  uint32_t capturedUtc = 0;
  std::string characterName;  // identifying: anything that records this state must strip it
  uint16_t level = 0;
  uint16_t hp = 0, maxHp = 0, fp = 0, maxFp = 0, stamina = 0, maxStamina = 0;
  uint8_t attrs[9] = {};  // Vigor ... Luck
  uint32_t souls = 0, deaths = 0, playSeconds = 0;
  uint8_t clearCount = 0;  // NG cycle
  uint32_t mapId = 0;
  int32_t playRegionId = 0;
  int32_t lastBonfireId = -1;
  bool havePose = false;
  float x = 0, y = 0, z = 0, facing = 0;
  CoopFlagSet bossesDefeated, keyItemsObtained, keyItemsHeld, pickupsFound;
  uint32_t areaListFingerprint = 0;
  std::vector<CoopAreaEntry> areas;      // only areas with recorded time or deaths, in the order first visited
  std::vector<CoopLevelPoint> levels;    // oldest first
  // Recorded seconds, deaths, souls lost, recovered, gained, longest session (s).
  uint32_t totals[6] = {};
  uint8_t sessionAreaBits = 0;  // how many area bits each session carries (the area list's size)
  bool sessionWeapons = false;  // whether the sessions carry weapons
  std::vector<CoopSessionSummary> sessions;  // newest first
  std::vector<CoopBossRecord> bossRecords;
  uint32_t lifeSec = 0;  // the longest stretch of a session without dying
  uint8_t lifeSession = kCoopNone6, lifeArea = kCoopNone6;
  uint32_t firstDeathAt = 0xFFFFFF;  // seconds into the recorded time; 0xFFFFFF = none
  uint8_t firstDeathSession = kCoopNone6, firstDeathArea = kCoopNone6;

  bool operator==(const SharedGameState& o) const {
    return capturedUtc == o.capturedUtc && characterName == o.characterName && level == o.level && hp == o.hp &&
           maxHp == o.maxHp && fp == o.fp && maxFp == o.maxFp && stamina == o.stamina &&
           maxStamina == o.maxStamina && std::equal(attrs, attrs + 9, o.attrs) && souls == o.souls &&
           deaths == o.deaths && playSeconds == o.playSeconds && clearCount == o.clearCount &&
           mapId == o.mapId && playRegionId == o.playRegionId &&
           lastBonfireId == o.lastBonfireId && havePose == o.havePose &&
           (!havePose || (x == o.x && y == o.y && z == o.z && facing == o.facing)) &&
           bossesDefeated == o.bossesDefeated && keyItemsObtained == o.keyItemsObtained &&
           keyItemsHeld == o.keyItemsHeld && pickupsFound == o.pickupsFound &&
           areaListFingerprint == o.areaListFingerprint && areas == o.areas && levels == o.levels &&
           std::equal(totals, totals + 6, o.totals) && sessionAreaBits == o.sessionAreaBits &&
           sessionWeapons == o.sessionWeapons && sessions == o.sessions && bossRecords == o.bossRecords &&
           lifeSec == o.lifeSec && lifeSession == o.lifeSession && lifeArea == o.lifeArea &&
           firstDeathAt == o.firstDeathAt && firstDeathSession == o.firstDeathSession &&
           firstDeathArea == o.firstDeathArea;
  }
};

uint16_t CoopClampU16(int64_t v) { return static_cast<uint16_t>(v < 0 ? 0 : v > 0xFFFF ? 0xFFFF : v); }

// Cut to at most `maxBytes` without splitting a UTF-8 character.
std::string CoopTruncateUtf8(const std::string& s, size_t maxBytes) {
  if (s.size() <= maxBytes) return s;
  size_t n = maxBytes;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  return s.substr(0, n);
}

std::vector<uint8_t> EncodeCoopSnapshot(const SharedGameState& s, uint32_t sequence) {
  std::vector<uint8_t> out = {kCoopProtocolVersion, static_cast<uint8_t>(CoopMessage::kFullSnapshot)};
  for (int k = 0; k < 4; ++k) out.push_back(static_cast<uint8_t>(sequence >> (8 * k)));
  CoopBitWriter w;
  auto f32 = [&](float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    w.Write(bits, 32);
  };
  w.Write(s.capturedUtc, 32);
  std::string name = CoopTruncateUtf8(s.characterName, kCoopMaxNameBytes);
  w.Write(static_cast<uint32_t>(name.size()), 6);
  for (unsigned char c : name) w.Write(c, 8);
  for (uint16_t v : {s.level, s.hp, s.maxHp, s.fp, s.maxFp, s.stamina, s.maxStamina}) w.Write(v, 16);
  for (uint8_t a : s.attrs) w.Write(a, 8);
  w.Write(s.souls, 32);
  w.Write(s.deaths, 32);
  w.Write(s.playSeconds, 32);
  w.Write(s.clearCount, 8);
  w.Write(s.mapId, 32);
  w.Write(static_cast<uint32_t>(s.playRegionId), 32);
  w.Write(static_cast<uint32_t>(s.lastBonfireId), 32);
  w.WriteBool(s.havePose);
  if (s.havePose) {
    f32(s.x);
    f32(s.y);
    f32(s.z);
    f32(s.facing);
  }
  for (const CoopFlagSet* set : {&s.bossesDefeated, &s.keyItemsObtained, &s.keyItemsHeld, &s.pickupsFound}) {
    size_t n = (std::min)(set->bits.size(), kCoopMaxFlags);
    w.Write(set->fingerprint, 32);
    w.Write(static_cast<uint32_t>(n), 12);
    for (size_t k = 0; k < n; ++k) w.WriteBool(set->bits[k]);
  }
  w.Write(s.areaListFingerprint, 32);
  std::vector<CoopAreaEntry> areas;
  for (const auto& a : s.areas)
    if (a.index < kCoopMaxAreas && areas.size() < kCoopMaxAreas) areas.push_back(a);
  w.Write(static_cast<uint32_t>(areas.size()), 6);
  for (const auto& a : areas) {
    w.Write(a.index, 6);
    w.Write((std::min)(a.seconds, 0xFFFFFFu), 24);
    w.Write(a.deaths, 16);
  }
  size_t levels = (std::min)(s.levels.size(), kCoopMaxLevelPoints);
  w.Write(static_cast<uint32_t>(levels), 6);
  for (size_t k = 0; k < levels; ++k) {
    w.Write((std::min)(s.levels[k].minutes, 0xFFFFFu), 20);
    w.Write((std::min)(s.levels[k].level, uint16_t{1023}), 10);
  }
  for (uint32_t v : s.totals) w.Write(v, 32);
  const size_t sessions = (std::min)(s.sessions.size(), (std::min)(kCoopMaxSessions, size_t{63}));
  const int areaBits = (std::min)(static_cast<int>(s.sessionAreaBits), 63);
  w.Write(static_cast<uint32_t>(sessions), 6);
  w.Write(static_cast<uint32_t>(areaBits), 6);
  w.WriteBool(s.sessionWeapons);
  auto clampTo = [](uint64_t v, int bits) { return static_cast<uint32_t>((std::min)(v, (uint64_t{1} << bits) - 1)); };
  for (size_t k = 0; k < sessions; ++k) {
    const CoopSessionSummary& x = s.sessions[k];
    w.Write(clampTo(x.startMin, 28), 28);
    w.Write(clampTo(x.minutes, 12), 12);
    w.Write(clampTo(x.deaths, 12), 12);
    w.Write(clampTo(x.gained, 24), 24);
    w.Write(clampTo(x.levelStart, 10), 10);
    w.Write(clampTo(x.levelEnd, 10), 10);
    for (int b = 0; b < areaBits; ++b) w.WriteBool((x.areaBits >> b) & 1u);
    w.Write(clampTo(x.kills, 6), 6);
    if (s.sessionWeapons) {
      for (int h = 0; h < 2; ++h) {
        w.Write(clampTo(x.weapon[h], 18), 18);
        w.Write(clampTo(x.upgrade[h], 5), 5);
      }
    }
  }
  const size_t records = (std::min)(s.bossRecords.size(), size_t{63});
  w.Write(static_cast<uint32_t>(records), 6);
  for (size_t k = 0; k < records; ++k) {
    const CoopBossRecord& r = s.bossRecords[k];
    w.Write(clampTo(r.boss, 6), 6);
    w.Write(clampTo(r.deaths, 10), 10);
    w.Write(r.ttkMin, 16);
    w.Write(r.session < sessions ? r.session : kCoopNone6, 6);
    w.Write(clampTo(r.killMin, 28), 28);
  }
  w.Write(clampTo(s.lifeSec, 24), 24);
  w.Write(s.lifeSession < sessions ? s.lifeSession : kCoopNone6, 6);
  w.Write(clampTo(s.lifeArea, 6), 6);
  w.Write(clampTo(s.firstDeathAt, 24), 24);
  w.Write(s.firstDeathSession < sessions ? s.firstDeathSession : kCoopNone6, 6);
  w.Write(clampTo(s.firstDeathArea, 6), 6);
  out.insert(out.end(), w.Bytes().begin(), w.Bytes().end());
  return out;
}

struct CoopPacket {
  bool ok = false;
  std::string error;  // why it was refused, for the player
  uint8_t version = 0;
  CoopMessage type = CoopMessage::kHello;
  uint32_t sequence = 0;
  SharedGameState state;  // FullSnapshot only
};

// Strict: anything that isn't exactly a protocol-1 FullSnapshot is refused
// with a reason, never half-applied.
CoopPacket DecodeCoopPacket(const uint8_t* data, size_t size) {
  CoopPacket p;
  if (size < kCoopHeaderBytes) {
    p.error = size == 0 ? "empty" : "too short to be a WASD packet";
    return p;
  }
  if (size > kCoopMaxPacketBytes) {
    p.error = "too long (" + std::to_string(size) + " bytes)";
    return p;
  }
  p.version = data[0];
  if (p.version != kCoopProtocolVersion) {
    p.error = "made by a different WASD version (protocol " + std::to_string(p.version) + "; this one reads " +
              std::to_string(kCoopProtocolVersion) + "): both of you need the same version";
    return p;
  }
  if (data[1] < 1 || data[1] > 6) {
    p.error = "unknown message type " + std::to_string(data[1]);
    return p;
  }
  p.type = static_cast<CoopMessage>(data[1]);
  p.sequence = static_cast<uint32_t>(data[2]) | static_cast<uint32_t>(data[3]) << 8 |
               static_cast<uint32_t>(data[4]) << 16 | static_cast<uint32_t>(data[5]) << 24;
  if (p.type != CoopMessage::kFullSnapshot) {
    p.error = "not a snapshot (message type " + std::to_string(data[1]) + ")";
    return p;
  }
  CoopBitReader r(data + kCoopHeaderBytes, size - kCoopHeaderBytes);
  auto f32 = [&]() {
    uint32_t bits = r.Read(32);
    float f = 0;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
  };
  SharedGameState& s = p.state;
  s.capturedUtc = r.Read(32);
  uint32_t nameLen = r.Read(6);
  if (nameLen > kCoopMaxNameBytes) {
    p.error = "bad character name length " + std::to_string(nameLen);
    return p;
  }
  for (uint32_t k = 0; k < nameLen && r.Ok(); ++k) {
    char c = static_cast<char>(r.Read(8));
    if (static_cast<unsigned char>(c) < 0x20) {
      p.error = "control character in the character name";
      return p;
    }
    s.characterName += c;
  }
  for (uint16_t* v : {&s.level, &s.hp, &s.maxHp, &s.fp, &s.maxFp, &s.stamina, &s.maxStamina})
    *v = static_cast<uint16_t>(r.Read(16));
  for (uint8_t& a : s.attrs) a = static_cast<uint8_t>(r.Read(8));
  s.souls = r.Read(32);
  s.deaths = r.Read(32);
  s.playSeconds = r.Read(32);
  s.clearCount = static_cast<uint8_t>(r.Read(8));
  s.mapId = r.Read(32);
  s.playRegionId = static_cast<int32_t>(r.Read(32));
  s.lastBonfireId = static_cast<int32_t>(r.Read(32));
  s.havePose = r.ReadBool();
  if (s.havePose) {
    s.x = f32();
    s.y = f32();
    s.z = f32();
    s.facing = f32();
    if (!std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(s.z) || !std::isfinite(s.facing)) {
      p.error = "position isn't a finite number";
      return p;
    }
  }
  for (CoopFlagSet* set : {&s.bossesDefeated, &s.keyItemsObtained, &s.keyItemsHeld, &s.pickupsFound}) {
    set->fingerprint = r.Read(32);
    uint32_t n = r.Read(12);
    for (uint32_t k = 0; k < n && r.Ok(); ++k) set->bits.push_back(r.ReadBool());
  }
  s.areaListFingerprint = r.Read(32);
  uint32_t areaCount = r.Read(6);
  for (uint32_t k = 0; k < areaCount && r.Ok(); ++k) {
    CoopAreaEntry a;
    a.index = static_cast<uint8_t>(r.Read(6));
    a.seconds = r.Read(24);
    a.deaths = static_cast<uint16_t>(r.Read(16));
    for (const auto& seen : s.areas)
      if (seen.index == a.index) {
        p.error = "an area listed twice";
        return p;
      }
    s.areas.push_back(a);
  }
  uint32_t levelCount = r.Read(6);
  if (levelCount > kCoopMaxLevelPoints) {
    p.error = "too many level points (" + std::to_string(levelCount) + ")";
    return p;
  }
  for (uint32_t k = 0; k < levelCount && r.Ok(); ++k) {
    CoopLevelPoint pt;
    pt.minutes = r.Read(20);
    pt.level = static_cast<uint16_t>(r.Read(10));
    s.levels.push_back(pt);
  }
  for (uint32_t& v : s.totals) v = r.Read(32);
  const uint32_t sessionCount = r.Read(6);
  s.sessionAreaBits = static_cast<uint8_t>(r.Read(6));
  s.sessionWeapons = r.ReadBool();
  if (sessionCount > kCoopMaxSessions) {
    p.error = "too many sessions (" + std::to_string(sessionCount) + ")";
    return p;
  }
  for (uint32_t k = 0; k < sessionCount && r.Ok(); ++k) {
    CoopSessionSummary x;
    x.startMin = r.Read(28);
    x.minutes = static_cast<uint16_t>(r.Read(12));
    x.deaths = static_cast<uint16_t>(r.Read(12));
    x.gained = r.Read(24);
    x.levelStart = static_cast<uint16_t>(r.Read(10));
    x.levelEnd = static_cast<uint16_t>(r.Read(10));
    for (int b = 0; b < s.sessionAreaBits; ++b)
      if (r.ReadBool()) x.areaBits |= uint64_t{1} << b;
    x.kills = static_cast<uint8_t>(r.Read(6));
    if (s.sessionWeapons) {
      for (int h = 0; h < 2; ++h) {
        x.weapon[h] = r.Read(18);
        x.upgrade[h] = static_cast<uint8_t>(r.Read(5));
      }
    }
    s.sessions.push_back(x);
  }
  auto sessionOk = [&](uint8_t idx) { return idx == kCoopNone6 || idx < s.sessions.size(); };
  const uint32_t recordCount = r.Read(6);
  for (uint32_t k = 0; k < recordCount && r.Ok(); ++k) {
    CoopBossRecord rec;
    rec.boss = static_cast<uint8_t>(r.Read(6));
    rec.deaths = static_cast<uint16_t>(r.Read(10));
    rec.ttkMin = static_cast<uint16_t>(r.Read(16));
    rec.session = static_cast<uint8_t>(r.Read(6));
    rec.killMin = r.Read(28);
    if (r.Ok() && !sessionOk(rec.session)) {
      p.error = "a boss record names a session that isn't there";
      return p;
    }
    s.bossRecords.push_back(rec);
  }
  s.lifeSec = r.Read(24);
  s.lifeSession = static_cast<uint8_t>(r.Read(6));
  s.lifeArea = static_cast<uint8_t>(r.Read(6));
  s.firstDeathAt = r.Read(24);
  s.firstDeathSession = static_cast<uint8_t>(r.Read(6));
  s.firstDeathArea = static_cast<uint8_t>(r.Read(6));
  if (r.Ok() && (!sessionOk(s.lifeSession) || !sessionOk(s.firstDeathSession))) {
    p.error = "a record names a session that isn't there";
    return p;
  }
  if (!r.Ok()) {
    p.error = "truncated (the snapshot ends early)";
    return p;
  }
  if (!r.AtCleanEnd()) {
    p.error = "unexpected data after the snapshot";
    return p;
  }
  p.ok = true;
  return p;
}

uint32_t CoopCrc32(const uint8_t* data, size_t size) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    c ^= data[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

// Standard base64 (RFC 4648 §4), no padding. Older codes used base64url
// ('-' '_'), but Discord reads '_' as markdown and drops it.
constexpr const char* kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const std::vector<uint8_t>& in) {
  std::string out;
  uint32_t acc = 0;
  int bits = 0;
  for (uint8_t b : in) {
    acc = (acc << 8) | b;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out += kBase64Alphabet[(acc >> bits) & 63u];
    }
  }
  if (bits > 0) out += kBase64Alphabet[(acc << (6 - bits)) & 63u];
  return out;  // no '=' padding
}

// Reads both alphabets, so older base64url codes still load.
bool Base64Decode(const std::string& in, std::vector<uint8_t>* out) {
  out->clear();
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '-') c = '+';
    if (c == '_') c = '/';
    const char* hit = std::strchr(kBase64Alphabet, c);
    if (c == '\0' || !hit) return false;
    acc = (acc << 6) | static_cast<uint32_t>(hit - kBase64Alphabet);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<uint8_t>(acc >> bits));
    }
  }
  return bits < 6 && (acc & ((1u << bits) - 1u)) == 0;  // leftover bits are padding zeros
}

std::string CoopToText(const std::vector<uint8_t>& packet) {
  std::vector<uint8_t> body = packet;
  uint32_t crc = CoopCrc32(packet.data(), packet.size());
  for (int k = 0; k < 4; ++k) body.push_back(static_cast<uint8_t>(crc >> (8 * k)));
  return kCoopTextPrefix + Base64Encode(body);
}

constexpr const char* kCoopDamaged =
    "damaged: not all of it was copied, or it was changed (send it in a code block or as a .txt file)";

// The packet inside a pasted code. Whitespace and backticks anywhere are
// ignored (chat apps wrap long lines; a code block adds ```); anything else
// wrong is refused with a reason.
bool CoopFromText(const std::string& text, std::vector<uint8_t>* packet, std::string* error) {
  std::string t;
  for (char c : text)
    if (!std::isspace(static_cast<unsigned char>(c)) && c != '`') t += c;
  const std::string prefix = kCoopTextPrefix;
  if (t.compare(0, prefix.size(), prefix) != 0) {
    *error = t.empty() ? "nothing pasted" : "not a WASD code (it starts with " + prefix + ")";
    return false;
  }
  std::vector<uint8_t> body;
  if (!Base64Decode(t.substr(prefix.size()), &body) || body.size() < 4) {
    *error = kCoopDamaged;
    return false;
  }
  size_t n = body.size() - 4;
  uint32_t crc = static_cast<uint32_t>(body[n]) | static_cast<uint32_t>(body[n + 1]) << 8 |
                 static_cast<uint32_t>(body[n + 2]) << 16 | static_cast<uint32_t>(body[n + 3]) << 24;
  if (CoopCrc32(body.data(), n) != crc) {
    *error = kCoopDamaged;
    return false;
  }
  body.resize(n);
  *packet = body;
  return true;
}

// What a decoded packet holds, as `wasd-cli.exe coop <code>` prints it.
std::string DescribeCoopPacket(const CoopPacket& p) {
  const SharedGameState& s = p.state;
  char buf[256];
  std::string out;
  std::snprintf(buf, sizeof(buf), "protocol %u, full snapshot, sequence %u\n", p.version, p.sequence);
  out += buf;
  time_t t = static_cast<time_t>(s.capturedUtc);
  std::tm tm{};
  gmtime_s(&tm, &t);
  std::strftime(buf, sizeof(buf), "captured   %Y-%m-%d %H:%M:%S UTC\n", &tm);
  out += buf;
  out += "character  " + s.characterName + "\n";
  std::snprintf(buf, sizeof(buf), "level      %u\nHP         %u / %u\nFP         %u / %u\nstamina    %u / %u\n",
                s.level, s.hp, s.maxHp, s.fp, s.maxFp, s.stamina, s.maxStamina);
  out += buf;
  std::snprintf(buf, sizeof(buf), "attributes %u %u %u %u %u %u %u %u %u  (Vig Att End Vit Str Dex Int Fth Lck)\n",
                s.attrs[0], s.attrs[1], s.attrs[2], s.attrs[3], s.attrs[4], s.attrs[5], s.attrs[6], s.attrs[7],
                s.attrs[8]);
  out += buf;
  std::snprintf(buf, sizeof(buf), "souls      %u\ndeaths     %u\nplay time  %uh %02um\njourney    NG%s\n", s.souls,
                s.deaths, s.playSeconds / 3600, s.playSeconds / 60 % 60,
                s.clearCount ? ("+" + std::to_string(s.clearCount)).c_str() : "");
  out += buf;
  std::snprintf(buf, sizeof(buf), "map        m%02u_%02u_%02u_%02u\nregion     %d\nbonfire    %d\n",
                (s.mapId >> 24) & 0xFF, (s.mapId >> 16) & 0xFF, (s.mapId >> 8) & 0xFF, s.mapId & 0xFF,
                s.playRegionId, s.lastBonfireId);
  out += buf;
  if (s.havePose)
    std::snprintf(buf, sizeof(buf), "position   %.2f, %.2f, %.2f  facing %.3f rad\n", s.x, s.y, s.z, s.facing);
  else
    std::snprintf(buf, sizeof(buf), "position   unknown\n");
  out += buf;
  const std::pair<const char*, const CoopFlagSet*> sets[] = {{"bosses defeated", &s.bossesDefeated},
                                                             {"key items obtained", &s.keyItemsObtained},
                                                             {"key items held", &s.keyItemsHeld},
                                                             {"pickups found", &s.pickupsFound}};
  for (const auto& [label, set] : sets) {
    std::snprintf(buf, sizeof(buf), "%-19s%d / %zu  (list %08x)\n", label, set->CountOn(), set->bits.size(),
                  set->fingerprint);
    out += buf;
  }
  uint64_t areaSeconds = 0, areaDeaths = 0;
  for (const auto& a : s.areas) areaSeconds += a.seconds, areaDeaths += a.deaths;
  std::snprintf(buf, sizeof(buf), "recorded   %zu area(s), %lluh %02llum, %llu death(s)  (list %08x)\n",
                s.areas.size(), static_cast<unsigned long long>(areaSeconds / 3600),
                static_cast<unsigned long long>(areaSeconds / 60 % 60), static_cast<unsigned long long>(areaDeaths),
                s.areaListFingerprint);
  out += buf;
  {
    uint32_t firstMin = 0, lastMin = 0;
    for (const auto& x : s.sessions) {
      if (!x.startMin) continue;
      if (!firstMin || x.startMin < firstMin) firstMin = x.startMin;
      lastMin = (std::max)(lastMin, x.startMin);
    }
    std::snprintf(buf, sizeof(buf),
                  "totals     %uh %02um recorded, %u death(s), %u souls lost, %u recovered, %u gained, longest %uh %02um\n",
                  s.totals[0] / 3600, s.totals[0] / 60 % 60, s.totals[1], s.totals[2], s.totals[3], s.totals[4],
                  s.totals[5] / 3600, s.totals[5] / 60 % 60);
    out += buf;
    std::snprintf(buf, sizeof(buf), "sessions   %zu%s", s.sessions.size(), s.sessionWeapons ? ", with weapons" : "");
    out += buf;
    if (firstMin) {
      time_t a = static_cast<time_t>(firstMin) * 60, b = static_cast<time_t>(lastMin) * 60;
      std::tm ta{}, tb{};
      gmtime_s(&ta, &a);
      gmtime_s(&tb, &b);
      char da[16], db[16];
      std::strftime(da, sizeof(da), "%Y-%m-%d", &ta);
      std::strftime(db, sizeof(db), "%Y-%m-%d", &tb);
      out += std::string(", ") + da + " to " + db + " (UTC)";
    }
    out += "\n";
    std::snprintf(buf, sizeof(buf), "bosses     %zu kill(s) recorded\n", s.bossRecords.size());
    out += buf;
  }
  std::snprintf(buf, sizeof(buf), "levels     %zu point(s)", s.levels.size());
  out += buf;
  if (!s.levels.empty()) {
    std::snprintf(buf, sizeof(buf), ", level %u at %uh %02um to %u at %uh %02um", s.levels.front().level,
                  s.levels.front().minutes / 60, s.levels.front().minutes % 60, s.levels.back().level,
                  s.levels.back().minutes / 60, s.levels.back().minutes % 60);
    out += buf;
  }
  return out + "\n";
}

// Whether a code loaded as the partner's is an earlier snapshot of your own
// character (your own code pasted back by mistake): the same name and
// journey, no more play time than now, and no pickup you haven't found.
bool CoopIsOwnCode(const SharedGameState& mine, const SharedGameState& code) {
  if (mine.characterName.empty() || code.characterName != mine.characterName) return false;
  if (code.clearCount != mine.clearCount || code.playSeconds > mine.playSeconds) return false;
  const CoopFlagSet &a = code.pickupsFound, &b = mine.pickupsFound;
  if (a.fingerprint != b.fingerprint || a.bits.size() != b.bits.size()) return true;  // can't compare: name and time
  for (size_t k = 0; k < a.bits.size(); ++k)
    if (a.bits[k] && !b.bits[k]) return false;
  return true;
}

CoopPacket CoopDecodeText(const std::string& text) {
  std::vector<uint8_t> packet;
  std::string error;
  if (!CoopFromText(text, &packet, &error)) {
    CoopPacket p;
    p.error = error;
    return p;
  }
  return DecodeCoopPacket(packet.data(), packet.size());
}

// One flag set compared with ours. Comparable only when both follow the
// same list; otherwise only the partner's own count means anything.
struct CoopFlagDiff {
  bool comparable = false;
  int partnerOn = 0;
  std::vector<size_t> onlyPartner, onlyMine;  // list indexes
};

CoopFlagDiff CompareCoopFlags(const CoopFlagSet& mine, const CoopFlagSet& partner) {
  CoopFlagDiff d;
  d.partnerOn = partner.CountOn();
  d.comparable = mine.fingerprint == partner.fingerprint && mine.bits.size() == partner.bits.size();
  if (!d.comparable) return d;
  for (size_t k = 0; k < mine.bits.size(); ++k) {
    if (partner.bits[k] && !mine.bits[k]) d.onlyPartner.push_back(k);
    if (mine.bits[k] && !partner.bits[k]) d.onlyMine.push_back(k);
  }
  return d;
}

// The receiver's own lists, in flag-set order, with what the comparison
// needs of each entry: names, each boss's area, each key item's kind, each
// world pickup's area ("" when its play region has none).
struct CoopLists {
  std::vector<std::string> bossNames, bossAreas, keyItemNames, keyItemKinds, pickupAreas;
  std::vector<uint32_t> bossFlags;
};

// Map section and world (Untended Graves has its own coordinates), as the
// live page's mapKey: map number * 2, + 1 in Untended Graves.
int CoopMapKey(uint32_t mapId, int32_t playRegionId) {
  return static_cast<int>(mapId >> 24) * 2 + (playRegionId >= 400000 && playRegionId < 400100 ? 1 : 0);
}

// A stable id for a name (FNV-1a over its bytes), so that a list of names
// gets a fingerprint the way a list of flag ids does.
uint32_t CoopNameId(const std::string& name) {
  uint32_t h = 2166136261u;
  for (unsigned char c : name) h = (h ^ c) * 16777619u;
  return h;
}
uint32_t CoopAreaListFingerprint(const std::vector<std::string>& areas) {
  std::vector<uint32_t> ids;
  for (const auto& a : areas) ids.push_back(CoopNameId(a));
  return CoopFingerprint(ids);
}

// One field of a session log's flat JSON line (as SessionLog writes them):
// a string's text (\" and \\ undone) or a number's digits; "" if absent.
std::string CoopJsonField(const std::string& line, const char* key) {
  const std::string k = std::string("\"") + key + "\":";
  size_t p = line.find(k);
  if (p == std::string::npos) return "";
  p += k.size();
  if (p < line.size() && line[p] == '"') {
    std::string out;
    for (++p; p < line.size() && line[p] != '"'; ++p) {
      if (line[p] == '\\' && p + 1 < line.size()) ++p;
      out += line[p];
    }
    return out;
  }
  size_t e = line.find_first_of(",}", p);
  return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

// One session log, read for the code: SummariseCoopSessions's input.
struct CoopLogSession {
  int64_t startMin = 0;  // Unix minutes, from startedAt (local time on this PC); 0 = unknown
  double dur = 0;
  uint64_t lost = 0, recovered = 0, gained = 0;
  int levelStart = 0, levelEnd = 0;
  std::vector<std::pair<double, std::string>> enters;
  struct Death {
    double t;
    std::string area;
    uint32_t boss;
  };
  std::vector<Death> deaths;
  std::vector<std::pair<double, uint32_t>> kills;  // t, boss flag
  struct Gear {
    double t;
    int hand;
    int32_t id;
    int upgrade;
  };
  std::vector<Gear> gear;
  std::vector<CoopLevelPoint> points;
};

// "2026-10-06T15:51:10", local time, to Unix minutes; 0 if it doesn't read.
int64_t CoopLocalStampToUnixMinutes(const std::string& stamp) {
  std::tm tm{};
  if (sscanf_s(stamp.c_str(), "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
                  &tm.tm_sec) != 6)
    return 0;
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  tm.tm_isdst = -1;
  time_t t = std::mktime(&tm);
  return t > 0 ? static_cast<int64_t>(t) / 60 : 0;
}

CoopLogSession CoopParseLog(const std::string& text) {
  CoopLogSession x;
  double lastT = 0, endSeconds = -1;
  bool haveStartLevel = false;
  for (size_t pos = 0; pos < text.size();) {
    size_t eol = text.find('\n', pos);
    std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
    pos = eol == std::string::npos ? text.size() : eol + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.size() < 2 || line.front() != '{' || line.back() != '}') continue;  // a line cut off mid-write
    const std::string type = CoopJsonField(line, "type"), t = CoopJsonField(line, "t");
    if (!t.empty()) lastT = std::strtod(t.c_str(), nullptr);
    auto num = [&](const char* key) { return std::strtoll(CoopJsonField(line, key).c_str(), nullptr, 10); };
    if (type == "start") {
      x.startMin = CoopLocalStampToUnixMinutes(CoopJsonField(line, "startedAt"));
      if (num("level") > 0) x.levelStart = x.levelEnd = static_cast<int>(num("level")), haveStartLevel = true;
    } else if (type == "enter") {
      x.enters.push_back({lastT, CoopJsonField(line, "area")});
    } else if (type == "death") {
      x.deaths.push_back({lastT, CoopJsonField(line, "area"), static_cast<uint32_t>(num("boss"))});
      x.lost += static_cast<uint64_t>((std::max)(0LL, num("soulsLost")));
    } else if (type == "bloodstain") {
      x.recovered += static_cast<uint64_t>((std::max)(0LL, num("souls")));
    } else if (type == "boss") {
      x.kills.push_back({lastT, static_cast<uint32_t>(num("flag"))});
    } else if (type == "gear") {
      x.gear.push_back({lastT, CoopJsonField(line, "slot") == "L" ? 1 : 0, static_cast<int32_t>(num("id")),
                        static_cast<int>(num("upgrade"))});
    } else if (type == "sample" || type == "end") {
      if (type == "end") {
        endSeconds = std::strtod(CoopJsonField(line, "seconds").c_str(), nullptr);
        x.gained = static_cast<uint64_t>((std::max)(0LL, num("gained")));
      }
      long long ms = num("playTimeMs"), level = num("level");
      if (level > 0) {
        if (!haveStartLevel) x.levelStart = static_cast<int>(level), haveStartLevel = true;
        x.levelEnd = static_cast<int>(level);
      }
      if (ms > 0 && level > 0)
        x.points.push_back({static_cast<uint32_t>(ms / 60000), static_cast<uint16_t>((std::min)(level, 1023LL))});
    }
  }
  x.dur = endSeconds >= 0 ? endSeconds : lastT;
  return x;
}

// The recorded part of a player's shared state, from their saved session
// logs (each a whole .jsonl file), summed the way the results page sums
// them: time per area from one `enter` to the next (the last one runs to
// the session's end), deaths per area from `death` events, areas in the
// order first visited. The level at each `sample` and `end` against the
// game's play time, kept where it changes (and the last one), then thinned
// evenly to kCoopMaxLevelPoints. Then the totals, the sessions (newest
// first, at most kCoopMaxSessions), the bosses killed (`boss` events, with
// the deaths near each before the kill: `death.boss`) and two records.
// Areas not in `areaList` and bosses not in `bossFlags` are left out.
void SummariseCoopSessions(const std::vector<std::string>& logs, const std::vector<std::string>& areaList,
                           const std::vector<uint32_t>& bossFlags, SharedGameState* s) {
  std::vector<CoopLogSession> all;
  for (const std::string& text : logs) all.push_back(CoopParseLog(text));
  std::stable_sort(all.begin(), all.end(),
                   [](const CoopLogSession& a, const CoopLogSession& b) { return a.startMin < b.startMin; });
  auto indexOf = [&](const std::string& area) {
    for (size_t k = 0; k < areaList.size(); ++k)
      if (areaList[k] == area) return static_cast<int>(k);
    return -1;
  };
  std::vector<double> seconds(areaList.size(), 0.0);
  std::vector<uint32_t> deaths(areaList.size(), 0);
  std::vector<int> order;  // area indexes, first visited first
  std::vector<CoopLevelPoint> points;
  std::vector<double> offset;  // each session's start in recorded time
  double recorded = 0;
  for (const auto& x : all) {
    offset.push_back(recorded);
    recorded += x.dur;
    for (size_t k = 0; k < x.enters.size(); ++k) {
      double until = k + 1 < x.enters.size() ? x.enters[k + 1].first : x.dur;
      int a = indexOf(x.enters[k].second);
      if (a < 0) continue;
      seconds[a] += (std::max)(0.0, until - x.enters[k].first);
      if (std::find(order.begin(), order.end(), a) == order.end()) order.push_back(a);
    }
    for (const auto& d : x.deaths) {
      int a = indexOf(d.area);
      if (a < 0) continue;
      ++deaths[a];
      if (std::find(order.begin(), order.end(), a) == order.end()) order.push_back(a);
    }
    points.insert(points.end(), x.points.begin(), x.points.end());
  }
  s->areaListFingerprint = CoopAreaListFingerprint(areaList);
  s->areas.clear();
  for (int k : order) {
    if (k >= static_cast<int>(kCoopMaxAreas)) continue;
    uint32_t sec = static_cast<uint32_t>((std::min)(seconds[k] + 0.5, 16777215.0));
    if (sec > 0 || deaths[k] > 0)
      s->areas.push_back({static_cast<uint8_t>(k), sec, static_cast<uint16_t>((std::min)(deaths[k], 65535u))});
  }
  std::stable_sort(points.begin(), points.end(),
                   [](const CoopLevelPoint& a, const CoopLevelPoint& b) { return a.minutes < b.minutes; });
  std::vector<CoopLevelPoint> kept;
  for (size_t k = 0; k < points.size(); ++k)
    if (kept.empty() || points[k].level != kept.back().level || k + 1 == points.size()) kept.push_back(points[k]);
  s->levels.clear();
  if (kept.size() <= kCoopMaxLevelPoints) {
    s->levels = kept;
  } else {
    for (size_t k = 0; k < kCoopMaxLevelPoints; ++k)
      s->levels.push_back(kept[k * (kept.size() - 1) / (kCoopMaxLevelPoints - 1)]);
  }

  // Totals, over every session.
  auto u32 = [](double v) { return static_cast<uint32_t>((std::min)((std::max)(v, 0.0), 4294967295.0)); };
  uint64_t lost = 0, recovered = 0, gained = 0, deathCount = 0;
  double longest = 0;
  for (const auto& x : all) {
    lost += x.lost, recovered += x.recovered, gained += x.gained, deathCount += x.deaths.size();
    longest = (std::max)(longest, x.dur);
  }
  s->totals[0] = u32(recorded + 0.5), s->totals[1] = u32(static_cast<double>(deathCount));
  s->totals[2] = u32(static_cast<double>(lost)), s->totals[3] = u32(static_cast<double>(recovered));
  s->totals[4] = u32(static_cast<double>(gained)), s->totals[5] = u32(longest + 0.5);

  // Sessions, newest first. idx(i) is session i's (oldest first) place in
  // that list, or kCoopNone6 when it's older than the ones sent.
  const size_t n = all.size(), sent = (std::min)(n, kCoopMaxSessions);
  auto idx = [&](size_t i) { return n - 1 - i < sent ? static_cast<uint8_t>(n - 1 - i) : kCoopNone6; };
  s->sessionAreaBits = static_cast<uint8_t>((std::min)(areaList.size(), size_t{63}));
  s->sessionWeapons = false;
  s->sessions.clear();
  for (size_t back = 0; back < sent; ++back) {
    const auto& x = all[n - 1 - back];
    CoopSessionSummary y;
    y.startMin = static_cast<uint32_t>((std::max)(x.startMin, int64_t{0}));
    y.minutes = static_cast<uint16_t>((std::min)(x.dur / 60 + 0.5, 4095.0));
    y.deaths = static_cast<uint16_t>((std::min)(x.deaths.size(), size_t{4095}));
    y.gained = static_cast<uint32_t>((std::min)(x.gained, uint64_t{0xFFFFFF}));
    y.levelStart = static_cast<uint16_t>((std::min)(x.levelStart, 1023));
    y.levelEnd = static_cast<uint16_t>((std::min)(x.levelEnd, 1023));
    for (const auto& e : x.enters) {
      int a = indexOf(e.second);
      if (a >= 0 && a < 63) y.areaBits |= uint64_t{1} << a;
    }
    y.kills = static_cast<uint8_t>((std::min)(x.kills.size(), size_t{63}));
    // The weapon held longest in each hand.
    for (int h = 0; h < 2; ++h) {
      std::vector<CoopLogSession::Gear> ev;
      for (const auto& g : x.gear)
        if (g.hand == h) ev.push_back(g);
      std::unordered_map<int32_t, double> held;
      for (size_t k = 0; k < ev.size(); ++k)
        held[ev[k].id] += (std::max)(0.0, (k + 1 < ev.size() ? ev[k + 1].t : x.dur) - ev[k].t);
      int32_t best = 0;
      double bestSec = -1;
      for (const auto& [id, sec] : held)
        if (sec > bestSec || (sec == bestSec && id < best)) best = id, bestSec = sec;
      if (best > 0) {
        y.weapon[h] = static_cast<uint32_t>(best / 100);
        y.upgrade[h] = static_cast<uint8_t>((std::min)(best % 100, 31));
        s->sessionWeapons = true;
      }
    }
    s->sessions.push_back(y);
  }

  // Bosses killed: attempts (deaths near it) before the kill, on recorded time.
  s->bossRecords.clear();
  for (size_t i = 0; i < n; ++i) {
    for (const auto& [kt, flag] : all[i].kills) {
      auto it = std::find(bossFlags.begin(), bossFlags.end(), flag);
      if (it == bossFlags.end() || it - bossFlags.begin() >= 63) continue;
      const double killAt = offset[i] + kt;
      uint16_t tries = 0;
      double first = -1;
      for (size_t j = 0; j < n; ++j)
        for (const auto& d : all[j].deaths)
          if (d.boss == flag && offset[j] + d.t <= killAt) {
            ++tries;
            if (first < 0 || offset[j] + d.t < first) first = offset[j] + d.t;
          }
      CoopBossRecord r;
      r.boss = static_cast<uint8_t>(it - bossFlags.begin());
      r.deaths = (std::min)(tries, uint16_t{1023});
      r.ttkMin = first < 0 ? 0xFFFF : static_cast<uint16_t>((std::min)((killAt - first) / 60 + 0.5, 65534.0));
      r.session = idx(i);
      r.killMin = all[i].startMin ? static_cast<uint32_t>(all[i].startMin + static_cast<int64_t>(kt / 60)) : 0;
      s->bossRecords.push_back(r);
    }
  }

  // Records: the longest life (within a session) and the first death.
  s->lifeSec = 0, s->lifeSession = s->lifeArea = kCoopNone6;
  s->firstDeathAt = 0xFFFFFF, s->firstDeathSession = s->firstDeathArea = kCoopNone6;
  for (size_t i = 0; i < n; ++i) {
    const auto& x = all[i];
    double from = 0;
    std::string area = x.enters.empty() ? "" : x.enters.front().second;
    std::vector<CoopLogSession::Death> ends = x.deaths;
    ends.push_back({x.dur, "", 0});
    for (const auto& d : ends) {
      double len = d.t - from;
      if (len > s->lifeSec) {
        int a = indexOf(d.area.empty() ? area : d.area);
        s->lifeSec = static_cast<uint32_t>((std::min)(len + 0.5, 16777215.0));
        s->lifeSession = idx(i);
        s->lifeArea = a >= 0 && a < 63 ? static_cast<uint8_t>(a) : kCoopNone6;
      }
      from = d.t;
      if (!d.area.empty()) area = d.area;
    }
    if (s->firstDeathAt == 0xFFFFFF && !x.deaths.empty()) {
      int a = indexOf(x.deaths.front().area);
      s->firstDeathAt = static_cast<uint32_t>((std::min)(offset[i] + x.deaths.front().t + 0.5, 16777214.0));
      s->firstDeathSession = idx(i);
      s->firstDeathArea = a >= 0 && a < 63 ? static_cast<uint8_t>(a) : kCoopNone6;
    }
  }
}

// A code that fits one chat message (kCoopMaxCodeChars): if the whole state
// doesn't, the weapons per session go first, then the oldest sessions, one
// at a time. `leftOut` says what was dropped ("" if nothing). Records that
// pointed at a dropped session point nowhere (kCoopNone6).
std::string FitCoopCode(SharedGameState s, uint32_t sequence, std::string* leftOut) {
  std::string code = CoopToText(EncodeCoopSnapshot(s, sequence));
  bool droppedWeapons = false;
  size_t droppedSessions = 0;
  if (code.size() > kCoopMaxCodeChars && s.sessionWeapons) {
    s.sessionWeapons = false;
    droppedWeapons = true;
    code = CoopToText(EncodeCoopSnapshot(s, sequence));
  }
  while (code.size() > kCoopMaxCodeChars && !s.sessions.empty()) {
    s.sessions.pop_back();
    ++droppedSessions;
    code = CoopToText(EncodeCoopSnapshot(s, sequence));
  }
  if (leftOut) {
    leftOut->clear();
    if (droppedWeapons) *leftOut = "the weapons per session";
    if (droppedSessions)
      *leftOut += std::string(droppedWeapons ? " and " : "") + "the " + std::to_string(droppedSessions) + " oldest session" +
                  (droppedSessions == 1 ? "" : "s");
  }
  return code;
}

// One player as the results page compares them.
struct CoopPlayerView {
  bool shown = false;
  std::string name, area, region, bonfire;  // filled in by CoopPlaceView
  bool elsewhere = false;                   // the partner is somewhere you haven't reached (no names)
  std::string beyond;                       // at Category: the area it's a new area beyond (route.tsv)
  int level = 0, ng = 0;
  int hp[2] = {}, fp[2] = {}, stamina[2] = {}, attrs[9] = {};
  int64_t souls = 0, deaths = 0, playSeconds = 0, ageSeconds = 0;
  int bosses[2] = {}, keys[2] = {}, keysHeld = 0, items[2] = {};  // how many on, of the list's size
  struct Area {
    std::string name;
    int64_t seconds = 0;
    int deaths = 0;
  };
  std::vector<Area> areas;                    // recorded time and deaths per area
  std::vector<std::pair<double, int>> levels;  // play time in hours, level
};
// A boss or key item, and which of you has it.
struct CoopRow {
  std::string name, group;  // both "" unless you have it yourself, or at Full
  bool me = false, partner = false, meHeld = false, partnerHeld = false;
};
struct CoopAreaItems {
  std::string area;  // kCoopOtherAreas for the areas you haven't reached (below Full)
  int total = 0, me = 0, partner = 0, both = 0;
};
struct CoopCompare {
  CoopPlayerView me, partner;
  bool sameMap = false;
  float dist = -1, dy = 0;
  std::string clock;
  bool bossesCompared = false, keysCompared = false, itemsCompared = false;
  std::vector<CoopRow> bosses, keys;  // empty until your own lists are read
  std::vector<CoopAreaItems> items;
  std::vector<std::string> notes;     // lists that don't match yours
  std::set<std::string> reached;      // the areas you've reached (CoopReachedAreas)
};
constexpr const char* kCoopOtherAreas = "Other areas";

// The areas you've reached: where you are now, and every area with your
// recorded time, an item you've found or a boss you've defeated.
std::set<std::string> CoopReachedAreas(const SharedGameState& mine, bool haveMine, const CoopLists& lists,
                                       const std::vector<std::string>& areaList, const std::string& myArea) {
  std::set<std::string> reached;
  if (!haveMine) return reached;
  if (!myArea.empty()) reached.insert(myArea);
  for (const auto& a : mine.areas)
    if (a.index < areaList.size()) reached.insert(areaList[a.index]);
  for (size_t k = 0; k < mine.pickupsFound.bits.size() && k < lists.pickupAreas.size(); ++k)
    if (mine.pickupsFound.bits[k] && !lists.pickupAreas[k].empty()) reached.insert(lists.pickupAreas[k]);
  for (size_t k = 0; k < mine.bossesDefeated.bits.size() && k < lists.bossAreas.size(); ++k)
    if (mine.bossesDefeated.bits[k]) reached.insert(lists.bossAreas[k]);
  return reached;
}

// Where a player is, by name, at the spoiler tier the way the live page
// shows places (route hints): yours always; your partner's at Full, or in
// an area you've reached (`reached`). Anywhere else, no names and
// `elsewhere`; at Category also `beyond`, the area it opens from
// (`fromArea`, if you've reached it; else "somewhere you've been"). A
// region with no area (a PvP arena, an unknown id) counts as not reached.
void CoopPlaceView(CoopPlayerView* v, const std::string& area, const std::string& region, const std::string& bonfire,
                   const std::set<std::string>* reached, int tier = kTierOff, const std::string& fromArea = "") {
  if (!v->shown) return;
  v->beyond.clear();
  if (reached && tier < kTierFull && (area.empty() || !reached->count(area))) {
    v->area = v->region = v->bonfire = "";
    v->elsewhere = !area.empty() || !region.empty();
    if (v->elsewhere && tier == kTierCategory)
      v->beyond = !fromArea.empty() && reached->count(fromArea) ? fromArea : "somewhere you've been";
    return;
  }
  v->area = area, v->region = region, v->bonfire = bonfire;
  v->elsewhere = false;
}

// Pure: both players side by side. Your partner's data follows the same
// spoiler rules as the live page would for the same data, at the tier:
// what you have is always named (bosses you've defeated, key items you
// have, areas you've reached: CoopReachedAreas); the rest only at Full.
// Below Full, those are unnamed rows (counted), and areas you haven't
// reached are summed as "Other areas". You need your own lists (read from
// the game) to compare anything. `myArea`: the area you're in now.
CoopCompare BuildCoopCompare(const SharedGameState& mine, bool haveMine, const SharedGameState& partner,
                             bool havePartner, const CoopLists& lists, const std::vector<std::string>& areaList,
                             const std::string& myArea, uint32_t nowUtc, int tier = kTierOff) {
  CoopCompare c;
  const bool full = tier >= kTierFull;
  const bool mineRead = haveMine && mine.bossesDefeated.fingerprint != 0;
  c.reached = CoopReachedAreas(mine, haveMine, lists, areaList, myArea);
  auto areaName = [&](const std::string& a) { return full || c.reached.count(a) ? a : std::string(kCoopOtherAreas); };
  const uint32_t areaFingerprint = CoopAreaListFingerprint(areaList);

  auto view = [&](const SharedGameState& s, bool isMine) {
    CoopPlayerView v;
    v.shown = true;
    v.name = s.characterName;
    v.level = s.level;
    v.ng = s.clearCount;
    v.hp[0] = s.hp, v.hp[1] = s.maxHp;
    v.fp[0] = s.fp, v.fp[1] = s.maxFp;
    v.stamina[0] = s.stamina, v.stamina[1] = s.maxStamina;
    for (int k = 0; k < 9; ++k) v.attrs[k] = s.attrs[k];
    v.souls = s.souls, v.deaths = s.deaths, v.playSeconds = s.playSeconds;
    v.ageSeconds = isMine || nowUtc < s.capturedUtc ? 0 : static_cast<int64_t>(nowUtc - s.capturedUtc);
    v.bosses[0] = s.bossesDefeated.CountOn(), v.bosses[1] = static_cast<int>(s.bossesDefeated.bits.size());
    v.keys[0] = s.keyItemsObtained.CountOn(), v.keys[1] = static_cast<int>(s.keyItemsObtained.bits.size());
    v.keysHeld = s.keyItemsHeld.CountOn();
    v.items[0] = s.pickupsFound.CountOn(), v.items[1] = static_cast<int>(s.pickupsFound.bits.size());
    if (isMine || s.areaListFingerprint == areaFingerprint) {
      for (const auto& a : s.areas) {
        if (a.index >= areaList.size()) continue;
        std::string name = isMine ? areaList[a.index] : areaName(areaList[a.index]);
        auto it = std::find_if(v.areas.begin(), v.areas.end(), [&](const auto& x) { return x.name == name; });
        if (it == v.areas.end()) v.areas.push_back({name, a.seconds, a.deaths});
        else it->seconds += a.seconds, it->deaths += a.deaths;
      }
    }
    for (const auto& p : s.levels) v.levels.push_back({p.minutes / 60.0, p.level});
    return v;
  };
  if (haveMine) c.me = view(mine, true);
  if (!havePartner) return c;
  c.partner = view(partner, false);
  if (!partner.areas.empty() && partner.areaListFingerprint != areaFingerprint)
    c.notes.push_back("Area lists differ: their time and deaths per area aren't shown");

  if (haveMine) {
    c.sameMap = partner.mapId != 0 &&
                CoopMapKey(mine.mapId, mine.playRegionId) == CoopMapKey(partner.mapId, partner.playRegionId);
    if (c.sameMap && mine.havePose && partner.havePose) {
      float dx = partner.x - mine.x, dy = partner.y - mine.y, dz = partner.z - mine.z;
      c.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
      c.dy = dy;
      PlayerPose me;
      me.ok = true;
      me.x = mine.x, me.y = mine.y, me.z = mine.z, me.facingRadians = mine.facing;
      c.clock = ClockDirection(me, dx, dz);
    }
  }
  if (!mineRead) return c;  // nothing to compare against yet

  CoopFlagDiff bosses = CompareCoopFlags(mine.bossesDefeated, partner.bossesDefeated);
  c.bossesCompared = bosses.comparable;
  for (size_t k = 0; k < mine.bossesDefeated.bits.size() && k < lists.bossNames.size(); ++k) {
    CoopRow r;
    r.me = mine.bossesDefeated.bits[k];
    r.partner = bosses.comparable && partner.bossesDefeated.bits[k];
    if (r.me || full) r.name = lists.bossNames[k], r.group = k < lists.bossAreas.size() ? lists.bossAreas[k] : "";
    c.bosses.push_back(r);
  }
  if (!bosses.comparable) c.notes.push_back("Boss lists differ");

  CoopFlagDiff keys = CompareCoopFlags(mine.keyItemsObtained, partner.keyItemsObtained);
  const bool heldComparable = CompareCoopFlags(mine.keyItemsHeld, partner.keyItemsHeld).comparable;
  c.keysCompared = keys.comparable;
  for (size_t k = 0; k < mine.keyItemsObtained.bits.size() && k < lists.keyItemNames.size(); ++k) {
    CoopRow r;
    r.me = mine.keyItemsObtained.bits[k];
    r.meHeld = k < mine.keyItemsHeld.bits.size() && mine.keyItemsHeld.bits[k];
    r.partner = keys.comparable && partner.keyItemsObtained.bits[k];
    r.partnerHeld = heldComparable && partner.keyItemsHeld.bits[k];
    if (r.me || full) r.name = lists.keyItemNames[k], r.group = k < lists.keyItemKinds.size() ? lists.keyItemKinds[k] : "";
    c.keys.push_back(r);
  }
  if (!keys.comparable) c.notes.push_back("Key item lists differ");

  CoopFlagDiff items = CompareCoopFlags(mine.pickupsFound, partner.pickupsFound);
  c.itemsCompared = items.comparable;
  CoopAreaItems other;
  other.area = kCoopOtherAreas;
  for (size_t k = 0; k < mine.pickupsFound.bits.size() && k < lists.pickupAreas.size(); ++k) {
    if (lists.pickupAreas[k].empty()) continue;
    std::string name = areaName(lists.pickupAreas[k]);
    CoopAreaItems* row = &other;
    if (name != kCoopOtherAreas) {
      auto it = std::find_if(c.items.begin(), c.items.end(), [&](const auto& x) { return x.area == name; });
      if (it == c.items.end()) {
        c.items.push_back({name});
        row = &c.items.back();
      } else {
        row = &*it;
      }
    }
    bool me = mine.pickupsFound.bits[k], them = items.comparable && partner.pickupsFound.bits[k];
    ++row->total;
    row->me += me;
    row->partner += them;
    row->both += me && them;
  }
  if (other.total) c.items.push_back(other);
  if (!items.comparable) c.notes.push_back("Item lists differ (a different journey, game version or map data)");
  if (!c.notes.empty()) c.notes.push_back("both of you need the same WASD version, game version and NG cycle");
  return c;
}

// ---- the pair report's data (results page: Report for Together / Partner)

// Two sessions are played together when their times overlap: each such
// pair, with the overlap. Sessions are newest first, as in a code;
// sessions with no start time are left out. Touching isn't overlapping.
struct CoopTogether {
  size_t me = 0, partner = 0;  // indexes into each player's sessions
  uint32_t startMin = 0;       // when the overlap starts (Unix minutes)
  uint32_t minutes = 0;
};
std::vector<CoopTogether> CoopSessionsTogether(const std::vector<CoopSessionSummary>& mine,
                                               const std::vector<CoopSessionSummary>& partner) {
  std::vector<CoopTogether> out;
  for (size_t i = 0; i < mine.size(); ++i) {
    if (!mine[i].startMin) continue;
    const uint32_t a0 = mine[i].startMin, a1 = a0 + mine[i].minutes;
    for (size_t j = 0; j < partner.size(); ++j) {
      if (!partner[j].startMin) continue;
      const uint32_t b0 = partner[j].startMin, b1 = b0 + partner[j].minutes;
      const uint32_t from = (std::max)(a0, b0), to = (std::min)(a1, b1);
      if (to > from) out.push_back({i, j, from, to - from});
    }
  }
  std::sort(out.begin(), out.end(), [](const CoopTogether& x, const CoopTogether& y) { return x.startMin > y.startMin; });
  return out;
}

// One player's recorded play, as the pair report shows it, names already
// at the spoiler tier the way the live page would show them: the partner's
// areas, bosses and weapons are named at Full, and below it only where
// you've reached them yourself (an area in `reached`, a boss you've
// defeated, a weapon you've held in a recorded session); the rest arrive
// unnamed ("" or kCoopOtherAreas).
struct CoopPairPlayer {
  bool shown = false;
  uint32_t totals[6] = {};
  struct Weapon {
    std::string name;  // "" = not named (a weapon you haven't found)
    int upgrade = 0;
    bool any = false;  // a weapon was recorded at all
  };
  struct Session {
    uint32_t startMin = 0, minutes = 0, deaths = 0, gained = 0, levelStart = 0, levelEnd = 0, kills = 0;
    std::vector<std::string> areas;  // kCoopOtherAreas once for any you haven't reached
    Weapon weapons[2];
  };
  std::vector<Session> sessions;  // newest first
  struct Boss {
    std::string name, area;  // "" when not named
    uint32_t deaths = 0;
    int ttkMin = -1;   // -1: no attempts before the kill
    int session = -1;  // index into sessions, -1: not among them
    uint32_t killMin = 0;
  };
  std::vector<Boss> bosses;
  struct Mark {
    bool have = false;
    uint32_t seconds = 0;
    int session = -1;
    std::string area;
  };
  Mark life, firstDeath;  // firstDeath.seconds: into the recorded time
};
struct CoopPair {
  CoopPairPlayer me, partner;
  std::vector<CoopTogether> together;
  uint32_t togetherMinutes = 0;
};

// Pure. `weaponName(id)` names a weapon param id with its upgrade digits
// zeroed ("" if unknown).
CoopPair BuildCoopPair(const SharedGameState& mine, bool haveMine, const SharedGameState& partner, bool havePartner,
                       const CoopLists& lists, const std::vector<std::string>& areaList,
                       const std::set<std::string>& reached,
                       const std::function<std::string(int32_t)>& weaponName, int tier = kTierOff) {
  CoopPair c;
  const bool full = tier >= kTierFull;
  const uint32_t areaFingerprint = CoopAreaListFingerprint(areaList);
  // Weapons you've held in a recorded session, without their infusion and
  // upgrade (a Heavy Long Sword +3 is a Long Sword you've found).
  std::set<uint32_t> myWeapons;
  if (haveMine && mine.sessionWeapons)
    for (const auto& x : mine.sessions)
      for (uint32_t w : x.weapon)
        if (w) myWeapons.insert(w - w % 100);
  auto view = [&](const SharedGameState& s, bool isMine) {
    CoopPairPlayer v;
    v.shown = true;
    std::copy(s.totals, s.totals + 6, v.totals);
    const bool areasOk = isMine || s.areaListFingerprint == areaFingerprint;
    auto areaName = [&](int index) -> std::string {
      if (index < 0 || index >= 63 || !areasOk || index >= static_cast<int>(areaList.size())) return "";
      const std::string& a = areaList[index];
      return isMine || full || reached.count(a) ? a : std::string(kCoopOtherAreas);
    };
    for (const auto& x : s.sessions) {
      CoopPairPlayer::Session y;
      y.startMin = x.startMin, y.minutes = x.minutes, y.deaths = x.deaths, y.gained = x.gained;
      y.levelStart = x.levelStart, y.levelEnd = x.levelEnd, y.kills = x.kills;
      bool other = false;
      for (int b = 0; b < s.sessionAreaBits && b < 63; ++b) {
        if (!((x.areaBits >> b) & 1u)) continue;
        std::string a = areaName(b);
        if (a.empty()) continue;
        if (a == kCoopOtherAreas) other = true;
        else y.areas.push_back(a);
      }
      if (other) y.areas.push_back(kCoopOtherAreas);
      if (s.sessionWeapons) {
        for (int h = 0; h < 2; ++h) {
          if (!x.weapon[h]) continue;
          y.weapons[h].any = true;
          y.weapons[h].upgrade = x.upgrade[h];
          if (isMine || full || myWeapons.count(x.weapon[h] - x.weapon[h] % 100)) {
            std::string n = weaponName(static_cast<int32_t>(x.weapon[h]) * 100);
            y.weapons[h].name = n.empty() ? "Weapon " + std::to_string(x.weapon[h] * 100) : n;
          }
        }
      }
      v.sessions.push_back(y);
    }
    for (const auto& r : s.bossRecords) {
      CoopPairPlayer::Boss b;
      // Named from your boss list only if theirs is the same one.
      const bool sameList = isMine || s.bossesDefeated.fingerprint == CoopFingerprint(lists.bossFlags);
      const bool yours = haveMine && r.boss < mine.bossesDefeated.bits.size() && mine.bossesDefeated.bits[r.boss] &&
                         mine.bossesDefeated.fingerprint == s.bossesDefeated.fingerprint;
      if ((isMine || yours || (full && sameList)) && r.boss < lists.bossNames.size()) {
        b.name = lists.bossNames[r.boss];
        b.area = r.boss < lists.bossAreas.size() ? lists.bossAreas[r.boss] : "";
      }
      b.deaths = r.deaths;
      b.ttkMin = r.ttkMin == 0xFFFF ? -1 : r.ttkMin;
      b.session = r.session < v.sessions.size() ? r.session : -1;
      b.killMin = r.killMin;
      v.bosses.push_back(b);
    }
    auto mark = [&](uint32_t sec, uint8_t session, uint8_t area, bool have) {
      CoopPairPlayer::Mark m;
      m.have = have;
      m.seconds = sec;
      m.session = session < v.sessions.size() ? session : -1;
      m.area = area == kCoopNone6 ? "" : areaName(area);
      return m;
    };
    v.life = mark(s.lifeSec, s.lifeSession, s.lifeArea, s.lifeSec > 0);
    v.firstDeath = mark(s.firstDeathAt, s.firstDeathSession, s.firstDeathArea, s.firstDeathAt != 0xFFFFFF);
    return v;
  };
  if (haveMine) c.me = view(mine, true);
  if (havePartner) c.partner = view(partner, false);
  if (haveMine && havePartner) {
    c.together = CoopSessionsTogether(mine.sessions, partner.sessions);
    for (const auto& t : c.together) c.togetherMinutes += t.minutes;
  }
  return c;
}

