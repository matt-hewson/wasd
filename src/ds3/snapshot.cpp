// src/ds3/snapshot.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

struct LiveNearby {
  std::string label;     // unfound: at the spoiler tier ("" at Off and Vague: counts only); found: the name
  std::string category;  // unfound at Full: the item's kind, shown beside its name
  bool found = false;
  std::string clock;
  float dist = 0, dy = 0, x = 0, z = 0;
  double angle = 0;  // bearing from straight ahead, clockwise, radians
  bool showHeight = false;
};
struct LiveBoss {
  uint32_t flag = 0;  // its defeat flag (bosses.tsv)
  std::string name;  // "" when hidden by the tier
  bool defeated = false, optional = false, nearest = false, humanType = false;
  bool later = false;  // its route step isn't open yet (BossIsLater)
  std::string opens;   // when later: what opens it at the tier
  int statsLevel = 0;  // 0 none, 1 weakness only, 2 full numbers
  float dist = -1;     // metres to an undefeated boss with a known position, else -1
  std::string weak;
  float rate[8] = {};      // physical, slash, strike, thrust, magic, fire, lightning, dark
  int16_t resist[5] = {};  // poison, toxic, bleed, curse, frost
};
struct LiveRouteStep {
  std::string label, kind, from, wayIn;  // label/from/wayIn at the tier (wayIn only at Full)
  int status = 0;                         // 0 locked, 1 open, 2 done
  int requiredLeft = 0, optionalLeft = 0;
  bool unnamed = false;                   // "A new area beyond ..." (tier Category)
};
struct LiveMissable {
  std::string id, kind, title, text, lose, area;  // at the tier
  int status = 0;     // 0 pending, 1 missed (lockout happened), 2 done, 3 don't care
  bool here = false;  // its area is the current area
  bool automatic = false;  // done because the game shows it (not a mark)
};
struct LiveWeapon {
  std::string slot, name, upgrade;
  bool active = false, twoHanded = false, catalyst = false, haveAr = false;
  bool present[5] = {};
  int base[5] = {}, bonus1h[5] = {}, bonus2h[5] = {};  // Phys, Magic, Fire, Lightning, Dark
  int spellBuff = 0;
  // Level planner: total AR change (1H / 2H), spell buff change, and
  // whether a requirement penalty goes away, for Str, Dex, Int, Fth, Lck +1.
  int plan1h[5] = {}, plan2h[5] = {}, planBuff[5] = {};
  bool planFix[5] = {};
  // Infusions at current stats: total AR (1H / 2H; spell buff for catalysts)
  // of each of the 16 infusion rows; -1 where the row doesn't exist.
  double inf1h[16] = {}, inf2h[16] = {};
  int infCurrent = -1;          // this weapon's infusion index, -1 if it can't be infused
  std::string infusion;         // "Heavy: +18 AR (gem held)" -- the best other infusion, at the grip
  std::string infusionDetail;   // top three
};

// This session's walked trail for the live page's map: a point every
// 1 unit moved, plus deaths and bonfire rests. mapKey = map number * 2 +
// 1 in Untended Graves (map coordinate spaces overlap, so the page only
// draws points from the player's current map and world).
struct TrailPoint {
  float x, y, z;
  int16_t mapKey;
  uint8_t kind;  // 0 walk, 1 death, 2 bonfire rest
};
struct LiveTrail {
  CRITICAL_SECTION lock;
  std::vector<TrailPoint> points;
};
LiveTrail g_trail;
constexpr size_t kTrailMaxPoints = 50000;

void TrailAdd(const TrailPoint& p) {
  EnterCriticalSection(&g_trail.lock);
  if (g_trail.points.size() < kTrailMaxPoints) g_trail.points.push_back(p);
  LeaveCriticalSection(&g_trail.lock);
}

void LiveNearbyList(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
                    const PlayerPose& pose, const PlayerArea& where, const std::string& area,
                    std::vector<LiveNearby>* out);
void LiveBossList(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, const std::string& area,
                  const PlayerPose& pose, std::vector<LiveBoss>* out, int counts[4],
                  std::vector<SessionBoss>* all = nullptr);
void LiveRoute(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
               const std::string& character, const std::string& area, std::vector<LiveRouteStep>* steps,
               std::string* hint, int* hidden);
void LiveMissables(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
                   const std::string& character, const std::string& area, std::vector<LiveMissable>* out,
                   std::string* alert, int* hidden);

struct WindowSnapshot {
  std::string status;  // non-empty: shown instead of values (not attached, game closed...)
  bool paused = false;
  bool haveValues = false;
  int32_t hp = 0, maxHp = 0, fp = 0, maxFp = 0, stamina = 0, maxStamina = 0;
  int32_t level = 0, souls = 0, nextLevelCost = 0;
  bool haveEquipLoad = false;
  double equipLoad = 0.0, maxEquipLoad = 0.0;
  std::string rightHand, leftHand, spell, quickItem;
  PlayerPose pose;
  std::string areaName, lastBonfireName, currentArea;
  double sessionSeconds = 0.0, areaSeconds = 0.0, soulsPerHour = 0.0;
  int32_t sessionDeaths = 0, deathsTotal = 0;
  int itemsFound = -1, itemsTotal = 0;  // world pickups in this map section
  std::string bossesHere, shards;          // bosses in this area, shard counts
  std::string keyItems;                    // "3 / 39 obtained (2 held)"
  std::string upgrade;                     // active right-hand weapon's next upgrade
  std::string nearest;                     // closest unfound pickup, at the spoiler tier
  std::string nearestShort;                // same, overlay width
  std::string itemsScope;                  // "in Firelink Shrine" / "in this map section"
  std::string bossHint;                    // closest undefeated boss's weaknesses
  int bossesDefeated = 0, bossesTotal = 0, bossesLater = 0;  // later: not open yet (BossIsLater)
  int64_t soulsLost = 0, soulsRecovered = 0;
  std::string characterName;
  std::string lastReadTime;
  double lastLatencyMicros = 0.0;
  PerfWindow::Sample perf;
  DWORD pid = 0;
  // Live page only:
  std::vector<LiveNearby> nearby;
  std::vector<LiveBoss> bosses;
  std::vector<LiveWeapon> weapons;
  std::vector<LiveRouteStep> route;  // route hints, at the tier
  std::string routeHint;
  int routeHidden = 0;
  std::vector<LiveMissable> missables;  // missable-content warnings, at the tier
  std::string missableAlert;            // overlay line: a pending warning in this area
  int missablesHidden = 0;
  int bossCounts[4] = {};  // base defeated, base total, DLC defeated, DLC total
  int32_t attrs[9] = {};   // vigor, attunement, endurance, vitality, str, dex, int, faith, luck
  std::string planGain[9];  // level planner: what +1 in each attribute gives (short)
  std::string planDetail[9];
  std::string planBest;     // the attribute that helps the weapon in hand most
  bool gemHeld[16] = {};    // infusion gems in the inventory (index = infusion; 0 = Normal, none needed)
  int mapNumber = 0;
  bool untendedWorld = false;
};

// Co-op state shared between the poll loop and the live page's server:
// this player's state (refreshed by the loop) and the partner's latest
// snapshot (pasted on the results page, or live from a co-op session).
// Never merged into the reader's own state.
struct CoopShared {
  std::mutex lock;
  SharedGameState mine;
  bool mineReady = false;  // flag sets read at least once
  CoopLists lists;         // the lists of mine's flag sets
  SharedGameState partner;
  bool havePartner = false;
  bool partnerLive = false;  // `partner` comes from the running co-op session (and keeps updating)
  uint32_t sequence = 0;
  // The recorded part (areas, levels), summed from the saved sessions:
  // for this character, at this tick (redone every 30s at most).
  std::string recordedFor;
  ULONGLONG recordedAt = 0;
};
CoopShared g_coop;

void ReadMyCoopFlags(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, const ResolvedProfile& profile,
                     SharedGameState* s, CoopLists* lists);
bool LivePageRecentlyPolled();
bool CoopSessionActive();
void StartCoopSessionThread();
void StopCoopSessionThread();

struct WindowShared {
  CRITICAL_SECTION lock;
  WindowSnapshot snapshot;
  HWND hwnd = nullptr;
  HANDLE wakeEvent = nullptr;  // signalled on pause toggle and on shutdown
  volatile LONG paused = 0;
  volatile LONG stop = 0;
  HFONT font = nullptr;  // overlay font, remade only when the text size setting changes
  double scale = 1.0;                        // DPI / 96
};
WindowShared g_window;

void PublishSnapshot(const WindowSnapshot& snap) {
  EnterCriticalSection(&g_window.lock);
  g_window.snapshot = snap;
  LeaveCriticalSection(&g_window.lock);
  PostMessageW(g_window.hwnd, kMsgSnapshot, 0, 0);
}

// DS3's roll tiers by equip-load percentage (Fextralife, Equipment_Load).
const char* RollTypeForLoad(double percent) {
  if (percent < 30.0) return "light roll";
  if (percent < 70.0) return "medium roll";
  if (percent <= 100.0) return "heavy roll";
  return "overloaded";
}

