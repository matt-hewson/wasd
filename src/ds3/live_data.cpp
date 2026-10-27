// src/ds3/live_data.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- live page: data lists, JSON, local server (overlay mode) ----------
//
// The overlay process also serves a dashboard for a second monitor:
// templates/live.html at http://localhost:8765/ (next free port up to
// 8774), polling /api/state twice a second. Bound to 127.0.0.1 only and
// requests must carry a localhost Host header (so another site can't
// read it through DNS rebinding). The page can set the spoiler tier
// (POST /api/tier?t=N) -- the guide's own setting, the same as F9;
// nothing on the page touches the game.

// Every positioned pickup in the player's named area (or, where the area
// has none, in their map section and world): unfound ones first, closest
// first, labelled at the tier; then the found ones, always named (you
// have them -- user's call, 2026-10-06).
void LiveNearbyList(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
                    const PlayerPose& pose, const PlayerArea& where, const std::string& area,
                    std::vector<LiveNearby>* out) {
  const ItemNameTable& names = ItemNames();
  static EventFlagRoots roots;
  static const AreaInfo areaInfo;
  out->clear();
  if (!pose.ok || TreasurePositions().empty()) return;
  if (!roots.eventFlagManCell) roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  const int tier = SpoilerTier();
  const int clearCount = ReadClearCount(hProcess, moduleBase, baseACellOffset);
  const std::string map = where.ok ? where.MapName().substr(0, 3) : "";
  const auto& pickups = AllWorldPickups(hProcess, moduleBase, clearCount);
  std::vector<LiveNearby> unfound, found;
  for (int pass = 0; pass < 2 && unfound.empty() && found.empty(); ++pass) {
    for (const auto& g : pickups) {
      const TreasurePos* p = PickupPosition(g);
      if (!p) continue;
      bool inScope = pass == 0 ? !area.empty() && areaInfo.AreaOf(p->region) == area
                               : (map.empty() || p->map == map) && SameWorld(p->region, where.playRegionId);
      if (!inScope) continue;
      LiveNearby l;
      float dx = p->x - pose.x, dy = p->y - pose.y, dz = p->z - pose.z;
      l.found = ReadEventFlag(hProcess, moduleBase, roots, g.flagId) > 0;
      l.label = l.found ? PickupLabel(names, g.merged) : tier <= kTierVague ? "" : PickupHint(names, g.merged, tier);
      if (!l.found && tier >= kTierFull) l.category = PickupCategory(g.merged);
      l.clock = ClockDirection(pose, dx, dz);
      l.angle = RelativeBearing(pose, dx, dz);
      l.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
      l.dy = dy;
      l.x = p->x;
      l.z = p->z;
      l.showHeight = tier >= kTierCategory;
      (l.found ? found : unfound).push_back(l);
    }
  }
  auto byDist = [](const LiveNearby& a, const LiveNearby& b) { return a.dist < b.dist; };
  std::sort(unfound.begin(), unfound.end(), byDist);
  std::sort(found.begin(), found.end(), byDist);
  out->insert(out->end(), unfound.begin(), unfound.end());
  out->insert(out->end(), found.begin(), found.end());
}

// Bosses in the current area at the tier, plus overall counts
// (counts: base defeated, base total, DLC defeated, DLC total).
// Defeated bosses are always named. Numbers (weaknesses, resistances) only
// at Full, for every boss (user's call, 2026-10-07); undefeated bosses are
// unnamed below Full.
void LiveBossList(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, const std::string& area,
                  const PlayerPose& pose, std::vector<LiveBoss>* out, int counts[4],
                  std::vector<SessionBoss>* all) {
  const std::vector<BossInfo>& bosses = Bosses();
  if (all) all->clear();
  static EventFlagRoots roots;
  if (!roots.eventFlagManCell) roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  const int tier = SpoilerTier();
  out->clear();
  for (int i = 0; i < 4; ++i) counts[i] = 0;
  float bestDist = 1e9f;
  int best = -1;
  for (const auto& b : bosses) {
    bool dead = ReadEventFlag(hProcess, moduleBase, roots, b.flag) > 0;
    bool dlc = b.note.find("DLC") != std::string::npos;
    counts[dlc ? 2 : 0] += dead ? 1 : 0;
    counts[dlc ? 3 : 1] += 1;
    if (all)
      all->push_back({b.flag, b.name, b.area, dead, BossFightPhase(b, dead, [&](uint32_t f) {
                        return ReadEventFlag(hProcess, moduleBase, roots, f) > 0;
                      })});
    if (area.empty() || b.area != area) continue;
    LiveBoss lb;
    lb.flag = b.flag;
    lb.defeated = dead;
    lb.optional = b.status == "optional";
    // Not open yet in the normal route: shown apart, never the boss ahead.
    // What opens it is named at Full only.
    std::string opensAfter;
    lb.later = !dead && BossIsLater(hProcess, moduleBase, roots, b.flag, &opensAfter);
    if (lb.later) lb.opens = tier >= kTierFull ? "Opens after " + opensAfter : "Comes later in the game";
    if (dead || tier >= kTierFull) lb.name = b.name;
    // Weaknesses and resistances at Full only, defeated bosses included
    // (user's call, 2026-10-07; was Vague = weakness, Category = all).
    lb.statsLevel = tier >= kTierFull ? 2 : 0;
    BossStats st = ReadBossStats(hProcess, moduleBase, b);
    lb.humanType = st.humanType;
    if (st.ok && lb.statsLevel >= 1) lb.weak = FormatBossWeakness(st, true);
    if (st.ok && lb.statsLevel == 2) {
      std::memcpy(lb.rate, st.rate, sizeof(lb.rate));
      std::memcpy(lb.resist, st.resist, sizeof(lb.resist));
    }
    auto it = EnemyPlacements().find(BossEntity(b));
    if (!dead && it != EnemyPlacements().end() && pose.ok) {
      float dx = it->second.x - pose.x, dy = it->second.y - pose.y, dz = it->second.z - pose.z;
      float d = std::sqrt(dx * dx + dy * dy + dz * dz);
      lb.dist = d;
      if (!lb.later && d < bestDist) { bestDist = d; best = static_cast<int>(out->size()); }
    }
    out->push_back(lb);
  }
  if (best >= 0) (*out)[best].nearest = true;
}

std::string JsonNum(double v, int decimals = 2) {
  if (!std::isfinite(v)) return "null";
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  return buf;
}

// The page's whole state; trail points from index `trailFrom` on (the page
// sends how many it already has, so each poll only carries new ones).
std::string BuildLiveJson(const WindowSnapshot& s, size_t trailFrom) {
  const int tier = SpoilerTier();
  std::string j = "{";
  auto kv = [&](const char* k, const std::string& v) {
    if (j.size() > 1) j += ",";
    j += "\"";
    j += k;
    j += "\":" + v;
  };
  auto b = [](bool v) { return std::string(v ? "true" : "false"); };
  auto i = [](long long v) { return std::to_string(v); };
  kv("version", JsonString(WASD_VERSION_STRING));
  kv("status", JsonString(s.status));
  kv("paused", b(s.paused));
  kv("haveValues", b(s.haveValues));
  kv("tier", i(tier));
  kv("tierName", JsonString(SpoilerTierName(tier)));
  kv("character", JsonString(s.characterName));
  kv("level", i(s.level));
  kv("souls", i(s.souls));
  kv("nextLevelCost", i(s.nextLevelCost));
  kv("hp", "[" + i(s.hp) + "," + i(s.maxHp) + "]");
  kv("fp", "[" + i(s.fp) + "," + i(s.maxFp) + "]");
  kv("stamina", "[" + i(s.stamina) + "," + i(s.maxStamina) + "]");
  {
    std::string a = "[";
    for (int k = 0; k < 9; ++k) a += (k ? "," : "") + i(s.attrs[k]);
    kv("attrs", a + "]");
    std::string g = "[", d = "[";
    for (int k = 0; k < 9; ++k) {
      g += (k ? "," : "") + JsonString(s.planGain[k]);
      d += (k ? "," : "") + JsonString(s.planDetail[k]);
    }
    kv("plan", "{\"gain\":" + g + "],\"detail\":" + d + "],\"best\":" + JsonString(s.planBest) + "}");
  }
  kv("equipLoad", s.haveEquipLoad ? "[" + JsonNum(s.equipLoad, 1) + "," + JsonNum(s.maxEquipLoad, 1) + "]" : "null");
  kv("roll", JsonString(s.haveEquipLoad && s.maxEquipLoad > 0
                            ? RollTypeForLoad(s.equipLoad / s.maxEquipLoad * 100.0)
                            : ""));
  kv("spell", JsonString(s.spell));
  kv("quickItem", JsonString(s.quickItem));
  kv("area", "{\"region\":" + JsonString(s.areaName) + ",\"name\":" + JsonString(s.currentArea) +
                 ",\"bonfire\":" + JsonString(s.lastBonfireName) + ",\"mapKey\":" + i(s.mapNumber * 2 + (s.untendedWorld ? 1 : 0)) + "}");
  kv("pose", s.pose.ok ? "{\"x\":" + JsonNum(s.pose.x) + ",\"y\":" + JsonNum(s.pose.y) + ",\"z\":" +
                             JsonNum(s.pose.z) + ",\"facing\":" + JsonNum(s.pose.facingRadians, 4) + "}"
                       : "null");
  kv("items", "{\"found\":" + i(s.itemsFound) + ",\"total\":" + i(s.itemsTotal) +
                  ",\"scope\":" + JsonString(s.itemsScope) + "}");
  {
    std::string a = "[";
    for (size_t k = 0; k < s.nearby.size(); ++k) {
      const auto& n = s.nearby[k];
      a += (k ? "," : "");
      a += "{\"label\":" + JsonString(n.label) + ",\"category\":" + JsonString(n.category) +
           ",\"found\":" + b(n.found) + ",\"clock\":" + JsonString(n.clock) +
           ",\"dist\":" +
           JsonNum(n.dist, 1) + ",\"dy\":" + JsonNum(n.dy, 1) + ",\"showHeight\":" + b(n.showHeight) +
           ",\"angle\":" + JsonNum(n.angle, 4) + ",\"x\":" + JsonNum(n.x) + ",\"z\":" + JsonNum(n.z) + "}";
    }
    kv("nearby", a + "]");
  }
  {
    std::string a = "[";
    for (size_t k = 0; k < s.bosses.size(); ++k) {
      const auto& lb = s.bosses[k];
      a += (k ? "," : "");
      a += "{\"name\":" + JsonString(lb.name) + ",\"flag\":" + i(lb.flag) + ",\"defeated\":" + b(lb.defeated) + ",\"optional\":" +
           b(lb.optional) + ",\"nearest\":" + b(lb.nearest) + ",\"human\":" + b(lb.humanType && lb.statsLevel == 2) +
           ",\"later\":" + b(lb.later) + ",\"opens\":" + JsonString(lb.opens) + ",\"statsLevel\":" + i(lb.statsLevel) + ",\"dist\":" + JsonNum(lb.dist, 0) +
           ",\"weak\":" + JsonString(lb.weak);
      if (lb.statsLevel == 2 && !lb.humanType) {
        a += ",\"absorb\":[";
        for (int t = 0; t < 8; ++t) a += (t ? "," : "") + i(Absorption(lb.rate[t]));
        a += "],\"resist\":[";
        for (int t = 0; t < 5; ++t) a += (t ? "," : "") + i(lb.resist[t]);
        a += "]";
      }
      a += "}";
    }
    kv("bosses", a + "]");
  }
  kv("bossesHere", "[" + i(s.bossesDefeated) + "," + i(s.bossesTotal) + "," + i(s.bossesLater) + "]");
  {
    std::string a = "[";
    for (size_t k = 0; k < s.route.size(); ++k) {
      const auto& r = s.route[k];
      a += (k ? "," : "");
      a += "{\"label\":" + JsonString(r.label) + ",\"kind\":" + JsonString(r.kind) + ",\"status\":" + i(r.status) +
           ",\"requiredLeft\":" + i(r.requiredLeft) + ",\"optionalLeft\":" + i(r.optionalLeft) + ",\"unnamed\":" +
           b(r.unnamed) + ",\"from\":" + JsonString(r.from) + ",\"wayIn\":" + JsonString(r.wayIn) + "}";
    }
    kv("route", "{\"hint\":" + JsonString(s.routeHint) + ",\"hidden\":" + i(s.routeHidden) + ",\"steps\":" + a + "]}");
  }
  {
    std::string a = "[";
    for (size_t k = 0; k < s.missables.size(); ++k) {
      const auto& m = s.missables[k];
      a += (k ? "," : "");
      a += "{\"id\":" + JsonString(m.id) + ",\"kind\":" + JsonString(m.kind) + ",\"title\":" + JsonString(m.title) +
           ",\"text\":" + JsonString(m.text) + ",\"lose\":" + JsonString(m.lose) + ",\"area\":" + JsonString(m.area) +
           ",\"status\":" + i(m.status) + ",\"here\":" + b(m.here) + ",\"automatic\":" + b(m.automatic) + "}";
    }
    kv("missables", "{\"alert\":" + JsonString(s.missableAlert) + ",\"hidden\":" + i(s.missablesHidden) +
                        ",\"items\":" + a + "]}");
  }
  kv("bossHint", JsonString(s.bossHint));
  kv("progress", "{\"bossesBase\":[" + i(s.bossCounts[0]) + "," + i(s.bossCounts[1]) + "],\"bossesDlc\":[" +
                     i(s.bossCounts[2]) + "," + i(s.bossCounts[3]) + "],\"shards\":" + JsonString(s.shards) +
                     ",\"keys\":" + JsonString(s.keyItems) + ",\"deathsTotal\":" + i(s.deathsTotal) +
                     ",\"sessionDeaths\":" + i(s.sessionDeaths) + ",\"soulsLost\":" + i(s.soulsLost) +
                     ",\"soulsRecovered\":" + i(s.soulsRecovered) + ",\"soulsPerHour\":" +
                     JsonNum(s.soulsPerHour, 0) + ",\"sessionSeconds\":" + JsonNum(s.sessionSeconds, 0) +
                     ",\"areaSeconds\":" + JsonNum(s.areaSeconds, 0) + "}");
  {
    static const char* kTypes[5] = {"Phys", "Magic", "Fire", "Lightning", "Dark"};
    std::string a = "[";
    for (size_t k = 0; k < s.weapons.size(); ++k) {
      const auto& w = s.weapons[k];
      a += (k ? "," : "");
      a += "{\"slot\":" + JsonString(w.slot) + ",\"name\":" + JsonString(w.name) + ",\"active\":" + b(w.active) +
           ",\"twoHanded\":" + b(w.twoHanded) + ",\"catalyst\":" + b(w.catalyst) + ",\"spellBuff\":" +
           i(w.spellBuff) + ",\"upgrade\":" + JsonString(w.upgrade) + ",\"infusion\":" + JsonString(w.infusion) +
           ",\"infusionDetail\":" + JsonString(w.infusionDetail) + ",\"ar\":[";
      bool first = true;
      for (int t = 0; t < 5 && w.haveAr; ++t) {
        if (!w.present[t]) continue;
        a += std::string(first ? "" : ",") + "{\"type\":\"" + kTypes[t] + "\",\"base\":" + i(w.base[t]) +
             ",\"bonus\":" + i(w.twoHanded ? w.bonus2h[t] : w.bonus1h[t]) + "}";
        first = false;
      }
      a += "]}";
    }
    kv("weapons", a + "]");
  }
  {
    EnterCriticalSection(&g_trail.lock);
    size_t total = g_trail.points.size();
    size_t from = (std::min)(trailFrom, total);
    std::string a = "[";
    for (size_t k = from; k < total; ++k) {
      const auto& p = g_trail.points[k];
      a += (k > from ? "," : "");
      a += "[" + JsonNum(p.x, 1) + "," + JsonNum(p.z, 1) + "," + JsonNum(p.y, 1) + "," + i(p.mapKey) + "," +
           i(p.kind) + "]";
    }
    LeaveCriticalSection(&g_trail.lock);
    kv("trail", "{\"from\":" + i(static_cast<long long>(from)) + ",\"total\":" +
                    i(static_cast<long long>(total)) + ",\"points\":" + a + "]}");
  }
  {
    std::string o = "{";
    for (const auto& d : kOverlaySettingDefs)
      o += std::string(o.size() > 1 ? "," : "") + "\"" + d.key + "\":" + std::to_string(*d.value);
    kv("overlay", o + "}");
  }
  {
    static const char* ids[3] = {"a", "b", "c"};
    std::string m = "[";
    for (int k = 0; k < 6; ++k)
      if (g_pageMuted & (1L << k)) m += std::string(m.size() > 1 ? "," : "") + "\"" + kPagePanels[k] + "\"";
    kv("page", "{\"theme\":\"" + std::string(ids[g_pageTheme % 3]) + "\",\"muted\":" + m + "]}");
  }
  kv("perf", "{\"read\":" + JsonString(s.lastReadTime) + ",\"latencyUs\":" + JsonNum(s.lastLatencyMicros, 1) +
                 ",\"rateHz\":" + JsonNum(s.perf.valid ? s.perf.rateHz : 0.0) + ",\"cpu\":" +
                 JsonNum(s.perf.valid ? s.perf.cpuPercentOfCore : 0.0) + ",\"memMB\":" +
                 JsonNum(s.perf.valid ? s.perf.workingSetMB : 0.0, 1) + "}");
  return j + "}";
}

// The results page's co-op data (GET /api/coop): both players side by
// side, from BuildCoopCompare (already at the spoiler tier), plus the tier
// itself for the page's Spoilers control.
std::string CoopCompareJson(const CoopCompare& c, bool mineReady, bool loaded, const std::string& loadedName,
                            int tier) {
  auto b = [](bool v) { return std::string(v ? "true" : "false"); };
  auto i = [](long long v) { return std::to_string(v); };
  auto pair = [&](const int v[2]) { return "[" + i(v[0]) + "," + i(v[1]) + "]"; };
  auto player = [&](const CoopPlayerView& p) -> std::string {
    if (!p.shown) return "null";
    std::string attrs = "[";
    for (int k = 0; k < 9; ++k) attrs += (k ? "," : "") + i(p.attrs[k]);
    std::string areas = "[";
    for (size_t k = 0; k < p.areas.size(); ++k)
      areas += std::string(k ? "," : "") + "{\"a\":" + JsonString(p.areas[k].name) + ",\"s\":" +
               i(p.areas[k].seconds) + ",\"d\":" + i(p.areas[k].deaths) + "}";
    std::string levels = "[";
    for (size_t k = 0; k < p.levels.size(); ++k)
      levels += std::string(k ? "," : "") + "[" + JsonNum(p.levels[k].first, 3) + "," + i(p.levels[k].second) + "]";
    return "{\"name\":" + JsonString(p.name) + ",\"level\":" + i(p.level) + ",\"ng\":" + i(p.ng) +
           ",\"hp\":" + pair(p.hp) + ",\"fp\":" + pair(p.fp) + ",\"stamina\":" + pair(p.stamina) +
           ",\"attrs\":" + attrs + "],\"souls\":" + i(p.souls) + ",\"deaths\":" + i(p.deaths) +
           ",\"playSeconds\":" + i(p.playSeconds) + ",\"age\":" + i(p.ageSeconds) +
           ",\"area\":" + JsonString(p.area) + ",\"region\":" + JsonString(p.region) +
           ",\"bonfire\":" + JsonString(p.bonfire) + ",\"elsewhere\":" + b(p.elsewhere) +
           ",\"beyond\":" + JsonString(p.beyond) +
           ",\"bosses\":" + pair(p.bosses) + ",\"keys\":" + pair(p.keys) +
           ",\"held\":" + i(p.keysHeld) + ",\"items\":" + pair(p.items) + ",\"areas\":" + areas +
           "],\"levels\":" + levels + "]}";
  };
  auto rows = [&](const std::vector<CoopRow>& v, bool held) {
    std::string a = "[";
    for (size_t k = 0; k < v.size(); ++k) {
      a += std::string(k ? "," : "") + "{\"n\":" + JsonString(v[k].name) + ",\"g\":" + JsonString(v[k].group) +
           ",\"m\":" + b(v[k].me) + ",\"p\":" + b(v[k].partner);
      if (held) a += ",\"mh\":" + b(v[k].meHeld) + ",\"ph\":" + b(v[k].partnerHeld);
      a += "}";
    }
    return a + "]";
  };
  std::string items = "[";
  for (size_t k = 0; k < c.items.size(); ++k)
    items += std::string(k ? "," : "") + "{\"a\":" + JsonString(c.items[k].area) + ",\"t\":" + i(c.items[k].total) +
             ",\"m\":" + i(c.items[k].me) + ",\"p\":" + i(c.items[k].partner) + ",\"b\":" + i(c.items[k].both) + "}";
  std::string notes = "[";
  for (size_t k = 0; k < c.notes.size(); ++k) notes += (k ? "," : "") + JsonString(c.notes[k]);
  return "{\"mineReady\":" + b(mineReady) + ",\"loaded\":" + b(loaded) + ",\"loadedName\":" +
         JsonString(loadedName) + ",\"tier\":" + i(tier) + ",\"me\":" + player(c.me) + ",\"partner\":" +
         player(c.partner) + ",\"where\":{\"sameMap\":" + b(c.sameMap) + ",\"dist\":" + JsonNum(c.dist, 1) +
         ",\"dy\":" + JsonNum(c.dy, 1) + ",\"clock\":" + JsonString(c.clock) + "},\"bosses\":{\"compared\":" +
         b(c.bossesCompared) + ",\"rows\":" + rows(c.bosses, false) + "},\"keys\":{\"compared\":" +
         b(c.keysCompared) + ",\"rows\":" + rows(c.keys, true) + "},\"items\":{\"compared\":" +
         b(c.itemsCompared) + ",\"rows\":" + items + "]},\"notes\":" + notes + "]}";
}

// The pair report's part of /api/coop.
std::string CoopPairJson(const CoopPair& c) {
  auto i = [](long long v) { return std::to_string(v); };
  auto player = [&](const CoopPairPlayer& p) -> std::string {
    if (!p.shown) return "null";
    std::string j = "{\"totals\":{\"seconds\":" + i(p.totals[0]) + ",\"deaths\":" + i(p.totals[1]) + ",\"lost\":" +
                    i(p.totals[2]) + ",\"recovered\":" + i(p.totals[3]) + ",\"gained\":" + i(p.totals[4]) +
                    ",\"longest\":" + i(p.totals[5]) + "},\"sessions\":[";
    for (size_t k = 0; k < p.sessions.size(); ++k) {
      const auto& x = p.sessions[k];
      std::string areas = "[", weapons = "[";
      for (size_t a = 0; a < x.areas.size(); ++a) areas += (a ? "," : "") + JsonString(x.areas[a]);
      for (int h = 0; h < 2; ++h)
        weapons += std::string(h ? "," : "") +
                   (x.weapons[h].any ? "{\"name\":" + JsonString(x.weapons[h].name) + ",\"upgrade\":" + i(x.weapons[h].upgrade) + "}"
                                     : "null");
      j += std::string(k ? "," : "") + "{\"start\":" + i(x.startMin) + ",\"minutes\":" + i(x.minutes) + ",\"deaths\":" +
           i(x.deaths) + ",\"gained\":" + i(x.gained) + ",\"levels\":[" + i(x.levelStart) + "," + i(x.levelEnd) +
           "],\"areas\":" + areas + "],\"kills\":" + i(x.kills) + ",\"weapons\":" + weapons + "]}";
    }
    j += "],\"bosses\":[";
    for (size_t k = 0; k < p.bosses.size(); ++k) {
      const auto& b = p.bosses[k];
      j += std::string(k ? "," : "") + "{\"name\":" + JsonString(b.name) + ",\"area\":" + JsonString(b.area) +
           ",\"deaths\":" + i(b.deaths) + ",\"ttk\":" + (b.ttkMin < 0 ? std::string("null") : i(b.ttkMin)) +
           ",\"session\":" + i(b.session) + ",\"kill\":" + i(b.killMin) + "}";
    }
    auto mark = [&](const CoopPairPlayer::Mark& m) {
      return m.have ? "{\"seconds\":" + i(m.seconds) + ",\"session\":" + i(m.session) + ",\"area\":" + JsonString(m.area) + "}"
                    : std::string("null");
    };
    return j + "],\"life\":" + mark(p.life) + ",\"firstDeath\":" + mark(p.firstDeath) + "}";
  };
  std::string t = "[";
  for (size_t k = 0; k < c.together.size(); ++k)
    t += std::string(k ? "," : "") + "{\"me\":" + i(static_cast<long long>(c.together[k].me)) + ",\"partner\":" +
         i(static_cast<long long>(c.together[k].partner)) + ",\"start\":" + i(c.together[k].startMin) +
         ",\"minutes\":" + i(c.together[k].minutes) + "}";
  return "{\"me\":" + player(c.me) + ",\"partner\":" + player(c.partner) + ",\"together\":{\"minutes\":" +
         i(c.togetherMinutes) + ",\"blocks\":" + t + "]}}";
}

// region_areas.tsv's area names, sorted: the list behind a code's recorded
// time and deaths per area.
const std::vector<std::string>& CoopAreaList() {
  static const std::vector<std::string> list = [] {
    std::set<std::string> names;
    for (const auto& [id, area] : LoadIdNameTable(Paths().Data(L"region_areas.tsv")))
      if (!area.empty()) names.insert(area);
    std::vector<std::string> v(names.begin(), names.end());
    if (v.size() > kCoopMaxAreas) v.resize(kCoopMaxAreas);
    return v;
  }();
  return list;
}

// Every saved session log of one character (sessions\<name>\*.jsonl).
std::vector<std::string> CharacterSessionLogs(const std::string& character) {
  std::vector<std::string> logs;
  std::wstring dir = Paths().Sessions() + L"\\" + SafeFolderName(character);
  WIN32_FIND_DATAW f;
  HANDLE h = FindFirstFileW((dir + L"\\*.jsonl").c_str(), &f);
  if (h == INVALID_HANDLE_VALUE) return logs;
  do logs.push_back(ReadWholeFile(dir + L"\\" + f.cFileName));
  while (FindNextFileW(h, &f));
  FindClose(h);
  return logs;
}

// g_coop.mine's recorded part, summed again when the character changed or
// it's over 30s old. Reads files: call without g_coop.lock held.
void RefreshMyCoopRecord() {
  std::string character;
  std::vector<uint32_t> bossFlags;
  {
    std::lock_guard<std::mutex> guard(g_coop.lock);
    character = g_coop.mine.characterName;
    bossFlags = g_coop.lists.bossFlags;
    if (character.empty() ||
        (character == g_coop.recordedFor && GetTickCount64() - g_coop.recordedAt < 30000))
      return;
  }
  SharedGameState recorded;
  SummariseCoopSessions(CharacterSessionLogs(character), CoopAreaList(), bossFlags, &recorded);
  std::lock_guard<std::mutex> guard(g_coop.lock);
  if (g_coop.mine.characterName != character) return;
  SharedGameState& m = g_coop.mine;
  m.areaListFingerprint = recorded.areaListFingerprint;
  m.areas = recorded.areas;
  m.levels = recorded.levels;
  std::copy(recorded.totals, recorded.totals + 6, m.totals);
  m.sessionAreaBits = recorded.sessionAreaBits;
  m.sessionWeapons = recorded.sessionWeapons;
  m.sessions = recorded.sessions;
  m.bossRecords = recorded.bossRecords;
  m.lifeSec = recorded.lifeSec, m.lifeSession = recorded.lifeSession, m.lifeArea = recorded.lifeArea;
  m.firstDeathAt = recorded.firstDeathAt, m.firstDeathSession = recorded.firstDeathSession;
  m.firstDeathArea = recorded.firstDeathArea;
  g_coop.recordedFor = character;
  g_coop.recordedAt = GetTickCount64();
}

