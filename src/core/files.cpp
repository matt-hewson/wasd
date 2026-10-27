// src/core/files.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- report mode: results page from saved sessions -----
//
// Collects every sessions/<character>/*.jsonl, injects them into
// templates/results_template.html in place of __SESSIONS_JSON__ as
// [{"file": ..., "events": [<each line>]}, ...], writes
// reports/results.html and opens it in the default browser. The page is
// self-contained (inline JS/SVG, no network). Lines that aren't a whole
// {...} object (e.g. a line cut off by a crash mid-write) are skipped.
std::string ReadWholeFile(const std::wstring& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

