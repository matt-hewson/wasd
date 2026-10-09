// Text and data formats: JSON for the live page and report, folder names,
// durations, overlay line wrapping, the perf CSV the report reads.

TEST(json_string_escapes) {
  CHECK_EQ(JsonString("plain"), std::string("\"plain\""));
  CHECK_EQ(JsonString("a\"b\\c"), std::string("\"a\\\"b\\\\c\""));
  CHECK_EQ(JsonString("line\nnext\ttab\r"), std::string("\"line\\nnext\\ttab\\r\""));
  CHECK_EQ(JsonString(std::string("\x01", 1)), std::string("\"\\u0001\""));
  CHECK_EQ(JsonString("Irithyll \xE2\x80\x93 Tower"), std::string("\"Irithyll \xE2\x80\x93 Tower\""));  // UTF-8 kept
  CHECK_EQ(JsonString(""), std::string("\"\""));
}

TEST(json_number_format) {
  CHECK_EQ(JsonNum(3.14159), std::string("3.14"));
  CHECK_EQ(JsonNum(3.14159, 0), std::string("3"));
  CHECK_EQ(JsonNum(-0.5, 1), std::string("-0.5"));
  CHECK_EQ(JsonNum(std::nan("")), std::string("null"));
  CHECK_EQ(JsonNum(INFINITY), std::string("null"));
}

TEST(safe_folder_name) {
  CHECK_EQ(SafeFolderName("Little John"), std::wstring(L"Little John"));
  CHECK_EQ(SafeFolderName("Ashen One"), std::wstring(L"Ashen One"));
  CHECK_EQ(SafeFolderName("a/b:c*d?"), std::wstring(L"a_b_c_d_"));
  CHECK_EQ(SafeFolderName("..\\up"), std::wstring(L"___up"));
  CHECK_EQ(SafeFolderName(""), std::wstring(L"Unknown"));
}

TEST(duration_format) {
  CHECK_EQ(FormatDuration(0), std::string("0:00:00"));
  CHECK_EQ(FormatDuration(59.9), std::string("0:00:59"));
  CHECK_EQ(FormatDuration(3725), std::string("1:02:05"));
  CHECK_EQ(FormatDuration(36000), std::string("10:00:00"));
}

TEST(overlay_wrap_keeps_short_lines) {
  auto lines = WrapOverlayText(L"Bosses 1 / 1 defeated here", 40);
  CHECK_EQ(lines.size(), size_t(1));
  CHECK_EQ(lines[0], std::wstring(L"Bosses 1 / 1 defeated here"));
}

TEST(overlay_wrap_breaks_at_section_separators_first) {
  auto lines = WrapOverlayText(L"weak: Fire -20% | resists: Dark 30% | immune: poison", 22);
  CHECK_EQ(lines.size(), size_t(3));
  CHECK_EQ(lines[0], std::wstring(L"weak: Fire -20%"));
  CHECK_EQ(lines[1], std::wstring(L"resists: Dark 30%"));
  CHECK_EQ(lines[2], std::wstring(L"immune: poison"));
  // Sections that fit together share a line.
  auto joined = WrapOverlayText(L"a | b | c", 40);
  CHECK_EQ(joined.size(), size_t(1));
  CHECK_EQ(joined[0], std::wstring(L"a | b | c"));
}

TEST(overlay_wrap_long_section_at_spaces_and_hard_cut) {
  auto lines = WrapOverlayText(L"one two three four", 9);
  CHECK_EQ(lines.size(), size_t(3));
  CHECK_EQ(lines[0], std::wstring(L"one two"));
  CHECK_EQ(lines[1], std::wstring(L"three"));
  CHECK_EQ(lines[2], std::wstring(L"four"));
  auto hard = WrapOverlayText(L"abcdefghij", 4);
  CHECK_EQ(hard.size(), size_t(3));
  CHECK_EQ(hard[0], std::wstring(L"abcd"));
  CHECK_EQ(hard[2], std::wstring(L"ij"));
  for (const auto& l : WrapOverlayText(L"weak: Lightning -14% | resists: Dark 38% | immune: poison, toxic | bleed 200, frost 63", 20))
    CHECK(l.size() <= 20);
}

TEST(perf_csv_to_json_keeps_good_rows_only) {
  std::string csv =
      std::string(kPerfCsvHeader) + "\n" +
      "2026-10-07T10:00:05,5.0,5.00,50,9.99,12.6,32.1,0.622,25.6,21.6\n" +
      "2026-10-07T10:00:10,10.0,5.00,50,10.00,12." + "\n" +           // cut off mid-write
      "2026-10-07T10:00:15,15.0,5.00,50,nan,12.6,32.1,0.622,25.6,21.6\n" +  // not finite
      "10:00:20 bad time,20.0,5.00,50,9.99,12.6,32.1,0.622,25.6,21.6\n" +
      "2026-10-07T10:00:25,25.0,5.00,50,9.98,12.0,30.0,0.600,25.7,21.7\r\n";  // CRLF
  CHECK_EQ(PerfCsvToJson(csv),
           std::string("[[\"2026-10-07T10:00:05\",5.0,5.00,50,9.99,12.6,32.1,0.622,25.6,21.6],"
                       "[\"2026-10-07T10:00:25\",25.0,5.00,50,9.98,12.0,30.0,0.600,25.7,21.7]]"));
  CHECK_EQ(PerfCsvToJson(""), std::string(""));
  CHECK_EQ(PerfCsvToJson(std::string(kPerfCsvHeader) + "\n"), std::string(""));
}

TEST(perf_csv_row_matches_the_header) {
  PerfWindow::Sample s;
  s.valid = true;
  s.ticks = 50;
  s.seconds = 5.0;
  s.rateHz = 10.0;
  s.avgMicros = 12.34;
  s.maxMicros = 40.0;
  s.cpuPercentOfCore = 0.5;
  s.workingSetMB = 25.5;
  s.privateMB = 21.0;
  std::string row = PerfCsvRow(s, 123.4);
  CHECK_EQ(SplitStr(row, ',').size(), SplitStr(kPerfCsvHeader, ',').size());
  CHECK(row.find(",123.4,5.00,50,10.00,12.3,40.0,0.500,25.5,21.0") != std::string::npos);
  CHECK(!PerfCsvToJson(row).empty());  // what the tool writes, the report reads
}
