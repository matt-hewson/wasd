// src/core/hexdump.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- shared hex-dump printer (read-only diagnostic, used by dump/inv) --

void PrintHexDump(const unsigned char* buf, SIZE_T bytesRead) {
  for (SIZE_T i = 0; i < bytesRead; i += 16) {
    std::printf("  +%04llx: ", static_cast<unsigned long long>(i));
    for (SIZE_T j = 0; j < 16; ++j) {
      if (i + j < bytesRead)
        std::printf("%02X ", buf[i + j]);
      else
        std::printf("   ");
    }
    std::printf(" ");
    for (SIZE_T j = 0; j < 16 && i + j < bytesRead; ++j) {
      unsigned char c = buf[i + j];
      std::printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
    }
    std::printf("\n");
  }
}

// Writes a byte-exact binary capture to disk (still just the bytes
// already read via ReadProcessMemory above -- this never touches the
// target process itself). Used to feed captured code into an offline
// disassembler without retyping hex by hand.
bool SaveRawBytes(const std::wstring& path, const unsigned char* buf, SIZE_T length) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(buf), static_cast<std::streamsize>(length));
  return static_cast<bool>(out);
}

// ---- dump mode: raw hex dump of module bytes (read-only diagnostic) --

int RunDump(uintptr_t moduleOffset, SIZE_T length, const std::wstring& savePath) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;

  uintptr_t moduleBase = GetModuleBase(pid, kTargetProcessName);
  if (moduleBase == 0) {
    Log("Could not resolve module base address.");
    CloseHandle(hProcess);
    return 4;
  }

  uintptr_t addr = moduleBase + moduleOffset;
  std::vector<unsigned char> buf(length);
  SIZE_T bytesRead = 0;
  BOOL ok = ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr), buf.data(), length,
                               &bytesRead);
  CloseHandle(hProcess);

  if (!ok) {
    Log("Read failed at DarkSoulsIII.exe+0x%llx (0x%llx) (error %lu).",
        static_cast<unsigned long long>(moduleOffset), static_cast<unsigned long long>(addr),
        GetLastError());
    return 5;
  }

  Log("Dumping %zu bytes at DarkSoulsIII.exe+0x%llx (0x%llx):",
      bytesRead, static_cast<unsigned long long>(moduleOffset),
      static_cast<unsigned long long>(addr));
  PrintHexDump(buf.data(), bytesRead);
  if (!savePath.empty()) {
    if (SaveRawBytes(savePath, buf.data(), bytesRead)) {
      std::wprintf(L"Saved %zu raw bytes to %ls\n", bytesRead, savePath.c_str());
    } else {
      Log("Failed to save bytes to the given path.");
    }
  }
  return 0;
}

// ---- dumpmodule: the whole game module to a file (read-only) ---------
//
// For finding our own byte patterns offline:
// tools/find_patterns.py reads the file. Read in 64 KB chunks; a chunk
// that fails is retried page by page, and pages that still can't be read
// (guard or no-access pages) are left as zeros and counted. Nothing is
// printed but a summary; the file is the game's own code, so it belongs
// in a private folder, never in the repository.
int RunDumpModule(const std::wstring& savePath) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;
  auto [moduleBase, moduleSize] = GetModuleBaseAndSize(pid, kTargetProcessName);
  if (moduleBase == 0 || moduleSize == 0) {
    Log("Could not resolve the module base and size.");
    CloseHandle(hProcess);
    return 4;
  }
  constexpr SIZE_T kChunk = 0x10000, kPage = 0x1000;
  std::vector<unsigned char> image(moduleSize, 0);
  SIZE_T unreadable = 0;
  for (SIZE_T off = 0; off < moduleSize; off += kChunk) {
    SIZE_T len = (std::min)(kChunk, moduleSize - off);
    SIZE_T br = 0;
    if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(moduleBase + off), image.data() + off, len, &br) &&
        br == len)
      continue;
    for (SIZE_T p = off; p < off + len; p += kPage) {
      SIZE_T plen = (std::min)(kPage, off + len - p);
      if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(moduleBase + p), image.data() + p, plen, &br) ||
          br != plen) {
        std::fill(image.begin() + p, image.begin() + p + plen, 0);
        unreadable += plen;
      }
    }
  }
  CloseHandle(hProcess);
  if (!SaveRawBytes(savePath, image.data(), image.size())) {
    Log("Failed to save the module to the given path.");
    return 5;
  }
  Log("Saved DarkSoulsIII.exe's module (0x%llx bytes at 0x%llx; 0x%llx unreadable bytes left as zeros) to %ls",
      static_cast<unsigned long long>(moduleSize), static_cast<unsigned long long>(moduleBase),
      static_cast<unsigned long long>(unreadable), savePath.c_str());
  return 0;
}

// ---- peek mode: raw hex dump of an absolute address (read-only) --
//
// Same mechanism as `dump`, but takes an absolute address instead of a
// module-relative offset -- for inspecting scan/rescan hits and other
// heap addresses found live. Not restart-proof by itself (the address
// has to be re-derived each run); it's an investigative tool, not a
// permanent command.

int RunPeek(uintptr_t address, SIZE_T length, const std::wstring& savePath) {
  DWORD pid = 0;
  HANDLE hProcess = AttachReadOnly(&pid);
  if (!hProcess) return 2;

  std::vector<unsigned char> buf(length);
  SIZE_T bytesRead = 0;
  BOOL ok = ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(address), buf.data(), length,
                               &bytesRead);
  CloseHandle(hProcess);

  if (!ok) {
    Log("Read failed at 0x%llx (error %lu).", static_cast<unsigned long long>(address),
        GetLastError());
    return 5;
  }

  Log("Dumping %zu bytes at 0x%llx:", bytesRead, static_cast<unsigned long long>(address));
  PrintHexDump(buf.data(), bytesRead);
  if (!savePath.empty()) {
    if (SaveRawBytes(savePath, buf.data(), bytesRead)) {
      std::wprintf(L"Saved %zu raw bytes to %ls\n", bytesRead, savePath.c_str());
    } else {
      Log("Failed to save bytes to the given path.");
    }
  }
  return 0;
}

