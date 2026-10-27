// src/ds3/game.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.


constexpr wchar_t kTargetProcessName[] = L"DarkSoulsIII.exe";

// Best-effort marker that the attached process is the Seamless Co-op
// (EAC-disabled) build rather than vanilla online DS3. Both run under
// the same DarkSoulsIII.exe process name, so this module-presence
// check is the only practical signal we have. Confirmed present in a
// live Seamless Co-op session via `probe --list-modules` on 2026-08-17.
constexpr wchar_t kSeamlessCoopModuleMarker[] = L"ds3sc.dll";

std::wstring WidenUtf8(const std::string& s);  // defined with the JSON helpers

