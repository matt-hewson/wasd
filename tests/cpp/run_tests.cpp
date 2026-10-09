// C++ test runner. Builds src/main.cpp without its entry point (DS3_NO_MAIN)
// plus every test file below into one program -- main.cpp defines
// everything at file scope, so it can only be compiled once per binary.
// Built and run by test.ps1:
//
//   build\wasd-tests.exe            every test
//   build\wasd-tests.exe route      only tests whose name contains "route"
//
// No game needed: these cover the pure logic (formulas, parsers, session
// bookkeeping, the spoiler/route rules) and lint the real data/ files
// through the real loaders. The exe must sit in build\ so Paths() finds
// data\ above it and sees a dev checkout, as for the tool itself.
#define DS3_NO_MAIN
#include "../../src/main.cpp"

#include "check.h"

#include <map>

#include "test_app.cpp"
#include "test_data.cpp"
#include "test_entry.cpp"
#include "test_formulas.cpp"
#include "test_geometry.cpp"
#include "test_mapdata.cpp"
#include "test_paths.cpp"
#include "test_patterns.cpp"
#include "test_route.cpp"
#include "test_session.cpp"
#include "test_text.cpp"
#include "test_version.cpp"

int wmain(int argc, wchar_t** argv) {
  std::string filter;
  if (argc > 1) {
    for (const wchar_t* p = argv[1]; *p; ++p) filter += static_cast<char>(*p < 128 ? *p : '?');  // names are ASCII
  }
  int ran = 0, failed = 0;
  for (const auto& t : check::Registry()) {
    if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos) continue;
    check::Failures() = 0;
    t.body();
    ++ran;
    if (check::Failures()) {
      ++failed;
      std::printf("FAIL %s\n", t.name);
    } else {
      std::printf("ok   %s\n", t.name);
    }
  }
  std::printf("\n%d test(s), %d failed\n", ran, failed);
  return failed ? 1 : (ran ? 0 : 2);
}
