// src/core/tailscale.cpp: part of WASD's one translation unit. src/main.cpp includes it,
// in order, inside its namespace; it isn't compiled on its own.

// ---- Tailscale: what the host needs to tell the partner ----------------
//
// While this copy hosts the co-op relay, the live page shows the host the
// address their partner puts in relay_url, and whether Tailscale forwards
// the relay's port yet. Read from Tailscale's own CLI (`tailscale status
// --json`, `tailscale serve status --json`). The one change WASD makes to
// Tailscale is the forward itself, and only when the host clicks: "Forward
// it with Tailscale" runs `tailscale serve --bg --tcp <port> tcp://localhost:<port>`
// (Tailscale keeps it across restarts); "Stop forwarding" runs
// `tailscale serve --tcp=<port> off`.

struct TailscaleInfo {
  bool installed = false;  // tailscale.exe found
  bool running = false;    // logged in and connected
  std::string name;        // this PC's MagicDNS name, e.g. my-pc.tail1234.ts.net ("" unknown)
  std::string ip;          // its 100.x address
  bool serving = false;    // `tailscale serve` forwards the relay's port to it
  std::string forwardError;  // why the last Forward / Stop forwarding click failed ("" none)
};

// A JSON string value after `"key":` from `at` on, "" if none (no escapes
// expected: Tailscale's names and addresses don't have any).
std::string JsonStringAfter(const std::string& json, const std::string& key, size_t at = 0) {
  size_t k = json.find("\"" + key + "\"", at);
  if (k == std::string::npos) return "";
  size_t colon = json.find(':', k);
  if (colon == std::string::npos) return "";
  size_t open = json.find_first_not_of(" \t\r\n", colon + 1);
  if (open == std::string::npos || json[open] != '"') return "";
  size_t close = json.find('"', open + 1);
  return close == std::string::npos ? "" : json.substr(open + 1, close - open - 1);
}

// From `tailscale status --json`: running, this PC's name (no trailing dot) and 100.x address.
void ParseTailscaleStatus(const std::string& json, TailscaleInfo* info) {
  info->running = JsonStringAfter(json, "BackendState") == "Running";
  size_t self = json.find("\"Self\"");
  if (self == std::string::npos) return;
  std::string name = JsonStringAfter(json, "DNSName", self);
  while (!name.empty() && name.back() == '.') name.pop_back();
  info->name = name;
  size_t ips = json.find("\"TailscaleIPs\"", self);
  if (ips != std::string::npos) {
    size_t open = json.find('"', json.find('[', ips) + 1);
    size_t close = open == std::string::npos ? open : json.find('"', open + 1);
    if (close != std::string::npos) info->ip = json.substr(open + 1, close - open - 1);
  }
}

// From `tailscale serve status --json`: whether TCP `port` is forwarded to localhost:`port`.
bool TailscaleServesTcp(const std::string& json, int port) {
  std::string flat;
  for (char c : json)
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') flat += c;
  const std::string p = std::to_string(port);
  size_t tcp = flat.find("\"TCP\":{");
  if (tcp == std::string::npos) return false;
  size_t entry = flat.find("\"" + p + "\":{", tcp);
  if (entry == std::string::npos) return false;
  std::string target = JsonStringAfter(flat, "TCPForward", entry);
  return target == "localhost:" + p || target == "127.0.0.1:" + p;
}

// Runs `exe args` with no window and returns what it printed (stdout), or
// false if it couldn't start, failed or took longer than `timeoutMs`.
bool RunAndCapture(const std::wstring& exe, const std::wstring& args, std::string* out, DWORD timeoutMs = 5000) {
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE readEnd = nullptr, writeEnd = nullptr;
  if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return false;
  SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = writeEnd;
  si.hStdError = writeEnd;
  si.hStdInput = nullptr;
  std::wstring cmd = L"\"" + exe + L"\" " + args;
  PROCESS_INFORMATION pi{};
  BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(writeEnd);
  if (!started) {
    CloseHandle(readEnd);
    return false;
  }
  out->clear();
  const ULONGLONG end = GetTickCount64() + timeoutMs;
  bool exited = false;
  for (;;) {
    DWORD avail = 0;
    while (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
      char buf[4096];
      DWORD got = 0;
      if (!ReadFile(readEnd, buf, (std::min)(avail, DWORD{sizeof(buf)}), &got, nullptr) || !got) break;
      if (out->size() < (1 << 20)) out->append(buf, got);
    }
    if (exited) break;
    exited = WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0;  // one more read after it exits
    if (!exited && GetTickCount64() > end) {
      TerminateProcess(pi.hProcess, 1);
      break;
    }
  }
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  CloseHandle(readEnd);
  return exited && code == 0;
}

// tailscale.exe where Tailscale's installer puts it ("" if it isn't there).
std::wstring TailscaleExe() {
  wchar_t pf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
  if (!n || n >= MAX_PATH) return L"";
  std::wstring exe = std::wstring(pf) + L"\\Tailscale\\tailscale.exe";
  return GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES ? L"" : exe;
}

// Why `tailscale serve` didn't forward, from what it printed: its first
// line that says something, and any link Tailscale wants opened (e.g. to
// allow Serve on the tailnet). "" when it printed nothing useful.
std::string TailscaleServeProblem(const std::string& output) {
  std::string first, link;
  for (size_t at = 0; at < output.size();) {
    size_t end = output.find('\n', at);
    std::string line = output.substr(at, end == std::string::npos ? std::string::npos : end - at);
    at = end == std::string::npos ? output.size() : end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    size_t lead = line.find_first_not_of(" \t");
    if (lead == std::string::npos) continue;
    line.erase(0, lead);
    if (first.empty()) first = line;
    size_t url = line.find("https://");
    if (link.empty() && url != std::string::npos) link = line.substr(url, line.find(' ', url) - url);
  }
  if (first.size() > 200) first = first.substr(0, 200) + "...";
  if (!link.empty() && first.find(link) == std::string::npos) first += " " + link;
  return first;
}

// The `tailscale` arguments that forward the relay's `port` on this PC's
// Tailscale address to 127.0.0.1 (on), or remove that forward (off).
std::wstring TailscaleForwardArgs(int port, bool on) {
  const std::wstring p = std::to_wstring(port);
  return on ? L"serve --bg --tcp " + p + L" tcp://localhost:" + p : L"serve --tcp=" + p + L" off";
}

// Asks Tailscale to forward the relay's port, or to stop. "" when done, else why not.
std::string TailscaleForwardRelay(int port, bool on) {
  const std::wstring exe = TailscaleExe();
  if (exe.empty()) return "Tailscale isn't installed";
  std::string out;
  if (RunAndCapture(exe, TailscaleForwardArgs(port, on), &out, 15000)) {
    Log("Co-op relay host: asked Tailscale to %s port %d.", on ? "forward the relay's" : "stop forwarding", port);
    return "";
  }
  std::string why = TailscaleServeProblem(out);
  if (why.empty()) why = "tailscale serve didn't finish";
  Log("Co-op relay host: tailscale serve failed: %s", why.c_str());
  return why;
}

// What Tailscale says now, for the relay's `port`. Runs the CLI twice (a few
// tenths of a second): call it off the page's threads.
TailscaleInfo ReadTailscale(int port) {
  TailscaleInfo info;
  const std::wstring exe = TailscaleExe();
  if (exe.empty()) return info;
  info.installed = true;
  std::string json;
  if (RunAndCapture(exe, L"status --json", &json)) ParseTailscaleStatus(json, &info);
  if (info.running && RunAndCapture(exe, L"serve status --json", &json)) info.serving = TailscaleServesTcp(json, port);
  return info;
}
