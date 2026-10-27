// What the relay host reads from Tailscale's CLI (src/core/tailscale.cpp).
// The JSON is shaped like Tailscale 1.104's own output.

TEST(tailscale_status_name_address_and_state) {
  const std::string status = R"({
  "Version": "1.104.2-tc3430f0d2-g598b63867",
  "BackendState": "Running",
  "TailscaleIPs": ["100.64.0.9"],
  "Self": {
    "ID": "n1",
    "HostName": "MY-PC",
    "DNSName": "my-pc.tail1234.ts.net.",
    "OS": "windows",
    "TailscaleIPs": [
      "100.101.102.103",
      "fd7a:115c:a1e0::1"
    ]
  },
  "Peer": {
    "nodekey:x": { "DNSName": "partner-pc.tail1234.ts.net.", "TailscaleIPs": ["100.70.1.1"] }
  }
})";
  TailscaleInfo info;
  ParseTailscaleStatus(status, &info);
  CHECK(info.running);
  CHECK_EQ(info.name, std::string("my-pc.tail1234.ts.net"));  // Self's, without the trailing dot
  CHECK_EQ(info.ip, std::string("100.101.102.103"));
  TailscaleInfo off;
  ParseTailscaleStatus(R"({"BackendState": "Stopped", "Self": {"DNSName": ""}})", &off);
  CHECK(!off.running);
  CHECK_EQ(off.name, std::string(""));
  TailscaleInfo junk;
  ParseTailscaleStatus("not json", &junk);
  CHECK(!junk.running && junk.name.empty() && junk.ip.empty());
}

TEST(tailscale_serve_forwards_the_relay_port) {
  const std::string serving = R"({
  "TCP": {
    "8790": {
      "TCPForward": "localhost:8790"
    }
  }
})";
  CHECK(TailscaleServesTcp(serving, 8790));
  CHECK(!TailscaleServesTcp(serving, 8791));
  CHECK(TailscaleServesTcp(R"({"TCP":{"8790":{"TCPForward":"127.0.0.1:8790"}}})", 8790));
  CHECK(!TailscaleServesTcp(R"({"TCP":{"8790":{"TCPForward":"localhost:9999"}}})", 8790));  // to something else
  CHECK(!TailscaleServesTcp(R"({"TCP":{"443":{"HTTPS":true}},"Web":{}})", 8790));
  CHECK(!TailscaleServesTcp("{}", 8790));
  CHECK(!TailscaleServesTcp("No serve config", 8790));
}

TEST(tailscale_forward_commands) {
  CHECK_EQ(TailscaleForwardArgs(8790, true), std::wstring(L"serve --bg --tcp 8790 tcp://localhost:8790"));
  CHECK_EQ(TailscaleForwardArgs(8790, false), std::wstring(L"serve --tcp=8790 off"));
}

TEST(tailscale_serve_problem_from_its_output) {
  CHECK_EQ(TailscaleServeProblem(""), std::string(""));
  CHECK_EQ(TailscaleServeProblem("\r\n  access denied: run as the user logged in to Tailscale\r\n\r\nmore\r\n"),
           std::string("access denied: run as the user logged in to Tailscale"));
  // A link Tailscale wants opened is kept, even when it's on a later line.
  CHECK_EQ(TailscaleServeProblem("Serve is not enabled on your tailnet.\nTo enable, visit:\n\n  https://login.tailscale.com/f/serve?node=n1\n"),
           std::string("Serve is not enabled on your tailnet. https://login.tailscale.com/f/serve?node=n1"));
  CHECK_EQ(TailscaleServeProblem(std::string(300, 'x')).size(), size_t{203});
}

TEST(tailscale_run_and_capture) {
  std::string out;
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  CHECK(RunAndCapture(std::wstring(sys) + L"\\cmd.exe", L"/c echo hello", &out));
  CHECK(out.find("hello") != std::string::npos);
  CHECK(!RunAndCapture(std::wstring(sys) + L"\\cmd.exe", L"/c exit 3", &out));  // failed
  CHECK(!RunAndCapture(L"Z:\\no\\such.exe", L"", &out));
}
