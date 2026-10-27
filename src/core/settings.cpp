// src/core/settings.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- spoiler tiers -------------------------------------
//
// One tier for every hint, chosen by the user 2026-10-06:
//   Off      -- no guidance (counts only)
//   Vague    -- "something unfound, 40 m, 3 o'clock"; boss counts only
//   Category -- what kind of thing ("a ring"); defeated bosses named
//   Full     -- names everything
// Default Vague. F9 cycles it in the window/overlay and saves it to
// settings.ini (Paths().Settings(), git-ignored); commands follow it too, and
// `--full` shows everything for one run. Things you already have (found
// items, held keys, defeated bosses at Category+) are never hidden.
enum : int { kTierOff = 0, kTierVague = 1, kTierCategory = 2, kTierFull = 3 };
volatile LONG g_spoilerTier = kTierVague;

int SpoilerTier() { return static_cast<int>(g_spoilerTier); }

const char* SpoilerTierName(int tier) {
  static const char* names[] = {"Off", "Vague", "Category", "Full"};
  return names[(tier < 0 || tier > 3) ? kTierVague : tier];
}

std::wstring SettingsPath() { return Paths().Settings(); }

// Overlay settings, changed live from the live page (and F11 for perf).
// Written by the page server thread / F11, read by the UI thread: each is
// a whole LONG, so no lock is needed. Saved with the tier in settings.ini.
struct OverlaySettings {
  volatile LONG visible = 1;  // shown at all (F10 still hides it AND pauses polling)
  volatile LONG nearest = 1, bosses = 1, bossHint = 1, route = 1, missable = 1, perf = 0;  // which lines show
  volatile LONG corner = 0;    // of the game window: 0 top-right, 1 top-left, 2 bottom-right, 3 bottom-left
  volatile LONG opacity = 88;  // percent
  volatile LONG text = 15;     // font px at 96 DPI
  volatile LONG width = 340;   // panel px at 96 DPI
};
OverlaySettings g_ov;

// Live page settings (POST /api/page), saved with the tier: the theme and
// which panels are muted. The page owns no state of its own beyond these.
const char* const kPageThemes[3] = {"Bonfire", "Ledger", "Cartographer"};  // API ids a, b, c
const char* const kPagePanels[6] = {"char", "map", "boss", "gear", "items", "route"};
volatile LONG g_pageTheme = 0;  // index into kPageThemes
volatile LONG g_pageMuted = 0;  // bit i = kPagePanels[i] muted

// "map,items" -> bits; false if any name is unknown (nothing changed then).
bool SetPageMuted(const std::string& list) {
  LONG bits = 0;
  for (size_t pos = 0; pos <= list.size();) {
    size_t comma = list.find(',', pos);
    std::string name = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    if (!name.empty()) {
      int found = -1;
      for (int i = 0; i < 6; ++i)
        if (name == kPagePanels[i]) found = i;
      if (found < 0) return false;
      bits |= 1L << found;
    }
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  InterlockedExchange(&g_pageMuted, bits);
  return true;
}

std::string PageMutedList() {
  std::string out;
  for (int i = 0; i < 6; ++i)
    if (g_pageMuted & (1L << i)) out += (out.empty() ? "" : ",") + std::string(kPagePanels[i]);
  return out;
}

struct OverlaySettingDef {
  const char* key;  // in the API and (prefixed "overlay_") in settings.ini
  volatile LONG* value;
  LONG min, max;
};
const OverlaySettingDef kOverlaySettingDefs[] = {
    {"visible", &g_ov.visible, 0, 1}, {"nearest", &g_ov.nearest, 0, 1}, {"bosses", &g_ov.bosses, 0, 1},
    {"bossHint", &g_ov.bossHint, 0, 1}, {"route", &g_ov.route, 0, 1},   {"missable", &g_ov.missable, 0, 1},
    {"perf", &g_ov.perf, 0, 1},
    {"corner", &g_ov.corner, 0, 3},
    {"opacity", &g_ov.opacity, 30, 100}, {"text", &g_ov.text, 11, 24},  {"width", &g_ov.width, 240, 560},
};

// Validated set from text ("corner" also takes top-right / top-left /
// bottom-right / bottom-left). False if the key or value is invalid.
bool SetOverlaySetting(const std::string& key, const std::string& value) {
  static const char* corners[] = {"top-right", "top-left", "bottom-right", "bottom-left"};
  for (const auto& d : kOverlaySettingDefs) {
    if (key != d.key) continue;
    long v = -1;
    if (key == "corner")
      for (int c = 0; c < 4; ++c)
        if (value == corners[c]) v = c;
    char* end = nullptr;
    if (v < 0) v = std::strtol(value.c_str(), &end, 10);
    if (v < 0 && end == value.c_str()) return false;
    if (end && (end == value.c_str() || *end)) return false;
    if (v < d.min || v > d.max) return false;
    InterlockedExchange(d.value, v);
    return true;
  }
  return false;
}

// The DS3 "Game" folder the map data was read from (UTF-8; "" = unknown).
// Set by `extract` / the first-run step before any other thread starts.
std::string g_gameDir;

// The live page's first port (the next 9 are tried if it's taken), and the
// co-op relay: hosted by this copy (host_relay, on 127.0.0.1:relay_port) or
// the partner's, at relay_url ("" = none). A second copy, such as a test
// partner, sets its own in its own settings.ini. auto_pair: join the
// partner by the Seamless Co-op password, without a code.
constexpr int kLivePortDefault = 8765, kLivePortSpan = 10;
constexpr int kRelayPortDefault = 8790;
int g_livePort = kLivePortDefault;
std::string g_relayUrl;  // relay_url; the live page can change it, so read and set it under g_relayUrlLock
std::mutex g_relayUrlLock;
std::atomic<bool> g_hostRelay{false}, g_autoPair{true};
int g_relayPort = kRelayPortDefault;

std::string RelayUrlSetting() {
  std::lock_guard<std::mutex> g(g_relayUrlLock);
  return g_relayUrl;
}
void SetRelayUrlSetting(const std::string& url) {
  std::lock_guard<std::mutex> g(g_relayUrlLock);
  g_relayUrl = url;
}

// Where this copy's co-op link connects: its own relay when it hosts one,
// else relay_url ("" = no relay).
std::string CoopRelayUrl() {
  if (g_hostRelay) return "http://127.0.0.1:" + std::to_string(g_relayPort);
  return RelayUrlSetting();
}

// A relay port: 1024-65535. -1 if not.
int ParseRelayPort(const std::string& v) {
  if (v.empty() || v.size() > 5 || v.find_first_not_of("0123456789") != std::string::npos) return -1;
  int port = std::atoi(v.c_str());
  return port >= 1024 && port <= 65535 ? port : -1;
}

// A port for the live page: 1024-65526, so all ten tries stay valid. -1 if not.
int ParseLivePort(const std::string& v) {
  if (v.empty() || v.size() > 5 || v.find_first_not_of("0123456789") != std::string::npos) return -1;
  int port = std::atoi(v.c_str());
  return port >= 1024 && port <= 65535 - (kLivePortSpan - 1) ? port : -1;
}

// "http(s)://host[:port][/path]" without a trailing slash; "" stays "" (no
// relay). False for anything else.
bool ParseRelayUrl(const std::string& v, std::string* out) {
  if (v.empty()) {
    out->clear();
    return true;
  }
  size_t scheme = v.rfind("http://", 0) == 0 ? 7 : v.rfind("https://", 0) == 0 ? 8 : 0;
  if (!scheme || v.size() == scheme) return false;
  for (unsigned char c : v)
    if (c <= ' ' || c >= 0x7F) return false;
  std::string url = v;
  while (url.size() > scheme && url.back() == '/') url.pop_back();
  if (url.size() == scheme || url[scheme] == '/') return false;  // no host
  *out = url;
  return true;
}

// The host's relay address as a partner types or pastes it, as relay_url:
// the whole line the host's popover copies ("relay_url=http://..."), a URL,
// or just the host's Tailscale name or 100.x address. No scheme means
// http://, and an http:// address without a port gets the relay's (8790),
// since that's where the host's relay always is. "" if it can't be one.
std::string NormalizeRelayInput(const std::string& typed, int defaultPort = kRelayPortDefault) {
  std::string v = typed;
  auto trim = [&] {
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(v.begin());
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
  };
  trim();
  if (v.size() >= 9 && _strnicmp(v.c_str(), "relay_url", 9) == 0) {  // "relay_url=...", spaces allowed round the =
    size_t eq = v.find_first_not_of(" \t", 9);
    if (eq == std::string::npos || v[eq] != '=') return "";
    v.erase(0, eq + 1), trim();
  }
  if (v.empty()) return "";
  std::string lower = v;
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  bool https = lower.rfind("https://", 0) == 0;
  if (lower.rfind("http://", 0) == 0) v = "http://" + v.substr(7);
  else if (https) v = "https://" + v.substr(8);
  else if (lower.find("://") != std::string::npos) return "";  // ws://, ftp://...
  else v = "http://" + v;
  std::string url;
  if (!ParseRelayUrl(v, &url)) return "";
  if (!https) {
    size_t hostStart = 7, hostEnd = url.find('/', hostStart);
    std::string host = url.substr(hostStart, hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
    bool hasPort = host[0] == '[' ? host.find("]:") != std::string::npos : host.find(':') != std::string::npos;
    if (!hasPort) url.insert(hostStart + host.size(), ":" + std::to_string(defaultPort));
  }
  return url;
}

void LoadSettings() {
  std::ifstream in(SettingsPath());
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k == "spoiler_tier") {
      for (int t = 0; t <= 3; ++t)
        if (_stricmp(v.c_str(), SpoilerTierName(t)) == 0) InterlockedExchange(&g_spoilerTier, t);
    } else if (k.rfind("overlay_", 0) == 0) {
      SetOverlaySetting(k.substr(8), v);  // bad values are ignored, keeping the default
    } else if (k == "page_theme") {
      for (int t = 0; t < 3; ++t)
        if (_stricmp(v.c_str(), kPageThemes[t]) == 0) InterlockedExchange(&g_pageTheme, t);
    } else if (k == "page_muted") {
      SetPageMuted(v);
    } else if (k == "game_dir") {
      g_gameDir = v;
    } else if (k == "live_port") {
      int port = ParseLivePort(v);
      if (port > 0) g_livePort = port;
      else Log("settings.ini: live_port=%s isn't a port from 1024 to %d -- using %d.", v.c_str(),
               65535 - (kLivePortSpan - 1), g_livePort);
    } else if (k == "relay_url") {
      std::string url;
      if (ParseRelayUrl(v, &url)) SetRelayUrlSetting(url);
      else
        Log("settings.ini: relay_url=%s isn't an http:// or https:// address -- ignored.", v.c_str());
    } else if (k == "host_relay") {
      g_hostRelay = v == "1";
    } else if (k == "relay_port") {
      int port = ParseRelayPort(v);
      if (port > 0) g_relayPort = port;
      else Log("settings.ini: relay_port=%s isn't a port from 1024 to 65535 -- using %d.", v.c_str(), g_relayPort);
    } else if (k == "auto_pair") {
      g_autoPair = v != "0";
    }
  }
}

void SaveSettings() {
  static const char* corners[] = {"top-right", "top-left", "bottom-right", "bottom-left"};
  EnsureDir(Paths().user);
  std::ofstream out(SettingsPath(), std::ios::trunc);
  out << "# WASD settings (also changed from the live page).\n"
      << "# spoiler_tier: Off, Vague, Category or Full (F9 in game cycles it).\n"
      << "spoiler_tier=" << SpoilerTierName(SpoilerTier()) << "\n";
  for (const auto& d : kOverlaySettingDefs) {
    out << "overlay_" << d.key << "=";
    if (std::string(d.key) == "corner") out << corners[*d.value & 3] << "\n";
    else out << *d.value << "\n";
  }
  out << "# Live page: theme (Bonfire, Ledger or Cartographer) and muted panels.\n"
      << "page_theme=" << kPageThemes[g_pageTheme % 3] << "\n"
      << "page_muted=" << PageMutedList() << "\n";
  out << "# The DS3 Game folder map data is read from (wasd-cli.exe extract).\n"
      << "game_dir=" << g_gameDir << "\n";
  out << "# Live page port (the next 9 are tried if it's taken).\n"
      << "live_port=" << g_livePort << "\n"
      << "# Co-op relay. One of you hosts it (host_relay=1, on 127.0.0.1:relay_port;\n"
      << "# Tailscale carries the partner to it); the other sets relay_url to the\n"
      << "# host's Tailscale address, e.g. http://my-pc.tail1234.ts.net:8790.\n"
      << "# auto_pair=1 joins your partner by your Seamless Co-op password.\n"
      << "host_relay=" << (g_hostRelay ? 1 : 0) << "\n"
      << "relay_port=" << g_relayPort << "\n"
      << "relay_url=" << RelayUrlSetting() << "\n"
      << "auto_pair=" << (g_autoPair ? 1 : 0) << "\n";
}

int CycleSpoilerTier() {
  int next = (SpoilerTier() + 1) % 4;
  InterlockedExchange(&g_spoilerTier, next);
  SaveSettings();
  return next;
}

