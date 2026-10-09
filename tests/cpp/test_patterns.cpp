// The memory-root byte patterns: their shape, and
// the scanner's pure core on synthetic code. Whether each matches exactly
// once in the real game is checked live with `wasd-cli.exe roots` and
// offline with tools/find_patterns.py.

namespace patterns_test {
// Code bytes for a pattern: wildcards filled from `fill`, the disp32 at +3 set to `disp`.
std::vector<unsigned char> Concrete(const std::vector<PatternByte>& pat, int32_t disp, unsigned char fill = 0x11) {
  std::vector<unsigned char> out;
  for (const auto& b : pat) out.push_back(b.wildcard ? fill : b.value);
  std::memcpy(out.data() + 3, &disp, sizeof(disp));
  return out;
}
}  // namespace patterns_test

TEST(patterns_have_the_shape_the_resolver_reads) {
  for (const auto& r : kRootPatterns) {
    auto pat = ParsePattern(r.pattern);
    CHECK(pat.size() >= 16);  // long enough not to be unique by chance (find_patterns.py --min)
    CHECK(!pat[0].wildcard && (pat[0].value == 0x48 || pat[0].value == 0x4C));  // REX.W
    CHECK(!pat[1].wildcard && !pat[2].wildcard);                               // opcode, ModRM
    for (int k = 3; k < 7; ++k) CHECK(pat[k].wildcard);                         // the disp32
    CHECK(!pat.back().wildcard);
    uint8_t modrm = pat[2].value;
    if (r.length) CHECK_EQ(modrm & 0xC7, 0x05);  // [rip+disp32]
    else CHECK((modrm & 0xC0) == 0x80 && (modrm & 7) != 4);  // [reg+disp32], no SIB
    for (size_t k = 7; k < pat.size(); ++k) CHECK(!pat[k].wildcard);  // straight-line context only
  }
}

TEST(patterns_find_and_resolve_a_rip_cell_in_synthetic_code) {
  // 0x200 bytes of int3, the WorldChrMan instruction at 0x40 pointing at a cell at 0x1F0.
  std::vector<unsigned char> code(0x200, 0xCC);
  auto pat = ParsePattern(kWorldChrManPattern);
  auto ins = patterns_test::Concrete(pat, 0x1F0 - (0x40 + 7));
  std::copy(ins.begin(), ins.end(), code.begin() + 0x40);
  auto hits = FindPatternInBytes(code.data(), code.size(), pat);
  CHECK_EQ(hits.size(), size_t(1));
  CHECK_EQ(hits[0], size_t(0x40));
  CHECK_EQ(RootValueAt(code.data(), hits[0], 7), uintptr_t(0x1F0));
}

TEST(patterns_struct_offset_is_the_disp_itself) {
  std::vector<unsigned char> code(0x80, 0x90);
  auto pat = ParsePattern(kXaPattern);
  auto ins = patterns_test::Concrete(pat, 0x1F90);
  std::copy(ins.begin(), ins.end(), code.begin() + 8);
  auto hits = FindPatternInBytes(code.data(), code.size(), pat);
  CHECK_EQ(hits.size(), size_t(1));
  CHECK_EQ(RootValueAt(code.data(), hits[0], 0), uintptr_t(0x1F90));
}

TEST(patterns_negative_displacement_and_two_matches) {
  // A cell before the instruction (negative disp32) still resolves; a second copy makes two matches.
  std::vector<unsigned char> code(0x300, 0xCC);
  auto pat = ParsePattern(kParamMasterPattern);
  auto a = patterns_test::Concrete(pat, 0x10 - (0x100 + 7));
  std::copy(a.begin(), a.end(), code.begin() + 0x100);
  auto hits = FindPatternInBytes(code.data(), code.size(), pat);
  CHECK_EQ(hits.size(), size_t(1));
  CHECK_EQ(RootValueAt(code.data(), hits[0], 7), uintptr_t(0x10));
  auto b = patterns_test::Concrete(pat, 0, 0x22);  // different wildcard bytes, same fixed bytes
  std::copy(b.begin(), b.end(), code.begin() + 0x200);
  CHECK_EQ(FindPatternInBytes(code.data(), code.size(), pat).size(), size_t(2));
}

TEST(patterns_edge_cases) {
  std::vector<unsigned char> code = {0x48, 0x8B};
  CHECK(FindPatternInBytes(code.data(), code.size(), ParsePattern(kBaseAPattern)).empty());  // buffer too short
  CHECK(FindPatternInBytes(code.data(), code.size(), {}).empty());                           // empty pattern
}
