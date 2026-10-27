// src/core/process.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- log files (WASD.exe) ---------------------------
// One file per day, logs\wasd-YYYYMMDD.log in the user data folder; files
// more than kLogKeepDays old are deleted when the app starts.
constexpr int kLogKeepDays = 7;

std::wstring LogFileName(int year, int month, int day) {
  wchar_t buf[32];
  std::swprintf(buf, 32, L"wasd-%04d%02d%02d.log", year, month, day);
  return buf;
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
long DaysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Which of these file names are WASD logs older than keepDays before the
// given day. Anything not named wasd-YYYYMMDD.log is left alone.
std::vector<std::wstring> OldLogs(const std::vector<std::wstring>& names, int year, int month, int day, int keepDays) {
  std::vector<std::wstring> out;
  long today = DaysFromCivil(year, month, day);
  for (const auto& n : names) {
    // "wasd-" + 8 digits + ".log"
    if (n.size() != 17 || n.compare(0, 5, L"wasd-") != 0 || n.compare(13, 4, L".log") != 0) continue;
    if (!std::all_of(n.begin() + 5, n.begin() + 13, [](wchar_t c) { return c >= L'0' && c <= L'9'; })) continue;
    auto num = [&](size_t at, size_t len) { return std::stoi(n.substr(at, len)); };
    int y = num(5, 4), m = num(9, 2), d = num(11, 2);
    if (m < 1 || m > 12 || d < 1 || d > 31) continue;
    if (today - DaysFromCivil(y, m, d) > keepDays) out.push_back(n);
  }
  return out;
}

void OpenLogFile() {
  std::wstring dir = Paths().Logs();
  if (!EnsureDir(dir)) return;
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::vector<std::wstring> names;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\wasd-*.log").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do names.push_back(fd.cFileName);
    while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  for (const auto& old : OldLogs(names, st.wYear, st.wMonth, st.wDay, kLogKeepDays)) DeleteFileW((dir + L"\\" + old).c_str());
  g_logFile = _wfsopen((dir + L"\\" + LogFileName(st.wYear, st.wMonth, st.wDay)).c_str(), L"a", _SH_DENYWR);
}

void AdoptNewMapData();  // defined with the map-data tables

DWORD FindProcessIdByName(const wchar_t* name) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  DWORD pid = 0;
  if (Process32FirstW(snap, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, name) == 0) {
        pid = entry.th32ProcessID;
        break;
      }
    } while (Process32NextW(snap, &entry));
  }
  CloseHandle(snap);
  return pid;
}

std::vector<std::wstring> ListModuleNames(DWORD pid) {
  std::vector<std::wstring> result;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE) return result;

  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Module32FirstW(snap, &entry)) {
    do {
      result.push_back(entry.szModule);
    } while (Module32NextW(snap, &entry));
  }
  CloseHandle(snap);
  return result;
}

bool HasModule(const std::vector<std::wstring>& modules, const wchar_t* name) {
  for (const auto& m : modules) {
    if (_wcsicmp(m.c_str(), name) == 0) return true;
  }
  return false;
}

uintptr_t GetModuleBase(DWORD pid, const wchar_t* moduleName) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE) return 0;

  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  uintptr_t base = 0;
  if (Module32FirstW(snap, &entry)) {
    do {
      if (_wcsicmp(entry.szModule, moduleName) == 0) {
        base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
        break;
      }
    } while (Module32NextW(snap, &entry));
  }
  CloseHandle(snap);
  return base;
}

// Returns {base, size} of moduleName within pid's address space, or
// {0, 0} if not found.
std::pair<uintptr_t, SIZE_T> GetModuleBaseAndSize(DWORD pid, const wchar_t* moduleName) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE) return {0, 0};

  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  std::pair<uintptr_t, SIZE_T> result{0, 0};
  if (Module32FirstW(snap, &entry)) {
    do {
      if (_wcsicmp(entry.szModule, moduleName) == 0) {
        result = {reinterpret_cast<uintptr_t>(entry.modBaseAddr), entry.modBaseSize};
        break;
      }
    } while (Module32NextW(snap, &entry));
  }
  CloseHandle(snap);
  return result;
}

// Finds the process, verifies the Seamless Co-op marker module, and
// opens a read-only handle (PROCESS_VM_READ | PROCESS_QUERY_INFORMATION
// only). Returns nullptr and logs why on any failure -- callers must
// not proceed to read anything if this returns nullptr.
HANDLE AttachReadOnly(DWORD* outPid) {
  DWORD pid = FindProcessIdByName(kTargetProcessName);
  if (pid == 0) {
    Log("Process '%ls' not found. Launch DS3 via Seamless Co-op and retry.", kTargetProcessName);
    return nullptr;
  }
  Log("Found %ls (PID %lu)", kTargetProcessName, pid);

  auto modules = ListModuleNames(pid);
  if (!HasModule(modules, kSeamlessCoopModuleMarker)) {
    Log("SAFETY ABORT: '%ls' not found in the target process's module list. Refusing to "
        "attach -- this may be vanilla online DS3.",
        kSeamlessCoopModuleMarker);
    return nullptr;
  }
  Log("Verified '%ls' is loaded -- safe to attach.", kSeamlessCoopModuleMarker);

  HANDLE hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
  if (!hProcess) {
    Log("OpenProcess failed (error %lu).", GetLastError());
    return nullptr;
  }
  Log("Opened read-only handle (PROCESS_VM_READ | PROCESS_QUERY_INFORMATION).");

  if (outPid) *outPid = pid;
  return hProcess;
}

