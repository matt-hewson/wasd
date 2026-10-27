// src/core/text.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

std::wstring WidenUtf8(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string JsonString(const std::string& s) {
  std::string out = "\"";
  for (unsigned char ch : s) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (ch < 0x20) {
          char esc[8];
          std::snprintf(esc, sizeof(esc), "\\u%04x", ch);
          out += esc;
        } else {
          out += static_cast<char>(ch);
        }
    }
  }
  return out + "\"";
}

// Folder-safe version of a character name (keeps letters, digits, space, - _).
std::wstring SafeFolderName(const std::string& name) {
  std::wstring w = WidenUtf8(name), out;
  for (wchar_t c : w) out += (iswalnum(c) || c == L' ' || c == L'-' || c == L'_') ? c : L'_';
  return out.empty() ? L"Unknown" : out;
}

