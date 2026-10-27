// src/ds3/live_stats.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- live mode (Milestone 2) -----------------------------------------
//
// `stats --live` polls until closed instead of for a fixed count, with a
// global pause/resume hotkey (F10). Pausing is the brief's "zero reads,
// not just a hidden window": the single polling thread blocks in
// GetMessage until the hotkey (or Ctrl+C) arrives, so no
// ReadProcessMemory call can happen and it uses no CPU while paused.
//
// Why a low-level keyboard hook rather than RegisterHotKey: the first
// version used RegisterHotKey (Ctrl+Alt+G) and live-testing 2026-10-06
// showed it fires with the game unfocused but NEVER with DS3 focused --
// consistent with the game registering raw input with RIDEV_NOHOTKEYS,
// which suppresses every system hotkey while it's in the foreground. A
// WH_KEYBOARD_LL hook sees keys before that. It runs on its own thread
// that sleeps in GetMessage between key events, so it adds no polling
// and no measurable input lag; every key except the hotkey is passed
// straight through, and the hotkey itself is consumed so the game never
// sees it. F10 was picked by the user as unbound in their DS3 setup
// (Ctrl, the first choice's modifier, is a game control).
constexpr DWORD kLiveHotkeyVk = VK_F10;
constexpr UINT kMsgTogglePause = WM_APP + 1;  // hook thread -> poll thread
constexpr double kPollIntervalMs = 100.0;      // 10Hz, the brief's starting rate
constexpr double kPerfReportIntervalSeconds = 5.0;

// F11: show/hide the overlay's perf figures (brief Phase 2: "on-screen
// perf HUD toggle, separate from the main hotkey"). Only hooked in
// overlay mode -- elsewhere F11 passes through untouched.
constexpr DWORD kPerfHudHotkeyVk = VK_F11;
constexpr UINT kMsgTogglePerfHud = WM_APP + 3;

// F9: cycle the spoiler tier. Window/overlay modes only.
constexpr DWORD kSpoilerTierHotkeyVk = VK_F9;
constexpr UINT kMsgCycleSpoilerTier = WM_APP + 4;
bool g_spoilerHotkeyHeld = false;

DWORD g_pollThreadId = 0;
HWND g_hotkeyTargetWindow = nullptr;  // window/overlay modes: hotkeys are posted here instead
bool g_perfHudHotkeyEnabled = false;  // overlay mode only
volatile LONG g_stopRequested = 0;
bool g_hotkeyHeld = false;         // hook thread only: swallow auto-repeat
bool g_perfHudHotkeyHeld = false;  // same, for F11

LRESULT CALLBACK LiveKeyboardHook(int code, WPARAM wParam, LPARAM lParam) {
  if (code == HC_ACTION) {
    const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    if (key->vkCode == kLiveHotkeyVk) {
      bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;  // F10 arrives as SYSKEY
      if (down && !g_hotkeyHeld) {
        // A window's own queue survives modal loops (dragging, resizing)
        // that drop thread messages, so prefer it when there is one.
        if (g_hotkeyTargetWindow) {
          PostMessageW(g_hotkeyTargetWindow, kMsgTogglePause, 0, 0);
        } else {
          PostThreadMessageW(g_pollThreadId, kMsgTogglePause, 0, 0);
        }
      }
      g_hotkeyHeld = down;
      return 1;  // consumed: the game never sees the hotkey
    }
    if (key->vkCode == kSpoilerTierHotkeyVk && g_hotkeyTargetWindow) {
      bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
      if (down && !g_spoilerHotkeyHeld) PostMessageW(g_hotkeyTargetWindow, kMsgCycleSpoilerTier, 0, 0);
      g_spoilerHotkeyHeld = down;
      return 1;
    }
    if (key->vkCode == kPerfHudHotkeyVk && g_perfHudHotkeyEnabled && g_hotkeyTargetWindow) {
      bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
      if (down && !g_perfHudHotkeyHeld) {
        PostMessageW(g_hotkeyTargetWindow, kMsgTogglePerfHud, 0, 0);
      }
      g_perfHudHotkeyHeld = down;
      return 1;
    }
  }
  return CallNextHookEx(nullptr, code, wParam, lParam);
}

struct HotkeyThreadState {
  HANDLE ready = nullptr;
  bool hooked = false;
  DWORD error = 0;
};

DWORD WINAPI HotkeyThreadMain(LPVOID param) {
  auto* state = static_cast<HotkeyThreadState*>(param);
  HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, LiveKeyboardHook, GetModuleHandleW(nullptr), 0);
  state->hooked = hook != nullptr;
  state->error = hook ? 0 : GetLastError();
  SetEvent(state->ready);
  if (!hook) return 1;
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
  }  // hook callbacks run inside GetMessage; WM_QUIT ends the thread
  UnhookWindowsHookEx(hook);
  return 0;
}

// Ctrl+C / console close: ask the poll loop to stop cleanly (it prints
// a final summary) and wake it if it's blocked in a paused GetMessage.
BOOL WINAPI OnConsoleCtrl(DWORD ctrlType) {
  if (ctrlType != CTRL_C_EVENT && ctrlType != CTRL_BREAK_EVENT && ctrlType != CTRL_CLOSE_EVENT)
    return FALSE;
  InterlockedExchange(&g_stopRequested, 1);
  PostThreadMessageW(g_pollThreadId, WM_QUIT, 0, 0);
  return TRUE;
}

// The brief's "see our own performance metrics": measured poll rate,
// read latency, and this process's own CPU and memory, over a window.
struct PerfWindow {
  LARGE_INTEGER freq{}, start{};
  ULONGLONG cpuStart = 0;
  int ticks = 0;
  double sumMicros = 0.0, maxMicros = 0.0;

  static ULONGLONG ProcessCpu100ns() {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    auto toU64 = [](const FILETIME& ft) {
      return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    return toU64(kernel) + toU64(user);
  }
  void Reset() {
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    cpuStart = ProcessCpu100ns();
    ticks = 0;
    sumMicros = maxMicros = 0.0;
  }
  void AddTick(double micros) {
    ++ticks;
    sumMicros += micros;
    if (micros > maxMicros) maxMicros = micros;
  }
  double ElapsedSeconds() const {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - start.QuadPart) / freq.QuadPart;
  }
  struct Sample {
    bool valid = false;
    int ticks = 0;
    double seconds = 0.0, rateHz = 0.0, avgMicros = 0.0, maxMicros = 0.0;
    double cpuPercentOfCore = 0.0, workingSetMB = 0.0, privateMB = 0.0;
  };
  Sample Take() const {
    Sample out;
    double elapsed = ElapsedSeconds();
    if (elapsed <= 0.0 || ticks == 0) return out;
    double cpuSeconds = static_cast<double>(ProcessCpu100ns() - cpuStart) / 1e7;
    PROCESS_MEMORY_COUNTERS_EX mem{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem),
                         sizeof(mem));
    out.valid = true;
    out.ticks = ticks;
    out.seconds = elapsed;
    out.rateHz = ticks / elapsed;
    out.avgMicros = sumMicros / ticks;
    out.maxMicros = maxMicros;
    out.cpuPercentOfCore = cpuSeconds / elapsed * 100.0;
    out.workingSetMB = mem.WorkingSetSize / 1048576.0;
    out.privateMB = mem.PrivateUsage / 1048576.0;
    return out;
  }
  void Report() const {
    Sample s = Take();
    if (!s.valid) return;
    Log("perf: poll rate %.2f Hz (%d ticks / %.2fs), read latency avg %.1fus max %.1fus, "
        "CPU %.2f%% of one core, working set %.1f MB, private %.1f MB",
        s.rateHz, s.ticks, s.seconds, s.avgMicros, s.maxMicros, s.cpuPercentOfCore,
        s.workingSetMB, s.privateMB);
  }
};

// One perf.csv row (kPerfCsvHeader) from a finished perf window.
std::string PerfCsvRow(const PerfWindow::Sample& s, double sessionSeconds) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  char row[256];
  std::snprintf(row, sizeof(row), "%04u-%02u-%02uT%02u:%02u:%02u,%.1f,%.2f,%d,%.2f,%.1f,%.1f,%.3f,%.1f,%.1f",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, sessionSeconds, s.seconds,
                s.ticks, s.rateHz, s.avgMicros, s.maxMicros, s.cpuPercentOfCore, s.workingSetMB, s.privateMB);
  return row;
}

int RunStats(int iterations, bool live) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;

  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  if (moduleBase == 0) {
    Log("Could not resolve module base/size.");
    CloseHandle(hProcess);
    return 4;
  }

  Log("Resolving player HP/FP/Stamina pointer chain (WorldChrMan AOB scan, re-derived fresh)...");
  auto resolved = ResolvePlayerHp(hProcess, moduleBase, moduleSize);
  if (resolved.structBase == 0) {
    Log("FAILED to resolve the stat struct -- a pattern scan or a dereference along the chain "
        "came up empty. See docs/TECHNICAL.md, or re-derive with `wcm`.");
    CloseHandle(hProcess);
    return 6;
  }
  Log("Resolved struct base 0x%llx -- HP+0x%llx FP+0x%llx Stamina+0x%llx",
      static_cast<unsigned long long>(resolved.structBase),
      static_cast<unsigned long long>(kHpFieldOffset),
      static_cast<unsigned long long>(kFpFieldOffset),
      static_cast<unsigned long long>(kStaminaFieldOffset));

  Log("Resolving level/souls pointer chain (BaseA AOB scan, re-derived fresh)...");
  auto profile = ResolvePlayerProfile(hProcess, moduleBase, moduleSize);
  bool haveProfile = profile.characterStatsBase != 0;
  if (haveProfile) {
    Log("Resolved CharacterStats base 0x%llx -- Level+0x%llx Souls+0x%llx",
        static_cast<unsigned long long>(profile.characterStatsBase),
        static_cast<unsigned long long>(kLevelOffset - kCharacterStatsOffset),
        static_cast<unsigned long long>(kSoulsOffset - kCharacterStatsOffset));
  } else {
    Log("FAILED to resolve level/souls -- BaseA pattern scan or a dereference came up empty. "
        "Continuing with HP/FP/Stamina only.");
  }

  // Current equip load is computed from item weights now (see
  // ComputeCurrentEquipLoad); the old display-stats pointer
  // (ResolveDisplayStats) read garbage after a game relaunch. Item
  // Discovery came from that same pointer and has no replacement yet.
  Log("Item Discovery: not available -- its pointer anchor doesn't survive a game relaunch "
      "(see docs/TECHNICAL.md).");

  // Attributes and base-max only change on level-up. Rather than being
  // read once and going stale, they're wrapped in lambdas so the
  // polling loop below can re-invoke them the moment Level changes
  // mid-run, not just at startup.
  auto readAndPrintAttributes = [&]() {
    int32_t vig = 0, att = 0, end = 0, str = 0, dex = 0, intel = 0, fth = 0, lck = 0, vit = 0;
    SIZE_T brA = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.vigorAddress), &vig,
                       sizeof(vig), &brA);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.attunementAddress), &att,
                       sizeof(att), &brA);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.enduranceAddress), &end,
                       sizeof(end), &brA);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.strengthAddress), &str,
                       sizeof(str), &brA);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.dexterityAddress), &dex,
                       sizeof(dex), &brA);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.intelligenceAddress), &intel,
                       sizeof(intel), &brA);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.faithAddress), &fth,
                       sizeof(fth), &brA);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.luckAddress), &lck, sizeof(lck),
                       &brA);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.vitalityAddress), &vit,
                       sizeof(vit), &brA);
    Log("Attributes: Vig=%d Att=%d End=%d Str=%d Dex=%d Int=%d Fth=%d Lck=%d Vit=%d "
        "-- cross-check against your character status screen.",
        vig, att, end, str, dex, intel, fth, lck, vit);
  };

  auto readAndPrintBaseMax = [&]() {
    int32_t baseMaxHp = 0, baseMaxFp = 0, baseMaxStamina = 0;
    SIZE_T br0 = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.baseMaxHpAddress), &baseMaxHp,
                       sizeof(baseMaxHp), &br0);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.baseMaxFpAddress), &baseMaxFp,
                       sizeof(baseMaxFp), &br0);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.baseMaxStaminaAddress),
                       &baseMaxStamina, sizeof(baseMaxStamina), &br0);
    Log("Base max (no buffs): HP=%d FP=%d Stamina=%d -- cross-check against your character "
        "status screen.",
        baseMaxHp, baseMaxFp, baseMaxStamina);
  };

  // Equipped items (weapons/armor/rings), read via the disassembly-
  // verified chain (see docs/TECHNICAL.md). Unlike attributes/base-max above,
  // these ARE re-checked every tick below (cheap -- 14 extra int32
  // reads against a 100ms interval), since a gear swap can happen at
  // any time, not just on level-up. lastSlotIds is the baseline the
  // loop diffs against.
  uintptr_t equipInventoryData = 0;
  std::vector<SlotInfo> slots;
  ItemNameTable nameTable;
  std::vector<int32_t> lastSlotIds;
  std::vector<int32_t> lastSpellIds(14, 0);  // baseline for per-tick spell-slot diffing

  // Reads the player's current Attunement + equipped rings and returns
  // the live active-slot count (base from Attunement + ring bonuses --
  // see ComputeBaseAttunementSlots/ComputeRingAttunementSlotBonus
  // above). Small, self-contained ring-collection loop rather than
  // sharing state with readAndPrintEquipLoadAndPoise below, matching
  // this file's existing pattern of cheap, independent live re-reads.
  auto currentActiveSpellSlotCount = [&]() -> int32_t {
    int32_t attunement = 0;
    SIZE_T brA = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.attunementAddress), &attunement,
                       sizeof(attunement), &brA);
    std::vector<int32_t> ringGiveIds;
    for (const auto& slot : slots) {
      std::string sName = slot.name;
      if (sName != "Ring1" && sName != "Ring2" && sName != "Ring3" && sName != "Ring4") continue;
      int32_t id = -1;
      SIZE_T brN = 0;
      ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(slot.address), &id, sizeof(id), &brN);
      if (id < 0) continue;
      auto item = ResolveItem(hProcess, equipInventoryData, id);
      if (item.ok && item.plausible) ringGiveIds.push_back(item.giveId);
    }
    return ComputeBaseAttunementSlots(attunement) +
           ComputeRingAttunementSlotBonus(hProcess, moduleBase, ringGiveIds);
  };

  // Prints every currently-equipped spell (only the slots the character
  // actually has unlocked -- see currentActiveSpellSlotCount above) and
  // resets lastSpellIds to match, so the per-tick diff loop below has a
  // clean baseline. Live-verified exact match (2026-08-19): all 3 of
  // the user's equipped spells (Fireball/Fire Orb/Great Combustion)
  // read correctly and were confirmed by the user -- see
  // ReadSpellSlot's comment above for the pointer-dereference bug this
  // caught along the way.
  auto printEquippedSpells = [&]() {
    int32_t activeSlots = currentActiveSpellSlotCount();
    Log("Equipped spells (%d active attunement slot%s, live-verified exact match 2026-08-19 -- "
        "see docs/TECHNICAL.md):",
        activeSlots, activeSlots == 1 ? "" : "s");
    for (int32_t i = 1; i <= 14; ++i) {
      int32_t magicId = ReadSpellSlot(hProcess, profile.xBase, i);
      lastSpellIds[i - 1] = magicId;
      if (i > activeSlots) continue;
      if (magicId <= 0) {
        Log("  Slot %d: (empty)", i);
      } else {
        std::string name = LookupSpellName(nameTable, magicId);
        Log("  Slot %d: magicParamId=%d %s", i, magicId,
            name.empty() ? "(name not found)" : name.c_str());
      }
    }
  };

  // ---- active R/L hand + bottom (quick item) slot -----------------
  //
  // See kRightHandSlotOffset/kLeftHandSlotOffset/
  // kSelectedQuickItemSlotOffset above for how these were found/
  // verified. Top slot (active spell) is a known, documented gap --
  // ruled out of PlayerGameData entirely, not just unfound, see the
  // comment above kSelectedQuickItemSlotOffset.
  int32_t lastRightHandSlot = -999, lastLeftHandSlot = -999, lastQuickItemSlot = -999;
  int32_t lastActiveSpellIndex = -999;
  auto resolveActiveSpellName = [&](int32_t idx) -> std::string {
    if (idx < 0 || idx >= 14) return "(no spell slot)";
    int32_t magicId = ReadSpellSlot(hProcess, profile.xBase, idx + 1);
    std::string name = magicId > 0 ? LookupSpellName(nameTable, magicId) : "";
    return "slot " + std::to_string(idx + 1) + " " +
           (magicId <= 0 ? "(empty)" : (name.empty() ? "(name not found)" : name));
  };

  // Maps a hand-slot index (0-2) to its SlotInfo in `slots` and
  // resolves the item there the same way as everywhere else in this
  // file.
  auto resolveHandSlotName = [&](bool isRight, int32_t slotIdx) -> std::string {
    if (slotIdx < 0 || slotIdx > 2) return "(unknown slot index)";
    std::string wantedName = (isRight ? std::string("R") : std::string("L")) +
                              std::to_string(slotIdx + 1);
    for (const auto& slot : slots) {
      if (wantedName != slot.name) continue;
      int32_t id = -1;
      SIZE_T br = 0;
      ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(slot.address), &id, sizeof(id), &br);
      if (id < 0) return wantedName + " (empty)";
      auto item = ResolveItem(hProcess, equipInventoryData, id);
      if (!item.ok || !item.plausible) return wantedName + " (unresolved)";
      std::string name = LookupItemName(nameTable, item.uniqueId, item.giveId);
      return wantedName + " " + (name.empty() ? "(name not found)" : name);
    }
    return "(slot not found)";
  };

  auto resolveQuickItemName = [&](int32_t idx) -> std::string {
    if (idx < 0 || idx > 9) return "(unknown slot index)";
    int32_t id = -1;
    SIZE_T br = 0;
    ReadProcessMemory(hProcess,
                       reinterpret_cast<LPCVOID>(profile.xBase + kQuickItemArrayOffset +
                                                  static_cast<uintptr_t>(idx) * kQuickItemStride),
                       &id, sizeof(id), &br);
    if (id <= 0) return "(empty)";
    auto item = ResolveItem(hProcess, equipInventoryData, id);
    if (!item.ok || !item.plausible) return "(unresolved)";
    std::string name = LookupItemName(nameTable, item.uniqueId, item.giveId);
    return name.empty() ? "(name not found)" : name;
  };

  auto printActiveSlots = [&]() {
    int32_t rHand = 0, lHand = 0, qItem = 0;
    SIZE_T br = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.xBase + kRightHandSlotOffset),
                       &rHand, sizeof(rHand), &br);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.xBase + kLeftHandSlotOffset),
                       &lHand, sizeof(lHand), &br);
    ReadProcessMemory(hProcess,
                       reinterpret_cast<LPCVOID>(profile.xBase + kSelectedQuickItemSlotOffset),
                       &qItem, sizeof(qItem), &br);
    lastRightHandSlot = rHand;
    lastLeftHandSlot = lHand;
    lastQuickItemSlot = qItem;
    Log("Active right hand: %s -- live-verified correct 2026-08-19", resolveHandSlotName(true, rHand).c_str());
    Log("Active left hand: %s -- live-verified correct 2026-08-19", resolveHandSlotName(false, lHand).c_str());
    Log("Active bottom slot (quick item #%d): %s -- live-verified correct 2026-08-19, see "
        "docs/TECHNICAL.md",
        qItem + 1, resolveQuickItemName(qItem).c_str());
    lastActiveSpellIndex = ReadActiveSpellIndex(hProcess, profile.xBase);
    Log("Active top slot (spell): %s", resolveActiveSpellName(lastActiveSpellIndex).c_str());
  };

  // Physical Attack Rating for the 6 weapon slots -- see
  // ComputeWeaponPhysicalAr above for the full derivation/caveats.
  // Printed alongside a slot's item description, both at startup and
  // whenever that slot changes. Reads Str/Dex fresh every call (cheap)
  // so a level-up between gear checks isn't stale.
  auto isWeaponSlot = [](const char* slotName) {
    return std::string(slotName) == "R1" || std::string(slotName) == "R2" ||
           std::string(slotName) == "R3" || std::string(slotName) == "L1" ||
           std::string(slotName) == "L2" || std::string(slotName) == "L3";
  };
  auto printWeaponArIfApplicable = [&](const char* slotName, int32_t inventoryItemId) {
    if (!isWeaponSlot(slotName) || inventoryItemId < 0) return;
    auto item = ResolveItem(hProcess, equipInventoryData, inventoryItemId);
    if (!item.ok || !item.plausible) return;
    int32_t str = 0, dex = 0, intel = 0, faith = 0, luck = 0;
    SIZE_T brS = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.strengthAddress), &str,
                       sizeof(str), &brS);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.dexterityAddress), &dex,
                       sizeof(dex), &brS);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.intelligenceAddress), &intel,
                       sizeof(intel), &brS);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.faithAddress), &faith,
                       sizeof(faith), &brS);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.luckAddress), &luck,
                       sizeof(luck), &brS);
    auto ar = ComputeWeaponAr(hProcess, moduleBase, item.giveId, str, dex, intel, faith, luck);
    if (!ar.ok) return;

    // Live two-handing state -- see kWeaponSheathStateOffset above.
    // Only the currently-DRAWN slot on a given side can be two-handed
    // (RightHandSlot/LeftHandSlot pick which of that side's 3 weapon
    // slots is active); a slot that isn't the active one on its side is
    // always shown one-handed, since it isn't the weapon actually being
    // gripped right now regardless of the global sheath state.
    int32_t sheathState = 0, rightHandSlot = 0, leftHandSlot = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.xBase + kWeaponSheathStateOffset),
                       &sheathState, sizeof(sheathState), &brS);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.xBase + kRightHandSlotOffset),
                       &rightHandSlot, sizeof(rightHandSlot), &brS);
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.xBase + kLeftHandSlotOffset),
                       &leftHandSlot, sizeof(leftHandSlot), &brS);
    std::string slot = slotName;
    bool isRightSlot = slot == "R1" || slot == "R2" || slot == "R3";
    bool isLeftSlot = slot == "L1" || slot == "L2" || slot == "L3";
    int32_t slotIndex = slot.empty() ? -1 : (slot[1] - '1');  // R1/L1->0, R2/L2->1, R3/L3->2
    bool isActiveSlot = (isRightSlot && rightHandSlot == slotIndex) ||
                         (isLeftSlot && leftHandSlot == slotIndex);
    bool isTwoHanded = isActiveSlot && ((isRightSlot && sheathState == 3) ||
                                         (isLeftSlot && sheathState == 2));

    std::string line = "    AR (computed, Str=" + std::to_string(str) +
                        " Dex=" + std::to_string(dex) + " Int=" + std::to_string(intel) +
                        " Fth=" + std::to_string(faith) + (isTwoHanded ? ", two-handed" : "") +
                        "):";
    line += FormatWeaponAr(ar, isTwoHanded);
    Log("%s -- see docs/TECHNICAL.md \"Weapon Attack Rating\" for what's live-verified.", line.c_str());
  };

  if (haveProfile) {
    equipInventoryData = profile.xBase + kEquipGameDataOffset + kEquipInventoryDataOffset;
    slots = BuildEquipSlotList(profile);
    nameTable = LoadItemNameTable(FindItemNameTablePath());

    readAndPrintAttributes();

    Log("Equipped items -- re-resolved automatically below whenever a slot changes:%s",
        nameTable.loaded ? "" : " (no item names yet -- run  wasd-cli.exe extract;  showing raw IDs only)");
    lastSlotIds.assign(slots.size(), -1);
    for (size_t i = 0; i < slots.size(); ++i) {
      int32_t id = -1;
      SIZE_T br = 0;
      ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(slots[i].address), &id, sizeof(id),
                         &br);
      lastSlotIds[i] = id;
      Log("  %-6s %s", slots[i].name,
          FormatItemDescription(hProcess, equipInventoryData, nameTable, id).c_str());
      printWeaponArIfApplicable(slots[i].name, id);
    }
    printEquippedSpells();
    printActiveSlots();
  }

  readAndPrintBaseMax();

  // Max equip load + armor poise both depend on the currently equipped
  // items, so both are wrapped in a lambda the same way
  // readAndPrintAttributes/readAndPrintBaseMax are -- re-invoked below
  // any time an equip slot actually changes, not just at startup.
  // Max equip load also depends on Vitality (re-read here every call,
  // cheap) so a Vitality level-up refreshes it too even without a gear
  // change.
  double currentEquipLoad = 0.0;  // computed; refreshed on gear change / level-up
  bool haveCurrentEquipLoad = false;
  auto readAndPrintEquipLoadAndPoise = [&]() {
    int32_t vitality = 0;
    SIZE_T brV = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.vitalityAddress), &vitality,
                       sizeof(vitality), &brV);
    double baseMaxLoad = ComputeBaseMaxEquipLoad(vitality);

    std::vector<std::string> equippedNames;
    std::vector<int32_t> armorGiveIds;
    std::vector<int32_t> ringGiveIds;
    std::vector<EquippedWeightItem> weightItems;
    for (const auto& slot : slots) {
      int32_t id = -1;
      SIZE_T brN = 0;
      ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(slot.address), &id, sizeof(id), &brN);
      if (id < 0) continue;
      auto item = ResolveItem(hProcess, equipInventoryData, id);
      if (item.ok && item.plausible) {
        std::string name = LookupItemName(nameTable, item.uniqueId, item.giveId);
        std::string slotName = slot.name;
        std::string label = slotName + " " + (name.empty() ? std::to_string(item.giveId) : name);
        if (slotName == "Head" || slotName == "Chest" || slotName == "Hands" ||
            slotName == "Legs") {
          armorGiveIds.push_back(item.giveId);
          weightItems.push_back({EquipWeightTable::kProtector, item.giveId, label});
        } else if (slotName == "Ring1" || slotName == "Ring2" || slotName == "Ring3" ||
                   slotName == "Ring4") {
          ringGiveIds.push_back(item.giveId);
          weightItems.push_back({EquipWeightTable::kAccessory, item.giveId, label});
        } else if (slotName == "Covenant") {
          weightItems.push_back({EquipWeightTable::kAccessory, item.giveId, label});
        } else {  // R1-3, L1-3, Arrow/Bolt slots
          weightItems.push_back({EquipWeightTable::kWeapon, item.giveId, label});
        }
        if (!name.empty()) equippedNames.push_back(std::move(name));
      }
    }
    // Ring bonus (Havel's Ring/Ring of Favor): computed live via
    // SpEffectParam, same mechanism as ring poise bonus below -- see
    // ComputeRingEquipLoadRateMultiplier above (UNVERIFIED against an
    // actual load-boosting ring, none on hand this session).
    double ringLoadMultiplier = ComputeRingEquipLoadRateMultiplier(hProcess, moduleBase, ringGiveIds);
    double finalMaxLoad = baseMaxLoad * ringLoadMultiplier;
    auto load = ComputeCurrentEquipLoad(hProcess, moduleBase, weightItems);
    haveCurrentEquipLoad = load.ok;
    if (load.ok) {
      currentEquipLoad = load.total;
      Log("Current equip load (computed from item weights): %.1f / %.1f = %.1f%% -- %s",
          load.total, finalMaxLoad, finalMaxLoad > 0 ? load.total / finalMaxLoad * 100.0 : 0.0,
          load.breakdown.c_str());
    }
    std::string ringWarning = EquipLoadRingBonusWarning(equippedNames);
    if (ringLoadMultiplier != 1.0) {
      Log("Max equip load: %.1f (base %.1f from Vitality=%d x ring multiplier %.3f -- ring "
          "formula UNVERIFIED, no load ring tested yet, see docs/TECHNICAL.md)",
          finalMaxLoad, baseMaxLoad, vitality, ringLoadMultiplier);
    } else if (!ringWarning.empty()) {
      Log("Max equip load (computed from Vitality=%d, BASE ONLY): %.1f -- %s", vitality,
          baseMaxLoad, ringWarning.c_str());
    } else {
      Log("Max equip load (computed from Vitality=%d, formula from Fextralife's Equipment_Load "
          "page): %.1f",
          vitality, baseMaxLoad);
    }

    // Attunement slots: computed from Attunement via the documented
    // breakpoint table (see ComputeBaseAttunementSlots above), plus a
    // live SpEffectParam read for ring bonuses (see
    // ComputeRingAttunementSlotBonus above -- generic, not name-based;
    // works for any attunement-slot ring, not just the one on hand).
    // Live-verified (2026-08-19): Attunement=14 -> base 2, +1 from
    // Saint's Ring (the ring actually granting the bonus, corrected
    // live after an initial wrong guess at Sage Ring) = 3, exactly
    // matching the user's own stated breakdown.
    int32_t attunement = 0;
    SIZE_T brAtt = 0;
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.attunementAddress), &attunement,
                       sizeof(attunement), &brAtt);
    int32_t baseSlots = ComputeBaseAttunementSlots(attunement);
    int32_t ringSlots = ComputeRingAttunementSlotBonus(hProcess, moduleBase, ringGiveIds);
    Log("Attunement Slots (computed from Attunement=%d via Fextralife's breakpoint table, ring "
        "bonus read live via SpEffectParam): %d (base %d + %d from rings)",
        attunement, baseSlots + ringSlots, baseSlots, ringSlots);

    // Armor poise: read live from EquipParamProtector, not scanned or
    // hardcoded per-item (see ComputeArmorPoise above and docs/TECHNICAL.md).
    // Ring poise bonuses (e.g. Wolf Ring) layer on top via SpEffectParam
    // -- see ComputeRingPoiseRateMultiplier above for the offset
    // evidence and the still-UNVERIFIED-against-an-actual-poise-ring
    // caveat.
    auto poiseResult = ComputeArmorPoise(hProcess, moduleBase, armorGiveIds);
    if (poiseResult.ok) {
      double ringMultiplier = ComputeRingPoiseRateMultiplier(hProcess, moduleBase, ringGiveIds);
      if (ringMultiplier != 1.0) {
        double finalPoise = poiseResult.total * ringMultiplier;
        Log("Total Poise: %.2f (armor %.2f x ring multiplier %.3f -- ring formula UNVERIFIED, "
            "no poise ring tested yet, see docs/TECHNICAL.md)",
            finalPoise, poiseResult.total, ringMultiplier);
      } else {
        Log("Total Poise (armor only, computed live from EquipParamProtector, diminishing-returns "
            "formula): %.2f -- cross-check against a poise calculator, vanilla UI doesn't show "
            "this number. No ring poise bonus detected (or none equipped) -- see docs/TECHNICAL.md.",
            poiseResult.total);
      }
    } else {
      Log("Armor poise NOT computed (EquipParamProtector table resolution failed).");
    }

    // Defense/Absorption (all 8 damage types): see ComputeArmorDefense
    // above -- same table, same live-verified diminishing-returns
    // combine as poise, applied to a different field cluster. Formula
    // itself is UNVERIFIED against the live status screen (first use
    // of these 8 fields), flagged in the log line below.
    auto defenseResult = ComputeArmorDefense(hProcess, moduleBase, armorGiveIds);
    if (defenseResult.ok) {
      Log("Absorption %% (armor only, computed live from EquipParamProtector, same "
          "diminishing-returns formula as Poise -- live-verified exact match to 3 decimal "
          "places against the in-game Stats screen, 2026-08-19, see docs/TECHNICAL.md): Phys=%.3f "
          "Strike=%.3f Slash=%.3f Thrust=%.3f Magic=%.3f Fire=%.3f Lightning=%.3f Dark=%.3f",
          defenseResult.phys, defenseResult.strike, defenseResult.slash, defenseResult.thrust,
          defenseResult.magic, defenseResult.fire, defenseResult.lightning, defenseResult.dark);
    } else {
      Log("Armor defense/absorption NOT computed (EquipParamProtector table resolution failed).");
    }

    // Status resistances (Bleed/Poison/Frost/Curse, +Toxic): see
    // ComputeArmorResistance above -- plain point sum, NOT the
    // diminishing-returns formula used for Poise/Absorption. UNVERIFIED
    // against the live status screen, flagged in the log line below.
    auto resistResult = ComputeArmorResistance(hProcess, moduleBase, armorGiveIds);
    if (resistResult.ok) {
      Log("Resistance (armor only, computed live from EquipParamProtector, plain sum -- "
          "Bleed/Poison/Frost/Curse live-verified correct against the in-game screen "
          "2026-08-19; Toxic not shown in vanilla UI, unverified, see docs/TECHNICAL.md): Bleed=%.0f "
          "Poison=%.0f Toxic=%.0f Frost=%.0f Curse=%.0f",
          resistResult.bleed, resistResult.poison, resistResult.toxic, resistResult.frost,
          resistResult.curse);
    } else {
      Log("Armor resistance NOT computed (EquipParamProtector table resolution failed).");
    }
  };

  if (haveProfile) {
    readAndPrintEquipLoadAndPoise();
  }

  HANDLE hotkeyThread = nullptr;
  DWORD hotkeyThreadId = 0;
  if (live) {
    g_pollThreadId = GetCurrentThreadId();
    MSG queueInit;  // make sure this thread has a queue before the hook can post to it
    PeekMessageW(&queueInit, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    HotkeyThreadState hotkeyState;
    hotkeyState.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    hotkeyThread = CreateThread(nullptr, 0, HotkeyThreadMain, &hotkeyState, 0, &hotkeyThreadId);
    if (hotkeyThread) WaitForSingleObject(hotkeyState.ready, INFINITE);
    CloseHandle(hotkeyState.ready);
    if (!hotkeyThread || !hotkeyState.hooked) {
      Log("WARNING: couldn't install the F10 keyboard hook (error %lu). Running without "
          "pause/resume; Ctrl+C still stops cleanly.",
          hotkeyThread ? hotkeyState.error : GetLastError());
    }
    Log("LIVE: polling at 10Hz until closed. F10 pauses/resumes, even with the game focused "
        "(zero reads while paused; the key is consumed, the game never sees it). Ctrl+C "
        "stops. The status line prints only when a value changes; a perf line prints every "
        "%.0fs.",
        kPerfReportIntervalSeconds);
  } else {
    Log("Polling HP/FP/Stamina (current/max) at 10Hz for %d reads (Ctrl+C to stop early)...",
        iterations);
  }

  int32_t lastLevel = 0;
  bool haveLastLevel = false;
  // Cached rather than recomputed every tick -- currentActiveSpellSlotCount
  // resolves rings + walks their SpEffect chains, noticeably heavier than
  // the other per-tick reads. Only recomputed when something that could
  // actually change it happens: a level-up (Attunement) or a gear change
  // (a ring swap). Initialized from printEquippedSpells's own call below.
  int32_t cachedActiveSpellSlots = haveProfile ? currentActiveSpellSlotCount() : 0;

  double sumMicros = 0.0, maxMicros = 0.0;
  int okCount = 0, tickCount = 0;
  std::string lastStatus;  // live mode: status line printed only when this changes
  bool playerUnloaded = false;  // quit to menu / loading: see ReadCurrentPlayerIns
  const uintptr_t gameManCellOffset = ResolveGameManCellOffset(hProcess, moduleBase, moduleSize);
  const AreaNames areaNames;
  AreaTracker areaTracker;
  const AreaInfo areaInfo;
  SessionTracker tracker;
  tracker.log.enabled = live;  // only live runs are saved
  SessionStats& session = tracker.stats;
  LARGE_INTEGER lastSessionTick{};
  bool loggedCounters = false;
  PerfWindow perf;
  perf.Reset();
  LARGE_INTEGER qpcFreq, nextTick;
  QueryPerformanceFrequency(&qpcFreq);
  QueryPerformanceCounter(&nextTick);
  const LONGLONG tickInterval =
      static_cast<LONGLONG>(qpcFreq.QuadPart * kPollIntervalMs / 1000.0);
  double pausedSeconds = 0.0;

  for (int i = 0; live || i < iterations; ++i) {
    if (live) {
      // Hotkey / quit handling. Everything here is message-queue work,
      // not a memory read.
      bool togglePause = false;
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == kMsgTogglePause) togglePause = true;
        if (msg.message == WM_QUIT) InterlockedExchange(&g_stopRequested, 1);
      }
      if (g_stopRequested) break;
      if (togglePause) {
        perf.Report();
        if (auto ps = perf.Take(); ps.valid) tracker.log.WritePerf(PerfCsvRow(ps, tracker.stats.seconds));
        Log("PAUSED (F10) -- polling stopped, zero reads until resumed.");
        LARGE_INTEGER pauseStart, pauseEnd;
        QueryPerformanceCounter(&pauseStart);
        bool resumed = false;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {  // blocks: no CPU, no reads
          if (msg.message == kMsgTogglePause) {
            resumed = true;
            break;
          }
        }
        QueryPerformanceCounter(&pauseEnd);
        double pausedFor =
            static_cast<double>(pauseEnd.QuadPart - pauseStart.QuadPart) / qpcFreq.QuadPart;
        pausedSeconds += pausedFor;
        if (!resumed) break;  // WM_QUIT (Ctrl+C) while paused
        Log("RESUMED after %.1fs paused.", pausedFor);
        perf.Reset();
        lastStatus.clear();  // reprint the current state straight away
        nextTick = pauseEnd;
      }
      // Game closed? A process-handle query, not a memory read.
      DWORD exitCode = 0;
      if (GetExitCodeProcess(hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
        Log("Game process exited (code %lu) -- stopping.", exitCode);
        break;
      }
    }
    ++tickCount;

    // Player unloaded or rebuilt (quit to menu, reload)? See
    // ReadCurrentPlayerIns. While unloaded, no player reads happen.
    {
      uintptr_t playerIns =
          ReadCurrentPlayerIns(hProcess, moduleBase, resolved.worldChrManCellOffset);
      if (playerIns != resolved.entityAddress) {
        bool reloaded = false;
        if (playerIns != 0) {
          auto fresh = ResolvePlayerHp(hProcess, moduleBase, moduleSize);
          if (fresh.structBase != 0 &&
              ReadCurrentPlayerIns(hProcess, moduleBase, fresh.worldChrManCellOffset) ==
                  fresh.entityAddress) {
            resolved = fresh;
            profile = ResolvePlayerProfile(hProcess, moduleBase, moduleSize);
            haveProfile = profile.characterStatsBase != 0;
            if (haveProfile) {
              equipInventoryData =
                  profile.xBase + kEquipGameDataOffset + kEquipInventoryDataOffset;
              slots = BuildEquipSlotList(profile);
              lastSlotIds.assign(slots.size(), INT32_MIN);  // every slot re-reports below
            }
            lastStatus.clear();
            playerUnloaded = false;
            reloaded = true;
            Log("Player loaded -- all addresses re-resolved.");
          }
        } else if (!playerUnloaded) {
          playerUnloaded = true;
          Log("Player unloaded (main menu or loading) -- reads paused until a character is "
              "loaded.");
        }
        if (!reloaded) {
          // Still unloaded or mid-load: wait a tick (re-resolving runs AOB
          // scans, so retry at most once a second) without reading.
          DWORD waitMs = playerIns != 0 ? 1000 : static_cast<DWORD>(kPollIntervalMs);
          if (live) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, waitMs, QS_ALLPOSTMESSAGE);
          } else {
            Sleep(waitMs);
          }
          QueryPerformanceCounter(&nextTick);
          continue;
        }
      }
    }

    int32_t hp = 0, maxHp = 0, fp = 0, maxFp = 0, stamina = 0, maxStamina = 0;
    int32_t level = 0, souls = 0;
    SIZE_T br = 0;

    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    bool ok =
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.hpAddress), &hp,
                           sizeof(hp), &br) &&
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.maxHpAddress), &maxHp,
                           sizeof(maxHp), &br) &&
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.fpAddress), &fp,
                           sizeof(fp), &br) &&
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.maxFpAddress), &maxFp,
                           sizeof(maxFp), &br) &&
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.staminaAddress), &stamina,
                           sizeof(stamina), &br) &&
        ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(resolved.maxStaminaAddress),
                           &maxStamina, sizeof(maxStamina), &br);
    if (haveProfile) {
      ok = ok &&
           ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.levelAddress), &level,
                              sizeof(level), &br) &&
           ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.soulsAddress), &souls,
                              sizeof(souls), &br);
    }
    QueryPerformanceCounter(&end);
    double micros = (end.QuadPart - start.QuadPart) * 1000000.0 / freq.QuadPart;
    sumMicros += micros;
    if (micros > maxMicros) maxMicros = micros;
    perf.AddTick(micros);

    if (ok) {
      ++okCount;
      if (haveProfile) {
        // Level-up detection: re-resolve attributes/base-max the
        // moment they'd actually be stale, not just at startup. Not
        // counted in the latency figure below -- that's meant to
        // reflect steady-state polling cost, and a level-up is a rare
        // event, not the common case being measured.
        if (!haveLastLevel) {
          lastLevel = level;
          haveLastLevel = true;
        } else if (level != lastLevel) {
          Log("Level changed %d -> %d -- refreshing attributes and base-max...", lastLevel,
              level);
          readAndPrintAttributes();
          readAndPrintBaseMax();
          readAndPrintEquipLoadAndPoise();  // max equip load depends on Vitality
          cachedActiveSpellSlots = currentActiveSpellSlotCount();  // Attunement may have changed
          // AR depends on Str/Dex/Int/Fth, so every weapon's figure is
          // stale after a level-up even though no slot changed.
          for (size_t si = 0; si < slots.size(); ++si) {
            if (!isWeaponSlot(slots[si].name) || lastSlotIds[si] < 0) continue;
            Log("  %s AR after level-up:", slots[si].name);
            printWeaponArIfApplicable(slots[si].name, lastSlotIds[si]);
          }
          lastLevel = level;
        }

        // Gear-change detection, all 14 slots (weapons/armor/rings),
        // every tick -- a swap can happen at any time, unlike
        // attributes which only change on level-up. Any change also
        // triggers a max-equip-load/armor-poise recompute (once per
        // tick, even if multiple slots changed at once), since both
        // depend on currently equipped items and would otherwise go
        // stale the moment gear is swapped mid-run.
        bool anySlotChanged = false;
        for (size_t si = 0; si < slots.size(); ++si) {
          int32_t curId = -1;
          SIZE_T brS = 0;
          ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(slots[si].address), &curId,
                             sizeof(curId), &brS);
          if (curId != lastSlotIds[si]) {
            Log("%s slot changed: %s", slots[si].name,
                FormatItemDescription(hProcess, equipInventoryData, nameTable, curId).c_str());
            printWeaponArIfApplicable(slots[si].name, curId);
            lastSlotIds[si] = curId;
            anySlotChanged = true;
          }
        }
        if (anySlotChanged) {
          readAndPrintEquipLoadAndPoise();
          cachedActiveSpellSlots = currentActiveSpellSlotCount();  // a ring swap may have changed it
        }

        // Spell-slot change detection -- spells can be re-attuned at a
        // bonfire without touching gear at all, so this needs its own
        // trigger, not just piggybacking on anySlotChanged above. Uses
        // the cached active-slot count (refreshed above on level-up/
        // gear-change only, not recomputed every tick -- resolving
        // rings + walking SpEffect chains every 100ms would be needless
        // overhead for a count that only ever changes on those two
        // events). Only the currently-active slots are diffed/printed;
        // slots beyond the live count are silently re-baselined so a
        // later increase in Attunement Slots doesn't spuriously report
        // stale content as a "change".
        for (int32_t spellSlot = 1; spellSlot <= 14; ++spellSlot) {
          int32_t magicId = ReadSpellSlot(hProcess, profile.xBase, spellSlot);
          if (magicId == lastSpellIds[spellSlot - 1]) continue;
          if (spellSlot <= cachedActiveSpellSlots) {
            std::string name = LookupSpellName(nameTable, magicId);
            Log("Spell slot %d changed: %s", spellSlot,
                magicId <= 0 ? "(empty)" : (name.empty() ? "(name not found)" : name.c_str()));
          }
          lastSpellIds[spellSlot - 1] = magicId;
        }

        // Active R/L hand + bottom (quick-item) slot change detection --
        // these can change independently of both gear swaps (switching
        // which already-equipped weapon is drawn) and level-ups, so
        // this is its own trigger too. Cheap: 3 int32 reads per tick.
        {
          int32_t rHand = 0, lHand = 0, qItem = 0;
          SIZE_T brActive = 0;
          ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.xBase +
                                                                  kRightHandSlotOffset),
                             &rHand, sizeof(rHand), &brActive);
          ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(profile.xBase +
                                                                  kLeftHandSlotOffset),
                             &lHand, sizeof(lHand), &brActive);
          ReadProcessMemory(
              hProcess,
              reinterpret_cast<LPCVOID>(profile.xBase + kSelectedQuickItemSlotOffset), &qItem,
              sizeof(qItem), &brActive);
          if (rHand != lastRightHandSlot) {
            Log("Active right hand changed: %s", resolveHandSlotName(true, rHand).c_str());
            lastRightHandSlot = rHand;
          }
          if (lHand != lastLeftHandSlot) {
            Log("Active left hand changed: %s", resolveHandSlotName(false, lHand).c_str());
            lastLeftHandSlot = lHand;
          }
          int32_t spellIdx = ReadActiveSpellIndex(hProcess, profile.xBase);
          if (spellIdx != lastActiveSpellIndex) {
            Log("Active top slot changed (spell): %s", resolveActiveSpellName(spellIdx).c_str());
            lastActiveSpellIndex = spellIdx;
          }
          if (qItem != lastQuickItemSlot) {
            Log("Active bottom slot changed (quick item #%d): %s", qItem + 1,
                resolveQuickItemName(qItem).c_str());
            lastQuickItemSlot = qItem;
          }
        }

        // Session stats.
        std::string currentArea;
        {
          PlayerArea area = ReadPlayerArea(hProcess, moduleBase, resolved.entityAddress,
                                           gameManCellOffset);
          areaTracker.Update(area);
          currentArea = areaInfo.AreaOf(areaTracker.lastRegion);
          GameCounters counters = ReadGameCounters(hProcess, moduleBase, profile.baseACellOffset);
          if (counters.ok && !loggedCounters) {
            loggedCounters = true;
            Log("Game counters (UNVERIFIED offsets, see docs/TECHNICAL.md): DeathNum=%d TrueDeathNum=%d "
                "PlayTime=%u", counters.deathNum, counters.trueDeathNum, counters.playTime);
          }
          LARGE_INTEGER nowTick;
          QueryPerformanceCounter(&nowTick);
          double dt = lastSessionTick.QuadPart == 0
                          ? 0.0
                          : (std::min)(1.0, static_cast<double>(nowTick.QuadPart -
                                                                 lastSessionTick.QuadPart) /
                                                freq.QuadPart);
          lastSessionTick = nowTick;
          SessionContext ctx;
          ctx.level = level;
          ctx.souls = souls;
          ctx.deathNum = counters.deathNum;
          ctx.playTimeMs = counters.playTime;
          ctx.area = currentArea;
          ctx.characterName = ReadCharacterName(hProcess, profile.xBase);
          ctx.pose = ReadPlayerPose(hProcess, resolved);
          ctx.lastBonfireId = area.lastBonfireId;
          ctx.lastBonfireName = areaNames.Bonfire(area.lastBonfireId);
          if (counters.ok && tracker.Tick(dt, ctx)) {
            Log("%s (DeathNum=%d TrueDeathNum=%d)", session.lastEvent.c_str(), counters.deathNum,
                counters.trueDeathNum);
          }
        }
        int32_t reqSouls = RequiredSoulsForLevel(level + 1);
        char line[256];
        std::snprintf(line, sizeof(line), "HP=%d/%d FP=%d/%d Stamina=%d/%d", hp, maxHp, fp, maxFp,
                      stamina, maxStamina);
        std::string msg = line;
        if (haveCurrentEquipLoad) {
          std::snprintf(line, sizeof(line), " EquipLoad=%.1f", currentEquipLoad);
          msg += line;
        }
        if (auto pose = ReadPlayerPose(hProcess, resolved); pose.ok) {
          std::snprintf(line, sizeof(line), " Pos=(%.2f, %.2f, %.2f) Facing=%.0fdeg", pose.x,
                        pose.y, pose.z, pose.FacingDegrees());
          msg += line;
        }
        if (auto area = ReadPlayerArea(hProcess, moduleBase, resolved.entityAddress,
                                       gameManCellOffset);
            area.ok) {
          areaTracker.Update(area);
          std::snprintf(line, sizeof(line), " Area=\"%s\" (%s, region %d) LastBonfire=\"%s\"",
                        areaTracker.lastRegion ? areaNames.Region(areaTracker.lastRegion).c_str()
                                               : "?",
                        area.MapName().c_str(), area.playRegionId,
                        areaNames.Bonfire(area.lastBonfireId).c_str());
          msg += line;
        }
        if (session.started) {
          msg += " Deaths=" + std::to_string(session.deaths) + "/" +
                 std::to_string(session.lastDeathNum) + "total";
        }
        std::snprintf(line, sizeof(line), " Level=%d Souls=%d (next lvl %d costs %d, computed)",
                      level, souls, level + 1, reqSouls);
        msg += line;
        if (!live || msg != lastStatus) Log("%s latency=%.3fus", msg.c_str(), micros);
        lastStatus = msg;
      } else {
        char line[256];
        std::snprintf(line, sizeof(line), "HP=%d/%d FP=%d/%d Stamina=%d/%d", hp, maxHp, fp, maxFp,
                      stamina, maxStamina);
        std::string msg = line;
        if (haveCurrentEquipLoad) {
          std::snprintf(line, sizeof(line), " EquipLoad=%.1f", currentEquipLoad);
          msg += line;
        }
        if (auto pose = ReadPlayerPose(hProcess, resolved); pose.ok) {
          std::snprintf(line, sizeof(line), " Pos=(%.2f, %.2f, %.2f) Facing=%.0fdeg", pose.x,
                        pose.y, pose.z, pose.FacingDegrees());
          msg += line;
        }
        if (auto area = ReadPlayerArea(hProcess, moduleBase, resolved.entityAddress,
                                       gameManCellOffset);
            area.ok) {
          areaTracker.Update(area);
          std::snprintf(line, sizeof(line), " Area=\"%s\" (%s, region %d) LastBonfire=\"%s\"",
                        areaTracker.lastRegion ? areaNames.Region(areaTracker.lastRegion).c_str()
                                               : "?",
                        area.MapName().c_str(), area.playRegionId,
                        areaNames.Bonfire(area.lastBonfireId).c_str());
          msg += line;
        }
        if (!live || msg != lastStatus) Log("%s latency=%.3fus", msg.c_str(), micros);
        lastStatus = msg;
      }
    } else {
      Log("read FAILED (error %lu) latency=%.3fus", GetLastError(), micros);
    }

    if (live && perf.ElapsedSeconds() >= kPerfReportIntervalSeconds) {
      perf.Report();
      if (auto ps = perf.Take(); ps.valid) tracker.log.WritePerf(PerfCsvRow(ps, tracker.stats.seconds));
      perf.Reset();
    }

    // Fixed-deadline schedule: each tick is due exactly 100ms after the
    // previous DEADLINE, not 100ms after the work finished, so work time
    // and Sleep's ~15.6ms timer granularity jitter individual ticks but
    // don't drag the average rate below 10Hz. If we fall more than one
    // tick behind (machine stalled), resync rather than burst-catch-up.
    nextTick.QuadPart += tickInterval;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (now.QuadPart - nextTick.QuadPart > tickInterval) nextTick = now;
    LONGLONG waitTicks = nextTick.QuadPart - now.QuadPart;
    DWORD waitMs = waitTicks > 0 ? static_cast<DWORD>(waitTicks * 1000 / qpcFreq.QuadPart) : 0;
    if (live) {
      // Wakes early for a hotkey/quit message so pausing feels instant.
      MsgWaitForMultipleObjects(0, nullptr, FALSE, waitMs, QS_ALLPOSTMESSAGE);
    } else {
      Sleep(waitMs);
    }
  }

  if (hotkeyThread) {
    PostThreadMessageW(hotkeyThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(hotkeyThread, 2000);
    CloseHandle(hotkeyThread);
  }
  CloseHandle(hProcess);
  if (tickCount == 0) tickCount = 1;
  tracker.Finish();
  if (session.started) {
    Log("Session: %s polling, %d death(s), souls gained %lld (%.0f/h), lost %lld, recovered %lld.",
        FormatDuration(session.seconds).c_str(), session.deaths,
        static_cast<long long>(session.gained), session.SoulsPerHour(),
        static_cast<long long>(session.lost), static_cast<long long>(session.recovered));
    for (const auto& [areaName, secs] : session.areaSeconds) {
      Log("  %s in %s", FormatDuration(secs).c_str(), areaName.c_str());
    }
  }
  Log("Done. %d/%d reads ok, avg latency=%.3fus, max=%.3fus%s.", okCount, tickCount,
      sumMicros / tickCount, maxMicros,
      live ? (", " + std::to_string(static_cast<int>(pausedSeconds)) + "s spent paused").c_str()
           : "");
  return 0;
}

