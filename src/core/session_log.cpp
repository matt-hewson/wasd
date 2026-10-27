// src/core/session_log.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// One boss as the session recorder sees it (Bosses() order).
struct SessionBoss {
  uint32_t flag = 0;
  std::string name, area;
  bool defeated = false;
  int fightPhase = 0;  // its fight is on, in this phase (BossFightPhase); 0 = not
};
// The weapon in one hand; id -1 = not read yet.
struct SessionWeapon {
  int32_t id = -1;  // the weapon's param id (upgrade in the last two digits)
  std::string name;
  int upgrade = 0;
};

struct SessionContext {
  int32_t level = 0, souls = 0, deathNum = 0;
  uint32_t playTimeMs = 0;
  std::string area, characterName;
  PlayerPose pose;
  int32_t lastBonfireId = -1;
  std::string lastBonfireName;
  std::vector<SessionBoss> bosses;  // every boss's defeat flag and fight; empty = not read yet
  SessionWeapon hands[2];           // right, left
};

// Columns of sessions/<character>/<stamp>.perf.csv (see SessionLog::WritePerf):
// local time at the window's end, session seconds (the .jsonl's "t"), the
// window's length and poll count, poll rate, read latency (avg / max, of
// one tick's reads), this process's CPU (% of one core) and memory.
constexpr const char kPerfCsvHeader[] =
    "time,session_t,window_s,ticks,rate_hz,latency_avg_us,latency_max_us,cpu_pct_core,working_set_mb,private_mb";

class SessionLog {
 public:
  bool enabled = false;
  std::wstring path;  // set once the file is opened
  std::vector<std::string>* capture = nullptr;  // tests: lines go here, never to a file

  void Write(const std::string& line, double sessionSeconds) {
    if (capture) {
      capture->push_back(line);
      return;
    }
    if (!enabled) return;
    if (!file_) {
      pending_.push_back(line);
      if (sessionSeconds < kMinRecordedSessionSeconds) return;
      Open();
      if (!file_) return;
      for (const auto& l : pending_) std::fputs((l + "\n").c_str(), file_);
      pending_.clear();
    } else {
      std::fputs((line + "\n").c_str(), file_);
    }
    std::fflush(file_);
  }
  // Perf figures for the Milestone 5 stress test: one CSV row per perf
  // window (5s), in <stamp>.perf.csv beside the .jsonl. It opens with the
  // .jsonl (rows before that are buffered), so a session too short to be
  // saved leaves no CSV either. Rows come from PerfCsvRow.
  void WritePerf(const std::string& row) {
    if (!enabled) return;
    if (!perfFile_) {
      pendingPerf_.push_back(row);
      return;
    }
    std::fputs((row + "\n").c_str(), perfFile_);
    std::fflush(perfFile_);
  }
  void SetCharacter(const std::string& name) {
    if (characterName_.empty()) characterName_ = name;
  }
  void Close() {
    if (file_) std::fclose(file_);
    file_ = nullptr;
    if (perfFile_) std::fclose(perfFile_);
    perfFile_ = nullptr;
  }
  // Ready for the next session (another character): `enabled` and
  // `capture` are kept.
  void Reset() {
    Close();
    pending_.clear();
    pendingPerf_.clear();
    characterName_.clear();
    path.clear();
  }
  ~SessionLog() { Close(); }

 private:
  void Open() {
    std::wstring dir = Paths().Sessions() + L"\\" + SafeFolderName(characterName_);
    EnsureDir(dir);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[32];
    std::swprintf(stamp, 32, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond);
    path = dir + L"\\" + stamp + L".jsonl";
    // Shared for reading (not writing), so the session can be read -- or a
    // `report` built -- while the game is still being played.
    file_ = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
    if (file_) Log("Recording this session to %ls", path.c_str());
    if (!file_) return;
    std::wstring perfPath = dir + L"\\" + stamp + L".perf.csv";
    perfFile_ = _wfsopen(perfPath.c_str(), L"w", _SH_DENYWR);
    if (!perfFile_) return;
    std::fputs((std::string(kPerfCsvHeader) + "\n").c_str(), perfFile_);
    for (const auto& r : pendingPerf_) std::fputs((r + "\n").c_str(), perfFile_);
    pendingPerf_.clear();
    std::fflush(perfFile_);
  }
  FILE* file_ = nullptr;
  FILE* perfFile_ = nullptr;
  std::vector<std::string> pending_, pendingPerf_;
  std::string characterName_;
};

// SessionStats + recording. Live modes call Tick() once per polling tick
// and Finish() on exit.
struct SessionTracker {
  SessionStats stats;
  SessionLog log;
  int32_t lastLevel = -1;
  std::string lastArea;
  double nextSampleAt = 0.0;
  int32_t lastDeathNum = 0;
  uint32_t lastPlayTimeMs = 0;
  bool finished = false;
  bool haveTrail = false;
  float trailX = 0, trailY = 0, trailZ = 0;
  double trailT = -1e9;
  int32_t lastBonfireId = -1;
  std::vector<bool> lastBossDefeated;  // from the previous read, Bosses() order
  int32_t lastHandIds[2] = {-1, -1};
  // The boss fight on at the last read, kept kBossFightGraceSeconds after it
  // goes off. A death counted against it blocks that boss until a read shows
  // its fight off (its flags stay set until the map reloads).
  uint32_t fightFlag = 0, fightBlocked = 0;
  std::string fightName;
  int fightPhase = 0;
  double fightSeenAt = -1e9;
  std::string character;  // whose session this is

  // Another character loaded: a different name, or play time going back.
  bool IsOtherCharacter(const SessionContext& c) const {
    if (!c.characterName.empty() && c.characterName != character) return true;
    return c.playTimeMs + kPlayTimeBackSlackMs < lastPlayTimeMs;
  }
  // Ends this session and starts afresh for the next character.
  void StartOver() {
    Finish();
    log.Reset();
    SessionTracker fresh;
    fresh.log.enabled = log.enabled;
    fresh.log.capture = log.capture;
    *this = fresh;
  }

  bool Tick(double dt, const SessionContext& c) {
    char buf[512];
    if (stats.started && IsOtherCharacter(c)) {
      Log("Another character loaded (%s, play time %s): new session.", c.characterName.c_str(),
          FormatDuration(c.playTimeMs / 1000.0).c_str());
      StartOver();
    }
    bool firstTick = !stats.started;
    if (firstTick) {
      character = c.characterName;
      log.SetCharacter(c.characterName);
      SYSTEMTIME st;
      GetLocalTime(&st);
      char when[32];
      std::snprintf(when, sizeof(when), "%04u-%02u-%02uT%02u:%02u:%02u", st.wYear, st.wMonth,
                    st.wDay, st.wHour, st.wMinute, st.wSecond);
      std::snprintf(buf, sizeof(buf),
                    "{\"type\":\"start\",\"t\":0,\"character\":%s,\"level\":%d,\"deathsTotal\":%d,"
                    "\"playTimeMs\":%u,\"startedAt\":\"%s\"}",
                    JsonString(c.characterName).c_str(), c.level, c.deathNum, c.playTimeMs, when);
      log.Write(buf, 0.0);
      lastLevel = c.level;
    }
    bool event = stats.Tick(dt, c.souls, c.deathNum, c.area);
    double t = stats.seconds;
    if (!c.area.empty() && c.area != lastArea) {
      std::snprintf(buf, sizeof(buf), "{\"type\":\"enter\",\"t\":%.1f,\"area\":%s}", t,
                    JsonString(c.area).c_str());
      log.Write(buf, t);
      lastArea = c.area;
    }
    // Placeholder (0, 1, 0) is what an unloaded character reads; skip it.
    bool realPose = c.pose.ok && !(c.pose.x == 0.0f && c.pose.y == 1.0f && c.pose.z == 0.0f);
    if (realPose) {
      float dx = c.pose.x - trailX, dy = c.pose.y - trailY, dz = c.pose.z - trailZ;
      bool moved = !haveTrail || dx * dx + dy * dy + dz * dz >= kTrailMinStep * kTrailMinStep;
      if (moved && t - trailT >= kTrailMinSeconds) {
        std::snprintf(buf, sizeof(buf), "{\"type\":\"pos\",\"t\":%.1f,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}",
                      t, c.pose.x, c.pose.y, c.pose.z);
        log.Write(buf, t);
        trailX = c.pose.x;
        trailY = c.pose.y;
        trailZ = c.pose.z;
        trailT = t;
        haveTrail = true;
      }
      if (c.lastBonfireId > 0 && c.lastBonfireId != lastBonfireId) {
        if (lastBonfireId != -1) {  // not on the first tick: that's where the session started
          std::snprintf(buf, sizeof(buf),
                        "{\"type\":\"bonfire\",\"t\":%.1f,\"name\":%s,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}",
                        t, JsonString(c.lastBonfireName).c_str(), c.pose.x, c.pose.y, c.pose.z);
          log.Write(buf, t);
        }
        lastBonfireId = c.lastBonfireId;
      }
    }
    // The fight on now: the one already followed while it lasts, else the
    // first one on (normally there's only one).
    const SessionBoss* on = nullptr;
    for (const auto& b : c.bosses) {
      if (b.flag == fightBlocked) {
        if (b.fightPhase) continue;
        fightBlocked = 0;
      }
      if (b.fightPhase && (!on || b.flag == fightFlag)) on = &b;
    }
    if (on) fightFlag = on->flag, fightName = on->name, fightPhase = on->fightPhase, fightSeenAt = t;
    if (stats.eventLost >= 0) {
      std::string boss;
      if (fightFlag && t - fightSeenAt <= kBossFightGraceSeconds) {
        boss = ",\"boss\":" + std::to_string(fightFlag) + ",\"bossName\":" + JsonString(fightName) +
               ",\"phase\":" + std::to_string(fightPhase);
        fightBlocked = fightFlag;
        fightFlag = 0;
      }
      std::snprintf(buf, sizeof(buf),
                    "{\"type\":\"death\",\"t\":%.1f,\"area\":%s,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,"
                    "\"soulsLost\":%lld,\"deathsTotal\":%d%s}",
                    t, JsonString(lastArea).c_str(), c.pose.x, c.pose.y, c.pose.z,
                    static_cast<long long>(stats.eventLost), c.deathNum, boss.c_str());
      log.Write(buf, t);
    }
    // Boss kills: a defeat flag that was clear on the last read and is set
    // now. The first read only sets the baseline, and so does any read in
    // the first kSessionSettleSeconds.
    if (!c.bosses.empty()) {
      if (lastBossDefeated.size() == c.bosses.size()) {
        for (size_t k = 0; k < c.bosses.size(); ++k) {
          if (lastBossDefeated[k] || !c.bosses[k].defeated) continue;
          if (c.bosses[k].flag == fightFlag) fightFlag = 0;
          if (t < kSessionSettleSeconds) continue;
          std::snprintf(buf, sizeof(buf),
                        "{\"type\":\"boss\",\"t\":%.1f,\"flag\":%u,\"name\":%s,\"area\":%s,\"playTimeMs\":%u}", t,
                        c.bosses[k].flag, JsonString(c.bosses[k].name).c_str(), JsonString(c.bosses[k].area).c_str(),
                        c.playTimeMs);
          log.Write(buf, t);
        }
      }
      lastBossDefeated.clear();
      for (const auto& b : c.bosses) lastBossDefeated.push_back(b.defeated);
    }
    // The weapon in each hand: logged when it changes, and once at the start.
    for (int h = 0; h < 2; ++h) {
      const SessionWeapon& w = c.hands[h];
      if (w.id == -1 || w.id == lastHandIds[h]) continue;
      std::snprintf(buf, sizeof(buf), "{\"type\":\"gear\",\"t\":%.1f,\"slot\":\"%s\",\"id\":%d,\"name\":%s,\"upgrade\":%d}",
                    t, h == 0 ? "R" : "L", w.id, JsonString(w.name).c_str(), w.upgrade);
      log.Write(buf, t);
      lastHandIds[h] = w.id;
    }
    if (stats.eventRecovered >= 0) {
      std::snprintf(buf, sizeof(buf), "{\"type\":\"bloodstain\",\"t\":%.1f,\"souls\":%lld}", t,
                    static_cast<long long>(stats.eventRecovered));
      log.Write(buf, t);
    }
    if (!firstTick && c.level != lastLevel && c.level > 0) {
      std::snprintf(buf, sizeof(buf), "{\"type\":\"level\",\"t\":%.1f,\"from\":%d,\"to\":%d}", t,
                    lastLevel, c.level);
      log.Write(buf, t);
      lastLevel = c.level;
    }
    if (t >= nextSampleAt) {
      std::snprintf(buf, sizeof(buf),
                    "{\"type\":\"sample\",\"t\":%.1f,\"level\":%d,\"souls\":%d,\"deathsTotal\":%d,"
                    "\"area\":%s,\"playTimeMs\":%u}",
                    t, c.level, c.souls, c.deathNum, JsonString(lastArea).c_str(), c.playTimeMs);
      log.Write(buf, t);
      nextSampleAt = t + kSessionSampleSeconds;
    }
    lastDeathNum = c.deathNum;
    lastPlayTimeMs = c.playTimeMs;
    return event;
  }

  void Finish() {
    if (finished || !stats.started) return;
    finished = true;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"type\":\"end\",\"t\":%.1f,\"seconds\":%.1f,\"deaths\":%d,\"gained\":%lld,"
                  "\"lost\":%lld,\"recovered\":%lld,\"level\":%d,\"deathsTotal\":%d,"
                  "\"playTimeMs\":%u}",
                  stats.seconds, stats.seconds, stats.deaths, static_cast<long long>(stats.gained),
                  static_cast<long long>(stats.lost), static_cast<long long>(stats.recovered),
                  lastLevel, lastDeathNum, lastPlayTimeMs);
    log.Write(buf, stats.seconds);
    log.Close();
  }
};



