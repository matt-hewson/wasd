// src/ds3/missables.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- missable-content warnings --------------------------
//
// data/missables.tsv: NPC questline steps, missable items and ending
// requirements, each with when it becomes relevant, its lockout trigger
// and (where the game shows it) a "done" condition, all as event flags
// (sources and syntax in the file header). Most quest-step flags are
// unknown, so the player can also mark an entry done or "don't care" on
// the live page; marks are kept per character in
// sessions/<character>/missables.txt (git-ignored with sessions/). We
// add flags to the data as we find them with `flagwatch` (user's choice,
// 2026-10-06). Status: pending; missed (the trigger happened first); done
// (by the game or a mark); don't care.
// Spoiler tiers: Off nothing; Vague only "something here can still be
// missed" in visited areas; Category the kind of thing ("an NPC questline
// step") and whether a boss kill locks it; Full everything.
struct Missable {
  std::string id, kind, topic, area, shortText, doText, loseText;
  FlagNeeds relevant, trigger, done;
};

std::vector<Missable> LoadMissables() {
  std::vector<Missable> out;
  std::ifstream in(Paths().Data(L"missables.tsv"));
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    auto f = SplitStr(line, '\t');
    if (f.size() < 10) continue;
    out.push_back({f[0], f[1], f[2], f[3], f[7], f[8], f[9], ParseNeeds(f[4]), ParseNeeds(f[5]), ParseNeeds(f[6])});
  }
  return out;
}

// The player's marks (id -> "done" / "ignore"), per character. Written by
// the page server thread, read by the poller: guarded by an SRW lock.
SRWLOCK g_missMarksLock = SRWLOCK_INIT;
std::string g_missMarksFor;
std::unordered_map<std::string, std::string> g_missMarks;

std::wstring MissableMarksPath(const std::string& character) {
  return Paths().Sessions() + L"\\" + SafeFolderName(character) + L"\\missables.txt";
}

void LoadMissableMarksLocked(const std::string& character) {  // caller holds the lock exclusively
  if (g_missMarksFor == character) return;
  g_missMarksFor = character;
  g_missMarks.clear();
  std::ifstream in(MissableMarksPath(character));
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    g_missMarks[line.substr(0, eq)] = line.substr(eq + 1);
  }
}

std::unordered_map<std::string, std::string> MissableMarks(const std::string& character) {
  AcquireSRWLockExclusive(&g_missMarksLock);
  LoadMissableMarksLocked(character);
  auto copy = g_missMarks;
  ReleaseSRWLockExclusive(&g_missMarksLock);
  return copy;
}

// state: done / ignore / clear. False for an unknown id or state.
bool SetMissableMark(const std::string& character, const std::string& id, const std::string& state) {
  static std::vector<Missable> all = LoadMissables();
  if (state != "done" && state != "ignore" && state != "clear") return false;
  if (std::none_of(all.begin(), all.end(), [&](const Missable& m) { return m.id == id; })) return false;
  AcquireSRWLockExclusive(&g_missMarksLock);
  LoadMissableMarksLocked(character);
  if (state == "clear") g_missMarks.erase(id);
  else g_missMarks[id] = state;
  EnsureDir(Paths().Sessions() + L"\\" + SafeFolderName(character));
  std::ofstream out(MissableMarksPath(character), std::ios::trunc);
  out << "# Missable-content marks made on the live page (done / ignore), per entry of data/missables.tsv.\n";
  for (const auto& [k, v] : g_missMarks) out << k << "=" << v << "\n";
  ReleaseSRWLockExclusive(&g_missMarksLock);
  return true;
}

struct MissablesView {
  std::vector<LiveMissable> items;  // at the tier, pending first
  std::string alert;                // a pending warning in the current area, at the tier
  int hidden = 0;                   // relevant entries not shown at this tier
};

MissablesView BuildMissablesView(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
                                 const std::string& character, const std::string& currentArea, int tier) {
  static std::vector<Missable> all = LoadMissables();
  static EventFlagRoots roots;
  if (!roots.eventFlagManCell) roots = ResolveEventFlagRoots(hProcess, moduleBase, moduleSize);
  MissablesView view;
  if (all.empty()) {
    view.alert = tier > kTierOff ? "data/missables.tsv missing" : "";
    return view;
  }
  std::unordered_map<uint32_t, bool> cache;
  auto isOn = [&](uint32_t flag) {
    auto it = cache.find(flag);
    if (it != cache.end()) return it->second;
    return cache[flag] = ReadEventFlag(hProcess, moduleBase, roots, flag) > 0;
  };
  auto marks = MissableMarks(character);
  std::vector<std::string> visited = VisitedAreas(hProcess, moduleBase, roots, baseACellOffset, character, currentArea);
  auto wasVisited = [&](const std::string& a) { return std::find(visited.begin(), visited.end(), a) != visited.end(); };
  auto kindText = [](const std::string& k) {
    return k == "npc" ? std::string("An NPC questline step") : k == "item" ? std::string("A missable item")
                                                                          : std::string("An ending requirement");
  };
  for (const auto& m : all) {
    if (!NeedsMet(m.relevant, isOn)) continue;
    LiveMissable lm;
    lm.id = m.id;
    lm.kind = m.kind;
    lm.here = !currentArea.empty() && m.area == currentArea;
    bool autoDone = !m.done.empty() && NeedsMet(m.done, isOn);
    auto mark = marks.find(m.id);
    bool triggered = !m.trigger.empty() && NeedsMet(m.trigger, isOn);
    lm.automatic = autoDone;
    lm.status = autoDone ? 2
                : mark != marks.end() ? (mark->second == "done" ? 2 : 3)
                : triggered           ? 1
                                      : 0;
    bool seen = wasVisited(m.area);
    if (tier == kTierOff || (tier == kTierVague && (!seen || lm.status == 1))) {
      ++view.hidden;
      continue;
    }
    if (tier >= kTierFull) {
      lm.title = m.topic + ": " + m.shortText;
      lm.text = m.doText;
      lm.lose = m.loseText;
      lm.area = m.area;
    } else if (tier == kTierCategory) {
      lm.title = kindText(m.kind);
      lm.text = m.trigger.empty() ? "" : "A boss kill or a later step locks it out.";
      lm.area = seen ? m.area : "somewhere you haven't been yet";
    } else {
      lm.title = "Something here can still be missed";
      lm.area = m.area;
    }
    if (lm.status == 0 && lm.here && view.alert.empty())
      view.alert = tier >= kTierFull ? m.topic + ": " + m.shortText
                   : tier == kTierCategory ? kindText(m.kind) + " here can be missed"
                                           : "Something here can be missed";
    view.items.push_back(lm);
  }
  // Pending in this area first, then pending, missed, done, don't care.
  std::stable_sort(view.items.begin(), view.items.end(), [](const LiveMissable& a, const LiveMissable& b) {
    auto rank = [](const LiveMissable& m) { return m.status == 0 ? (m.here ? 0 : 1) : m.status + 1; };
    return rank(a) < rank(b);
  });
  return view;
}

void LiveMissables(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
                   const std::string& character, const std::string& area, std::vector<LiveMissable>* out,
                   std::string* alert, int* hidden) {
  MissablesView v = BuildMissablesView(hProcess, moduleBase, moduleSize, baseACellOffset, character, area, SpoilerTier());
  *out = std::move(v.items);
  *alert = v.alert;
  *hidden = v.hidden;
}

// missables [--full]: the warnings at the spoiler tier.
int RunMissables() {
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
  MissablesView v = BuildMissablesView(hProcess, moduleBase, moduleSize, profile.baseACellOffset, character, area, tier);
  static const char* kStatus[] = {"PENDING", "missed", "done", "don't care"};
  Log("Missable content (spoiler tier %s, %s):", SpoilerTierName(tier), character.c_str());
  for (const auto& m : v.items) {
    Log("  %-10s %-7s %s%s  [%s] (%s)", kStatus[m.status], m.kind.c_str(), m.title.c_str(),
        m.automatic ? " (the game shows it)" : "", m.area.c_str(), m.id.c_str());
    if (!m.text.empty() && m.status <= 1) Log("             %s", m.text.c_str());
    if (!m.lose.empty() && m.status <= 1) Log("             Lose: %s", m.lose.c_str());
  }
  if (v.hidden) Log("  (%d not shown at tier %s; F9 in game or --full)", v.hidden, SpoilerTierName(tier));
  if (!v.alert.empty()) Log("Here: %s", v.alert.c_str());
  CloseHandle(hProcess);
  return 0;
}

