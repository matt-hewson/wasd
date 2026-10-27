// src/core/memdiff.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- memdiff mode: live byte-diff of a struct, with noise learning ----
//
// Built for the active-spell (top slot) hunt: PlayerGameData itself was
// ruled out by a 12KB diff, so the remaining candidates are objects
// reached through pointers (EquipMagicData at PlayerGameData+0x470, the
// ChrIns/WorldChrMan side). Snapshots `len` bytes every 100ms and prints
// every aligned int32 that changes. For the first `learn` ticks the user
// touches nothing; anything that changes anyway (timers, animation
// state) is marked noise and suppressed for the rest of the run.
//
//   memdiff <base> [hex deref offset ...] [--len hex] [--learn n] [--iter n]
//   base: pgd (PlayerGameData) | chrins (WorldChrMan player, "s1") |
//         chrmods ("s2") | chrdata (HP/FP struct, "s3") | <hex address>
//   Each deref offset: read the pointer at current+offset, continue there.
// --summary: instead of a line per change, print one line per changed
// int32 at the end -- change count plus first/last/min/max read as a
// float -- for actions that change too much to read live (walking).
// Only cells that look like ordinary floats are listed (finite, not
// denormal, magnitude under 1e6), most-changed first.
int RunMemDiff(const std::wstring& baseName, const std::vector<uintptr_t>& derefs, SIZE_T len,
               int learnTicks, int iterations, bool summary) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  if (moduleBase == 0) {
    CloseHandle(hProcess);
    return 4;
  }

  uintptr_t addr = 0;
  if (baseName == L"pgd") {
    addr = ResolvePlayerProfile(hProcess, moduleBase, moduleSize).xBase;
  } else if (baseName == L"chrins" || baseName == L"chrmods" || baseName == L"chrdata") {
    auto resolved = ResolvePlayerHp(hProcess, moduleBase, moduleSize);
    addr = baseName == L"chrins"    ? resolved.entityAddress
           : baseName == L"chrmods" ? resolved.chrModulesAddress
                                    : resolved.structBase;
  } else {
    addr = static_cast<uintptr_t>(std::wcstoull(baseName.c_str(), nullptr, 16));
  }
  SIZE_T br = 0;
  for (uintptr_t off : derefs) {
    uintptr_t next = 0;
    if (addr == 0 || !ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr + off), &next,
                                        sizeof(next), &br)) {
      addr = 0;
      break;
    }
    addr = next;
  }
  if (addr == 0) {
    Log("FAILED to resolve the base/deref chain.");
    CloseHandle(hProcess);
    return 6;
  }
  len &= ~static_cast<SIZE_T>(3);
  Log("memdiff 0x%llx, 0x%llx bytes. Learning noise for %d ticks (~%.1fs) -- DON'T TOUCH "
      "ANYTHING yet.",
      static_cast<unsigned long long>(addr), static_cast<unsigned long long>(len), learnTicks,
      learnTicks / 10.0);

  std::vector<unsigned char> prev(len), cur(len);
  std::vector<bool> noisy(len / 4, false);
  if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), prev.data(), len, &br)) {
    Log("Initial read FAILED (error %lu).", GetLastError());
    CloseHandle(hProcess);
    return 6;
  }
  struct CellStats {
    int changes = 0;
    float first = 0.0f, last = 0.0f, lo = 0.0f, hi = 0.0f;
  };
  std::vector<CellStats> stats(summary ? len / 4 : 0);
  for (int i = 0; i < iterations; ++i) {
    Sleep(100);
    if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), cur.data(), len, &br))
      continue;
    bool learning = i < learnTicks;
    if (i == learnTicks) {
      size_t n = 0;
      for (bool b : noisy) n += b;
      Log("Learning done: %zu noisy int32(s) suppressed. NOW do the action.", n);
    }
    for (size_t w = 0; w < len / 4; ++w) {
      int32_t a = 0, b = 0;
      std::memcpy(&a, prev.data() + w * 4, 4);
      std::memcpy(&b, cur.data() + w * 4, 4);
      if (a == b) continue;
      if (learning) {
        noisy[w] = true;
      } else if (!noisy[w]) {
        float fa = 0.0f, fb = 0.0f;
        std::memcpy(&fa, &a, 4);
        std::memcpy(&fb, &b, 4);
        if (summary) {
          auto& c = stats[w];
          if (c.changes == 0) c.first = c.lo = c.hi = fa;
          ++c.changes;
          c.last = fb;
          c.lo = (std::min)(c.lo, fb);  // parenthesised: windows.h defines min/max macros
          c.hi = (std::max)(c.hi, fb);
        } else {
          Log("  +0x%04zx  %d -> %d   (0x%08X, as float %g)", w * 4, a, b,
              static_cast<uint32_t>(b), fb);
        }
      }
    }
    prev.swap(cur);
  }
  if (summary) {
    auto plausible = [](float f) {
      return std::isfinite(f) && (f == 0.0f || std::fabs(f) >= 1e-6f) && std::fabs(f) < 1e6f;
    };
    std::vector<size_t> order;
    for (size_t w = 0; w < stats.size(); ++w) {
      const auto& c = stats[w];
      if (c.changes > 0 && plausible(c.first) && plausible(c.last) && plausible(c.lo) &&
          plausible(c.hi))
        order.push_back(w);
    }
    std::sort(order.begin(), order.end(),
              [&](size_t x, size_t y) { return stats[x].changes > stats[y].changes; });
    Log("Summary: %zu float-looking cell(s) changed after learning (most-changed first):",
        order.size());
    for (size_t w : order) {
      const auto& c = stats[w];
      Log("  +0x%05zx  changes=%-4d first=%-12g last=%-12g min=%-12g max=%-12g delta=%g", w * 4,
          c.changes, c.first, c.last, c.lo, c.hi, c.last - c.first);
    }
  }
  Log("Done.");
  CloseHandle(hProcess);
  return 0;
}

// Defined further down (event flags, map data); the window worker uses them.
int CountFoundPickups(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize,
                      uintptr_t baseACellOffset, int mapArea, int* total);
// Bosses in `area` ("" if none), shard and key-item counts, for the window.
void WindowProgress(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize,
                    uintptr_t baseACellOffset, const std::string& area, std::string* bosses,
                    int* bossesDefeated, int* bossesTotal, int* bossesLater, std::string* shards);
std::string WindowKeyItems(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize,
                           const ResolvedProfile& profile);
std::string WindowUpgrade(HANDLE hProcess, uintptr_t moduleBase, const ResolvedProfile& profile,
                          int32_t inventoryItemId, const std::unordered_map<int32_t, int>& held,
                          std::vector<int32_t>* materials = nullptr);
std::unordered_map<int32_t, int> HeldGoodsQty(HANDLE hProcess, const ResolvedProfile& profile);
void WindowNearest(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
                   const PlayerPose& pose, const PlayerArea& where, std::string* windowLine,
                   std::string* overlayLine);
int CountFoundInArea(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, uintptr_t baseACellOffset,
                     const std::string& area, int* total);
std::string WindowBossHint(HANDLE hProcess, uintptr_t moduleBase, SIZE_T moduleSize, const std::string& area,
                           const PlayerPose& pose);

