// src/ds3/route.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- route hints ----------------------------------------
//
// data/route.tsv: the steps of the game in the order they open (main
// path, optional areas, DLC), each with its unlock condition as boss
// defeat flags, the area its way in starts from, its bosses and the way
// in (our own words; sources in the file header). Status per step:
//   locked -- its condition isn't met; open -- met, bosses left;
//   done   -- all its bosses defeated (a step without bosses: once open).
// "Visited" areas: any saved session of this character entered them, the
// player is there now, an item there was picked up, or a boss there is
// dead. Spoiler tiers (user's choice, 2026-10-06):
//   Off      -- nothing
//   Vague    -- only areas you've visited ("Undead Settlement: a required
//               boss is still alive")
//   Category -- also that a new area is open, and from where, unnamed
//   Full     -- names the next area, the way in and its bosses
struct RouteAlt {
  int atLeast = 0;               // 0: a single flag; N: at least N of `flags`
  std::vector<uint32_t> flags;
};
using FlagNeeds = std::vector<std::vector<RouteAlt>>;  // all groups; any alternative per group

std::vector<std::string> SplitStr(const std::string& s, char sep) {
  std::vector<std::string> parts;
  size_t pos = 0;
  while (true) {
    size_t e = s.find(sep, pos);
    parts.push_back(s.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
    if (e == std::string::npos) break;
    pos = e + 1;
  }
  return parts;
}

std::vector<uint32_t> ParseFlagList(const std::string& s) {
  std::vector<uint32_t> flags;
  if (s == "-" || s.empty()) return flags;
  for (const auto& f : SplitStr(s, ',')) flags.push_back(static_cast<uint32_t>(std::strtoul(f.c_str(), nullptr, 10)));
  return flags;
}

// "a b|c 3of:d,e,f" -> groups (all must hold) of alternatives; "-" = none.
FlagNeeds ParseNeeds(const std::string& text) {
  FlagNeeds needs;
  if (text == "-" || text.empty()) return needs;
  for (const auto& group : SplitStr(text, ' ')) {
    std::vector<RouteAlt> alts;
    for (const auto& alt : SplitStr(group, '|')) {
      RouteAlt a;
      size_t colon = alt.find(':');
      if (colon != std::string::npos && alt.find("of") != std::string::npos) {
        a.atLeast = std::atoi(alt.c_str());
        a.flags = ParseFlagList(alt.substr(colon + 1));
      } else {
        a.flags = ParseFlagList(alt);
      }
      alts.push_back(a);
    }
    needs.push_back(alts);
  }
  return needs;
}

// isOn(flag) -> bool. Empty needs hold trivially.
template <class F>
bool NeedsMet(const FlagNeeds& needs, F isOn) {
  for (const auto& group : needs) {
    bool any = false;
    for (const auto& alt : group) {
      int n = 0;
      for (uint32_t f : alt.flags) n += isOn(f) ? 1 : 0;
      any |= alt.atLeast > 0 ? n >= alt.atLeast : (n == static_cast<int>(alt.flags.size()) && n > 0);
    }
    if (!any) return false;
  }
  return true;
}

struct RouteStep {
  std::string id, area, kind, from, wayIn;
  FlagNeeds needs;
  std::vector<uint32_t> bosses;
};

std::vector<RouteStep> LoadRoute() {
  std::vector<RouteStep> out;
  std::ifstream in(Paths().Data(L"route.tsv"));
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    auto f = SplitStr(line, '\t');
    if (f.size() < 7) continue;
    out.push_back({f[0], f[1], f[2], f[4], f[6], ParseNeeds(f[3]), ParseFlagList(f[5])});
  }
  return out;
}

// A boss is "later" when it's undefeated and its route step isn't open
// yet: the game's normal progression doesn't send you to it until other
// bosses are dead (the Dancer after three Lords of Cinder, the Dragonslayer
// Armour after the Dancer, ...). The boss lists show such bosses apart
// instead of as required now (user's request, 2026-10-07). *opensAfter, if
// given, gets what opens the step, e.g. "Dancer of the Boreal Valley" or
// "3 of Abyss Watchers / Yhorm the Giant / ... (1 so far)".
// The rule itself, on any route / boss list and defeat check (unit-tested).
bool BossIsLaterIn(const std::vector<RouteStep>& route, const std::vector<BossInfo>& bosses, uint32_t flag,
                   const std::function<bool(uint32_t)>& isDead, std::string* opensAfter) {
  if (isDead(flag)) return false;
  auto bossName = [&](uint32_t f) {
    for (const auto& b : bosses)
      if (b.flag == f) return b.name;
    return std::string("a boss");
  };
  for (const auto& st : route) {
    if (std::find(st.bosses.begin(), st.bosses.end(), flag) == st.bosses.end()) continue;
    if (NeedsMet(st.needs, isDead)) return false;
    if (opensAfter) {
      std::string text;
      for (const auto& group : st.needs) {
        std::string g;
        for (const auto& alt : group) {
          std::string names;
          int n = 0;
          for (uint32_t f : alt.flags) {
            names += (names.empty() ? "" : alt.atLeast > 0 ? " / " : " and ") + bossName(f);
            n += isDead(f) ? 1 : 0;
          }
          if (alt.atLeast > 0) names = std::to_string(alt.atLeast) + " of " + names + " (" + std::to_string(n) + " so far)";
          g += (g.empty() ? "" : " or ") + names;
        }
        text += (text.empty() ? "" : " and ") + g;
      }
      *opensAfter = text;
    }
    return true;
  }
  return false;
}

bool BossIsLater(HANDLE hProcess, uintptr_t moduleBase, const EventFlagRoots& roots, uint32_t flag,
                 std::string* opensAfter) {
  static std::vector<RouteStep> route = LoadRoute();
  const std::vector<BossInfo>& bosses = Bosses();
  return BossIsLaterIn(route, bosses, flag,
                       [&](uint32_t f) { return ReadEventFlag(hProcess, moduleBase, roots, f) > 0; }, opensAfter);
}

// Areas this character entered in any saved session ("enter" events),
// rescanned at most every 30s.
std::vector<std::string> SessionAreasVisited(const std::string& character) {
  static std::string cachedFor;
  static std::vector<std::string> cached;
  static ULONGLONG cachedAt = 0;
  if (character == cachedFor && GetTickCount64() - cachedAt < 30000) return cached;
  cachedFor = character;
  cachedAt = GetTickCount64();
  cached.clear();
  if (character.empty()) return cached;
  std::wstring dir = Paths().Sessions() + L"\\" + SafeFolderName(character);
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*.jsonl").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return cached;
  do {
    std::ifstream in(dir + L"\\" + fd.cFileName);
    std::string line;
    while (std::getline(in, line)) {
      if (line.find("\"type\":\"enter\"") == std::string::npos) continue;
      size_t a = line.find("\"area\":\"");
      if (a == std::string::npos) continue;
      a += 8;
      std::string area = line.substr(a, line.find('"', a) - a);
      if (std::find(cached.begin(), cached.end(), area) == cached.end()) cached.push_back(area);
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return cached;
}

// Areas the character has been to: saved sessions, the current area, areas
// with a dead boss, areas with a picked-up item.
std::vector<std::string> VisitedAreas(HANDLE hProcess, uintptr_t moduleBase, const EventFlagRoots& roots,
                                      uintptr_t baseACellOffset, const std::string& character,
                                      const std::string& currentArea) {
  const std::vector<BossInfo>& bosses = Bosses();
  static const AreaInfo areaInfo;
  std::vector<std::string> visited = SessionAreasVisited(character);
  auto visit = [&](const std::string& a) {
    if (!a.empty() && std::find(visited.begin(), visited.end(), a) == visited.end()) visited.push_back(a);
  };
  visit(currentArea);
  for (const auto& b : bosses)
    if (ReadEventFlag(hProcess, moduleBase, roots, b.flag) > 0) visit(b.area);
  if (!TreasurePositions().empty()) {
    int clearCount = ReadClearCount(hProcess, moduleBase, baseACellOffset);
    for (const auto& g : AllWorldPickups(hProcess, moduleBase, clearCount))
      if (ReadEventFlag(hProcess, moduleBase, roots, g.flagId) > 0) visit(areaInfo.AreaOf(PickupRegion(g)));
  }
  return visited;
}

// A route step's state for the player: open (its gate is met), done (open
// and every boss of it dead), visited (they've been to its area),
// required / optional bosses still alive.
struct RouteEval {
  bool open = false, done = false, visited = false;
  int requiredLeft = 0, optionalLeft = 0;
};

// The step the route hint is about: where to go next. The earliest open
// step (main first, then optional, then DLC) that is somewhere not yet
// visited -- its way in is the news -- or that still has a required boss
// alive, which blocks the way on (the Dancer, once three Lords are dead,
// in an area already visited). A step with only optional bosses left
// doesn't hold the hint up (user's report 2026-10-09: in Undead Settlement
// with Vordt dead and the optional Greatwood alive, the hint pointed back
// at the settlement's own way in). If nothing like that is left, the
// earliest open step that isn't done, so optional bosses still get a
// mention. -1 when everything is done.
int PickNextRouteStep(const std::vector<RouteStep>& route, const std::vector<RouteEval>& ev) {
  for (int pass = 0; pass < 2; ++pass)
    for (const char* kind : {"main", "optional", "dlc"})
      for (size_t i = 0; i < route.size(); ++i) {
        const RouteEval& e = ev[i];
        if (route[i].kind != kind || !e.open || e.done) continue;
        if (pass == 1 || !e.visited || e.requiredLeft > 0) return static_cast<int>(i);
      }
  return -1;
}

struct RouteView {
  std::vector<LiveRouteStep> steps;  // in route order, at the tier
  std::string hint;                  // the suggested next step, at the tier ("" at Off)
  int hidden = 0;                    // steps not shown at this tier
};

RouteView BuildRouteView(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
                         const std::string& character, const std::string& currentArea, int tier) {
  static std::vector<RouteStep> route = LoadRoute();
  const std::vector<BossInfo>& bosses = Bosses();
  static EventFlagRoots roots;
  if (!roots.eventFlagManCell) roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  RouteView view;
  if (route.empty()) {
    view.hint = "data/route.tsv missing";
    return view;
  }
  std::unordered_map<uint32_t, bool> dead;
  auto isDead = [&](uint32_t flag) {
    auto it = dead.find(flag);
    if (it != dead.end()) return it->second;
    return dead[flag] = ReadEventFlag(hProcess, moduleBase, roots, flag) > 0;
  };
  auto bossName = [&](uint32_t flag) {
    for (const auto& b : bosses)
      if (b.flag == flag) return b.name;
    return std::string("a boss");
  };
  auto bossRequired = [&](uint32_t flag) {
    for (const auto& b : bosses)
      if (b.flag == flag) return b.status == "required";
    return true;
  };

  std::vector<std::string> visited =
      VisitedAreas(hProcess, moduleBase, roots, baseACellOffset, character, currentArea);
  auto wasVisited = [&](const std::string& a) { return std::find(visited.begin(), visited.end(), a) != visited.end(); };

  // Status per step.
  std::vector<RouteEval> ev(route.size());
  for (size_t i = 0; i < route.size(); ++i) {
    const auto& st = route[i];
    bool open = NeedsMet(st.needs, isDead);
    RouteEval& e = ev[i];
    e.open = open;
    for (uint32_t f : st.bosses) {
      if (isDead(f)) continue;
      (bossRequired(f) ? e.requiredLeft : e.optionalLeft) += 1;
    }
    e.done = open && e.requiredLeft == 0 && e.optionalLeft == 0;
    e.visited = wasVisited(st.area);
  }

  // The step list at the tier.
  if (tier == kTierOff) {
    view.hidden = static_cast<int>(route.size());
    return view;
  }
  for (size_t i = 0; i < route.size(); ++i) {
    const auto& st = route[i];
    const RouteEval& e = ev[i];
    LiveRouteStep s;
    s.kind = st.kind;
    s.status = e.done ? 2 : e.open ? 1 : 0;
    s.requiredLeft = e.requiredLeft;
    s.optionalLeft = e.optionalLeft;
    if (tier >= kTierFull) {
      s.label = st.id == "dancer" ? st.area + " (return)" : st.area;
      s.wayIn = st.wayIn;
      s.from = st.from;
    } else if (e.visited && e.open) {
      s.label = st.area;
    } else if (tier == kTierCategory && e.open && !e.done) {
      s.label = "A new area beyond " + (wasVisited(st.from) ? st.from : std::string("somewhere you've been"));
      s.unnamed = true;
    } else {
      ++view.hidden;
      continue;
    }
    view.steps.push_back(s);
  }

  // The suggestion: where to go next (PickNextRouteStep).
  auto bossesText = [&](const RouteStep& st) {
    std::string names;
    for (uint32_t f : st.bosses)
      if (!isDead(f)) names += (names.empty() ? "" : ", ") + bossName(f) + (bossRequired(f) ? "" : " (optional)");
    return names;
  };
  auto unfinishedHere = [&](size_t i) {
    const RouteEval& e = ev[i];
    return route[i].area + ": " +
           (e.requiredLeft ? std::string(e.requiredLeft == 1 ? "a required boss is" : "required bosses are")
                           : std::string(e.optionalLeft == 1 ? "an optional boss is" : "optional bosses are")) +
           " still alive";
  };
  int next = PickNextRouteStep(route, ev);
  if (next < 0) {
    view.hint = "Everything on the route is done.";
  } else if (tier >= kTierFull) {
    const auto& st = route[next];
    std::string b = bossesText(st);
    view.hint = (st.kind == "main" ? "Next: " : "Open: ") + (st.id == "dancer" ? st.area + " (return)" : st.area) +
                " -- " + st.wayIn + (b.empty() ? "" : "; boss: " + b);
  } else {
    // Vague / Category: the first unfinished step in an area you've been to;
    // at Category a new area comes first if the route reaches it sooner.
    int visitedNext = -1;
    for (size_t i = 0; i < route.size() && visitedNext < 0; ++i)
      if (ev[i].open && !ev[i].done && ev[i].visited && (ev[i].requiredLeft || ev[i].optionalLeft))
        visitedNext = static_cast<int>(i);
    bool newFirst = tier == kTierCategory && !ev[next].visited && (visitedNext < 0 || next < visitedNext);
    if (newFirst)
      view.hint = "A new area is open beyond " +
                  (wasVisited(route[next].from) ? route[next].from : std::string("somewhere you've been")) + ".";
    else if (visitedNext >= 0)
      view.hint = unfinishedHere(visitedNext) + ".";
    else
      view.hint = "Nothing unfinished in the areas you've visited -- explore onward.";
  }
  return view;
}

// route [--full]: the route at the spoiler tier.
int RunRoute() {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  auto profile = ResolvePlayerProfile(hProcess, moduleBase, moduleSize);
  auto resolved = ResolvePlayerHp(hProcess, moduleBase, moduleSize);
  PlayerArea where = ReadPlayerArea(hProcess, moduleBase, resolved.entityAddress, 0);
  std::string area = AreaInfo().AreaOf(where.playRegionId);
  std::string character = profile.xBase ? ReadCharacterName(hProcess, profile.xBase) : "";
  const int tier = SpoilerTier();
  RouteView v = BuildRouteView(hProcess, moduleBase, moduleSize, profile.baseACellOffset, character, area, tier);
  static const char* kStatus[] = {"locked", "open", "done"};
  Log("Route (spoiler tier %s):", SpoilerTierName(tier));
  for (const auto& s : v.steps) {
    std::string left = s.status == 1 ? " (" + std::to_string(s.requiredLeft) + " required, " +
                                           std::to_string(s.optionalLeft) + " optional boss(es) left)"
                                     : "";
    Log("  %-7s %-9s %s%s", kStatus[s.status], s.kind.c_str(), s.label.c_str(), left.c_str());
    if (!s.wayIn.empty() && s.status != 2) Log("            way in: %s", s.wayIn.c_str());
  }
  if (v.hidden) Log("  (%d step(s) not shown at tier %s; F9 in game or --full)", v.hidden, SpoilerTierName(tier));
  Log("Hint: %s", v.hint.empty() ? "(hidden at tier Off)" : v.hint.c_str());
  CloseHandle(hProcess);
  return 0;
}

void LiveRoute(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
               const std::string& character, const std::string& area, std::vector<LiveRouteStep>* steps,
               std::string* hint, int* hidden) {
  RouteView v = BuildRouteView(hProcess, moduleBase, moduleSize, baseACellOffset, character, area, SpoilerTier());
  *steps = std::move(v.steps);
  *hint = v.hint;
  *hidden = v.hidden;
}

