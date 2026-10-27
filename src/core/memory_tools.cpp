// src/core/memory_tools.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- probe mode -----------------------------------------------------

int RunProbe(bool listModules) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;

  if (listModules) {
    auto modules = ListModuleNames(pid);
    Log("Loaded modules (%zu):", modules.size());
    for (const auto& m : modules) {
      std::printf("    %ls\n", m.c_str());
    }
  }

  uintptr_t base = GetModuleBase(pid, kTargetProcessName);
  if (base == 0) {
    Log("Could not resolve module base address.");
    CloseHandle(hProcess);
    return 4;
  }
  Log("Module base address: 0x%p", reinterpret_cast<void*>(base));

  Log("Performing 5 timed reads of the module's PE header (MZ signature)...");
  for (int i = 0; i < 5; ++i) {
    unsigned char buf[2] = {0, 0};
    SIZE_T bytesRead = 0;

    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    BOOL ok = ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(base), buf, sizeof(buf),
                                 &bytesRead);
    QueryPerformanceCounter(&end);

    double micros = (end.QuadPart - start.QuadPart) * 1000000.0 / freq.QuadPart;

    if (ok && bytesRead == sizeof(buf)) {
      Log("read #%d: bytes=0x%02X%02X (%s) latency=%.3fus", i + 1, buf[0], buf[1],
          (buf[0] == 'M' && buf[1] == 'Z') ? "OK: MZ" : "unexpected", micros);
    } else {
      Log("read #%d: FAILED (error %lu) latency=%.3fus", i + 1, GetLastError(), micros);
    }
    Sleep(100);
  }

  CloseHandle(hProcess);
  Log("Done. Handle closed.");
  return 0;
}

// ---- scan / rescan mode ----------------------------------------------

// Scans committed, read-write, private (heap) memory for 4-byte-aligned
// int32 cells equal to `value`. Read-only: uses VirtualQueryEx +
// ReadProcessMemory only, never touches the target's memory.
std::vector<uintptr_t> ScanForInt32(HANDLE hProcess, int32_t value) {
  std::vector<uintptr_t> hits;

  SYSTEM_INFO sysInfo{};
  GetSystemInfo(&sysInfo);

  auto* addr = reinterpret_cast<unsigned char*>(sysInfo.lpMinimumApplicationAddress);
  auto* maxAddr = reinterpret_cast<unsigned char*>(sysInfo.lpMaximumApplicationAddress);

  constexpr SIZE_T kChunk = 1 << 20;  // 1 MiB, multiple of 4 -> no split matches across chunks
  std::vector<unsigned char> buf(kChunk);

  while (addr < maxAddr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQueryEx(hProcess, addr, &mbi, sizeof(mbi)) == 0) break;

    bool committed = mbi.State == MEM_COMMIT;
    bool isPrivate = mbi.Type == MEM_PRIVATE;
    bool writable = (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                                     PAGE_EXECUTE_WRITECOPY)) != 0;
    bool guarded = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;

    if (committed && isPrivate && writable && !guarded) {
      auto* regionStart = reinterpret_cast<unsigned char*>(mbi.BaseAddress);
      SIZE_T remaining = mbi.RegionSize;
      SIZE_T offsetInRegion = 0;

      while (remaining > 0) {
        SIZE_T thisChunk = remaining < kChunk ? remaining : kChunk;
        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(hProcess, regionStart + offsetInRegion, buf.data(), thisChunk,
                               &bytesRead)) {
          for (SIZE_T i = 0; i + sizeof(int32_t) <= bytesRead; i += sizeof(int32_t)) {
            int32_t v;
            std::memcpy(&v, buf.data() + i, sizeof(v));
            if (v == value) {
              hits.push_back(reinterpret_cast<uintptr_t>(regionStart + offsetInRegion + i));
            }
          }
        }
        offsetInRegion += thisChunk;
        remaining -= thisChunk;
      }
    }

    addr = reinterpret_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
  }

  return hits;
}

void SaveCandidates(const std::vector<uintptr_t>& addrs) {
  EnsureDir(Paths().ScratchDir());
  std::ofstream out(CandidatesPath(), std::ios::trunc);
  for (auto a : addrs) {
    out << "0x" << std::hex << a << "\n";
  }
}

std::vector<uintptr_t> LoadCandidates() {
  std::vector<uintptr_t> addrs;
  std::ifstream in(CandidatesPath());
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    addrs.push_back(static_cast<uintptr_t>(std::strtoull(line.c_str(), nullptr, 16)));
  }
  return addrs;
}

int RunScan(int32_t value) {
  HANDLE hProcess = AttachReadOnly(nullptr);
  if (!hProcess) return 2;

  Log("Scanning committed/private/read-write memory for int32 value %d ...", value);
  LARGE_INTEGER freq, start, end;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  auto hits = ScanForInt32(hProcess, value);
  QueryPerformanceCounter(&end);
  double seconds = (end.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);

  SaveCandidates(hits);
  Log("Found %zu candidate address(es) in %.2fs. Saved to %ls", hits.size(), seconds,
      CandidatesPath().c_str());
  for (size_t i = 0; i < hits.size() && i < 20; ++i) {
    std::printf("    0x%llx\n", static_cast<unsigned long long>(hits[i]));
  }
  if (hits.size() > 20) std::printf("    ... and %zu more\n", hits.size() - 20);

  CloseHandle(hProcess);
  if (hits.empty()) {
    Log("No matches. Double check the HP value you passed matches the HUD exactly.");
    return 5;
  }
  if (hits.size() > 1) {
    Log("More than one candidate -- change your HP in-game (take a hit / heal) and run "
        "'rescan <newValue>' to narrow it down.");
  } else {
    Log("Exactly one candidate -- you can 'watch 0x%llx' now to verify it live against the HUD.",
        static_cast<unsigned long long>(hits[0]));
  }
  return 0;
}

int RunRescan(int32_t value) {
  auto candidates = LoadCandidates();
  if (candidates.empty()) {
    Log("No candidates file (or it's empty) at %ls. Run 'scan <value>' first.", CandidatesPath().c_str());
    return 6;
  }
  Log("Loaded %zu candidate(s) from %ls", candidates.size(), CandidatesPath().c_str());

  HANDLE hProcess = AttachReadOnly(nullptr);
  if (!hProcess) return 2;

  std::vector<uintptr_t> survivors;
  for (auto addr : candidates) {
    int32_t v = 0;
    SIZE_T bytesRead = 0;
    if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), &v, sizeof(v), &bytesRead) &&
        bytesRead == sizeof(v) && v == value) {
      survivors.push_back(addr);
    }
  }
  CloseHandle(hProcess);

  SaveCandidates(survivors);
  Log("%zu of %zu candidate(s) still equal %d. Saved to %s", survivors.size(), candidates.size(),
      value, CandidatesPath().c_str());
  for (size_t i = 0; i < survivors.size() && i < 20; ++i) {
    std::printf("    0x%llx\n", static_cast<unsigned long long>(survivors[i]));
  }

  if (survivors.empty()) {
    Log("No survivors -- the new value didn't match any candidate. Re-run 'scan <value>' fresh "
        "with the HP value currently on your HUD.");
    return 5;
  }
  if (survivors.size() > 1) {
    Log("Still more than one -- change HP again and rescan.");
  } else {
    Log("Narrowed to one address -- 'watch 0x%llx' to verify it live against the HUD.",
        static_cast<unsigned long long>(survivors[0]));
  }
  return 0;
}

// ---- fscan / frescan mode (float32 variant, for equip load / item
// discovery / poise -- values with no source lead in any reference
// project checked, so the only way in is the same value-scan technique
// that originally found HP, just for float32 instead of int32) --------

std::wstring FCandidatesPath() { return Paths().Scratch(L"fcandidates.txt"); }
constexpr float kDefaultFloatTolerance = 0.05f;

// Same scan as ScanForInt32, but for float32 cells within `tolerance`
// of `value` (floats displayed with limited decimal precision on the
// HUD rarely match the underlying binary value bit-for-bit, so an
// exact-equality scan would miss real hits). Read-only throughout.
std::vector<uintptr_t> ScanForFloat32(HANDLE hProcess, float value, float tolerance) {
  std::vector<uintptr_t> hits;

  SYSTEM_INFO sysInfo{};
  GetSystemInfo(&sysInfo);

  auto* addr = reinterpret_cast<unsigned char*>(sysInfo.lpMinimumApplicationAddress);
  auto* maxAddr = reinterpret_cast<unsigned char*>(sysInfo.lpMaximumApplicationAddress);

  constexpr SIZE_T kChunk = 1 << 20;
  std::vector<unsigned char> buf(kChunk);

  while (addr < maxAddr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQueryEx(hProcess, addr, &mbi, sizeof(mbi)) == 0) break;

    bool committed = mbi.State == MEM_COMMIT;
    bool isPrivate = mbi.Type == MEM_PRIVATE;
    bool writable = (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                                     PAGE_EXECUTE_WRITECOPY)) != 0;
    bool guarded = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;

    if (committed && isPrivate && writable && !guarded) {
      auto* regionStart = reinterpret_cast<unsigned char*>(mbi.BaseAddress);
      SIZE_T remaining = mbi.RegionSize;
      SIZE_T offsetInRegion = 0;

      while (remaining > 0) {
        SIZE_T thisChunk = remaining < kChunk ? remaining : kChunk;
        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(hProcess, regionStart + offsetInRegion, buf.data(), thisChunk,
                               &bytesRead)) {
          for (SIZE_T i = 0; i + sizeof(float) <= bytesRead; i += sizeof(float)) {
            float v;
            std::memcpy(&v, buf.data() + i, sizeof(v));
            if (std::fabs(v - value) <= tolerance) {
              hits.push_back(reinterpret_cast<uintptr_t>(regionStart + offsetInRegion + i));
            }
          }
        }
        offsetInRegion += thisChunk;
        remaining -= thisChunk;
      }
    }

    addr = reinterpret_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
  }

  return hits;
}

int RunFScan(float value, float tolerance) {
  HANDLE hProcess = AttachReadOnly(nullptr);
  if (!hProcess) return 2;

  Log("Scanning committed/private/read-write memory for float32 value %.4f +/- %.4f ...", value,
      tolerance);
  LARGE_INTEGER freq, start, end;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  auto hits = ScanForFloat32(hProcess, value, tolerance);
  QueryPerformanceCounter(&end);
  double seconds = (end.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);

  EnsureDir(Paths().ScratchDir());
  std::ofstream out(FCandidatesPath(), std::ios::trunc);
  for (auto a : hits) out << "0x" << std::hex << a << "\n";

  Log("Found %zu candidate address(es) in %.2fs. Saved to %s", hits.size(), seconds,
      FCandidatesPath().c_str());
  for (size_t i = 0; i < hits.size() && i < 20; ++i) {
    std::printf("    0x%llx\n", static_cast<unsigned long long>(hits[i]));
  }
  if (hits.size() > 20) std::printf("    ... and %zu more\n", hits.size() - 20);

  CloseHandle(hProcess);
  if (hits.empty()) {
    Log("No matches. Double check the value you passed matches the status screen exactly, or "
        "widen the tolerance.");
    return 5;
  }
  if (hits.size() > 1) {
    Log("More than one candidate -- change the value in-game and run 'frescan <newValue>' to "
        "narrow it down.");
  } else {
    Log("Exactly one candidate -- 'watch 0x%llx --float' to verify it live.",
        static_cast<unsigned long long>(hits[0]));
  }
  return 0;
}

int RunFRescan(float value, float tolerance) {
  std::vector<uintptr_t> candidates;
  {
    std::ifstream in(FCandidatesPath());
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      candidates.push_back(static_cast<uintptr_t>(std::strtoull(line.c_str(), nullptr, 16)));
    }
  }
  if (candidates.empty()) {
    Log("No candidates file (or it's empty) at %ls. Run 'fscan <value>' first.", FCandidatesPath().c_str());
    return 6;
  }
  Log("Loaded %zu candidate(s) from %ls", candidates.size(), FCandidatesPath().c_str());

  HANDLE hProcess = AttachReadOnly(nullptr);
  if (!hProcess) return 2;

  std::vector<uintptr_t> survivors;
  for (auto addr : candidates) {
    float v = 0.0f;
    SIZE_T bytesRead = 0;
    if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), &v, sizeof(v), &bytesRead) &&
        bytesRead == sizeof(v) && std::fabs(v - value) <= tolerance) {
      survivors.push_back(addr);
    }
  }
  CloseHandle(hProcess);

  EnsureDir(Paths().ScratchDir());
  std::ofstream out(FCandidatesPath(), std::ios::trunc);
  for (auto a : survivors) out << "0x" << std::hex << a << "\n";

  Log("%zu of %zu candidate(s) still match %.4f +/- %.4f. Saved to %s", survivors.size(),
      candidates.size(), value, tolerance, FCandidatesPath().c_str());
  for (size_t i = 0; i < survivors.size() && i < 20; ++i) {
    std::printf("    0x%llx\n", static_cast<unsigned long long>(survivors[i]));
  }

  if (survivors.empty()) {
    Log("No survivors -- re-run 'fscan <value>' fresh with the value currently on your status "
        "screen.");
    return 5;
  }
  if (survivors.size() > 1) {
    Log("Still more than one -- change the value again and frescan.");
  } else {
    Log("Narrowed to one address -- 'watch 0x%llx --float' to verify it live.",
        static_cast<unsigned long long>(survivors[0]));
  }
  return 0;
}

int RunWatch(uintptr_t address, int iterations, bool asFloat) {
  HANDLE hProcess = AttachReadOnly(nullptr);
  if (!hProcess) return 2;

  Log("Watching 0x%llx for %d reads as %s (Ctrl+C to stop early)...",
      static_cast<unsigned long long>(address), iterations, asFloat ? "float32" : "int32");

  for (int i = 0; i < iterations; ++i) {
    unsigned char raw[4] = {};
    SIZE_T bytesRead = 0;

    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    BOOL ok = ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(address), raw, sizeof(raw),
                                 &bytesRead);
    QueryPerformanceCounter(&end);
    double micros = (end.QuadPart - start.QuadPart) * 1000000.0 / freq.QuadPart;

    if (ok && bytesRead == sizeof(raw)) {
      if (asFloat) {
        float v = 0.0f;
        std::memcpy(&v, raw, sizeof(v));
        Log("value=%.4f latency=%.3fus", v, micros);
      } else {
        int32_t v = 0;
        std::memcpy(&v, raw, sizeof(v));
        Log("value=%d latency=%.3fus", v, micros);
      }
    } else {
      Log("read FAILED (error %lu) latency=%.3fus", GetLastError(), micros);
    }
    Sleep(100);
  }

  CloseHandle(hProcess);
  Log("Done. Handle closed.");
  return 0;
}

// ---- findptr / resolve mode ------------------------------------------

struct PtrHit {
  uintptr_t cellAddr;    // address of the pointer cell itself (P)
  uintptr_t cellValue;   // value stored there (V, a pointer)
  uintptr_t structOffset;  // target - V; how far past V the target sits
};

std::wstring PtrHitsPath() { return Paths().Scratch(L"ptrhits.txt"); }

// Scans all committed, readable, non-guarded memory for 8-byte-aligned
// pointer cells whose value lands at-or-just-before `target` (within
// maxBackOffset bytes). Read-only: VirtualQueryEx + ReadProcessMemory
// only.
std::vector<PtrHit> ScanForPointersNear(HANDLE hProcess, uintptr_t target,
                                         uintptr_t maxBackOffset) {
  std::vector<PtrHit> hits;

  SYSTEM_INFO sysInfo{};
  GetSystemInfo(&sysInfo);
  auto* addr = reinterpret_cast<unsigned char*>(sysInfo.lpMinimumApplicationAddress);
  auto* maxAddr = reinterpret_cast<unsigned char*>(sysInfo.lpMaximumApplicationAddress);

  constexpr SIZE_T kChunk = 1 << 20;
  std::vector<unsigned char> buf(kChunk);

  while (addr < maxAddr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQueryEx(hProcess, addr, &mbi, sizeof(mbi)) == 0) break;

    bool committed = mbi.State == MEM_COMMIT;
    bool readable = (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                                     PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                                     PAGE_EXECUTE_WRITECOPY)) != 0;
    bool guarded = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;

    if (committed && readable && !guarded) {
      auto* regionStart = reinterpret_cast<unsigned char*>(mbi.BaseAddress);
      SIZE_T remaining = mbi.RegionSize;
      SIZE_T offsetInRegion = 0;

      while (remaining > 0) {
        SIZE_T thisChunk = remaining < kChunk ? remaining : kChunk;
        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(hProcess, regionStart + offsetInRegion, buf.data(), thisChunk,
                               &bytesRead)) {
          for (SIZE_T i = 0; i + sizeof(uint64_t) <= bytesRead; i += sizeof(uint64_t)) {
            uint64_t raw;
            std::memcpy(&raw, buf.data() + i, sizeof(raw));
            uintptr_t val = static_cast<uintptr_t>(raw);
            if (val <= target && (target - val) <= maxBackOffset) {
              hits.push_back({reinterpret_cast<uintptr_t>(regionStart + offsetInRegion + i), val,
                               target - val});
            }
          }
        }
        offsetInRegion += thisChunk;
        remaining -= thisChunk;
      }
    }

    addr = reinterpret_cast<unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
  }

  return hits;
}

void SavePtrHits(const std::vector<PtrHit>& hits) {
  EnsureDir(Paths().ScratchDir());
  std::ofstream out(PtrHitsPath(), std::ios::trunc);
  for (const auto& h : hits) {
    out << "0x" << std::hex << h.cellAddr << " 0x" << h.cellValue << " 0x" << h.structOffset
        << "\n";
  }
}

int RunFindPtr(uintptr_t target, uintptr_t maxBackOffset) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;

  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  if (moduleBase == 0) {
    Log("Could not resolve module base/size.");
    CloseHandle(hProcess);
    return 4;
  }
  Log("Module range: 0x%llx - 0x%llx (size 0x%zx)", static_cast<unsigned long long>(moduleBase),
      static_cast<unsigned long long>(moduleBase + moduleSize), moduleSize);

  Log("Scanning for pointers within 0x%llx bytes before 0x%llx ...",
      static_cast<unsigned long long>(maxBackOffset), static_cast<unsigned long long>(target));
  LARGE_INTEGER freq, start, end;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  auto hits = ScanForPointersNear(hProcess, target, maxBackOffset);
  QueryPerformanceCounter(&end);
  double seconds = (end.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);

  // STATIC hits (inside the module) first, then heap hits. Within each
  // group, smallest struct offset first (more likely a real struct base).
  std::sort(hits.begin(), hits.end(), [&](const PtrHit& a, const PtrHit& b) {
    bool aStatic = a.cellAddr >= moduleBase && a.cellAddr < moduleBase + moduleSize;
    bool bStatic = b.cellAddr >= moduleBase && b.cellAddr < moduleBase + moduleSize;
    if (aStatic != bStatic) return aStatic > bStatic;
    return a.structOffset < b.structOffset;
  });

  SavePtrHits(hits);
  Log("Found %zu pointer hit(s) in %.2fs. Saved to %ls", hits.size(), seconds, PtrHitsPath().c_str());

  size_t staticCount = 0;
  for (size_t i = 0; i < hits.size() && i < 30; ++i) {
    const auto& h = hits[i];
    bool isStatic = h.cellAddr >= moduleBase && h.cellAddr < moduleBase + moduleSize;
    if (isStatic) {
      ++staticCount;
      std::printf(
          "    STATIC cell=0x%llx (DarkSoulsIII.exe+0x%llx) -> resolve 0x%llx 0x%llx\n",
          static_cast<unsigned long long>(h.cellAddr),
          static_cast<unsigned long long>(h.cellAddr - moduleBase),
          static_cast<unsigned long long>(h.cellAddr - moduleBase),
          static_cast<unsigned long long>(h.structOffset));
    } else {
      std::printf("    heap   cell=0x%llx value=0x%llx offset=0x%llx\n",
                  static_cast<unsigned long long>(h.cellAddr),
                  static_cast<unsigned long long>(h.cellValue),
                  static_cast<unsigned long long>(h.structOffset));
    }
  }
  if (hits.size() > 30) std::printf("    ... and %zu more (see %ls)\n", hits.size() - 30,
                                     PtrHitsPath().c_str());

  CloseHandle(hProcess);

  if (staticCount > 0) {
    Log("%zu STATIC hit(s) found -- try the suggested `resolve` command above.", staticCount);
  } else if (!hits.empty()) {
    Log("No STATIC hits -- all candidates are heap addresses. Pick one 'cell=' address above "
        "and run findptr on IT to go one level deeper (chain gets one hop longer).");
  } else {
    Log("No hits at all. Try a larger max back-offset (default 0x800), or re-derive the "
        "target address with scan/rescan -- it may be stale.");
  }
  return 0;
}

int RunResolve(const std::vector<uintptr_t>& offsets) {
  if (offsets.empty()) {
    std::fwprintf(stderr, L"resolve needs at least one offset\n");
    return 64;
  }

  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;

  uintptr_t moduleBase = GetModuleBase(pid, kTargetProcessName);
  if (moduleBase == 0) {
    Log("Could not resolve module base address.");
    CloseHandle(hProcess);
    return 4;
  }
  Log("Module base: 0x%llx", static_cast<unsigned long long>(moduleBase));

  for (int iter = 0; iter < 30; ++iter) {
    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    uintptr_t addr = moduleBase + offsets[0];
    bool ok = true;
    std::string chainDesc = "moduleBase+0x" + [&] {
      char b[32];
      std::snprintf(b, sizeof(b), "%llx", static_cast<unsigned long long>(offsets[0]));
      return std::string(b);
    }();

    for (size_t i = 1; i < offsets.size() && ok; ++i) {
      uintptr_t ptr = 0;
      SIZE_T bytesRead = 0;
      ok = ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), &ptr, sizeof(ptr),
                              &bytesRead) &&
           bytesRead == sizeof(ptr);
      if (!ok) {
        Log("chain broken: failed to dereference 0x%llx (hop %zu)",
            static_cast<unsigned long long>(addr), i);
        break;
      }
      addr = ptr + offsets[i];
    }

    QueryPerformanceCounter(&end);
    double micros = (end.QuadPart - start.QuadPart) * 1000000.0 / freq.QuadPart;

    if (!ok) {
      Sleep(100);
      continue;
    }

    int32_t v = 0;
    SIZE_T bytesRead = 0;
    BOOL readOk = ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), &v, sizeof(v),
                                     &bytesRead);
    if (readOk && bytesRead == sizeof(v)) {
      Log("value=%d (final addr 0x%llx) latency=%.3fus", v,
          static_cast<unsigned long long>(addr), micros);
    } else {
      Log("final read FAILED at 0x%llx (error %lu) latency=%.3fus",
          static_cast<unsigned long long>(addr), GetLastError(), micros);
    }
    Sleep(100);
  }

  CloseHandle(hProcess);
  Log("Done. Handle closed.");
  return 0;
}

