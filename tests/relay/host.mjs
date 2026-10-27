// Shared by the relay tests: run the co-op relay the host's WASD runs
// (`wasd-cli.exe relay-host`, src/core/relay_server.cpp), and WebSocket
// clients for it. Skipped on other systems, or until build.ps1 has built the exe.
import assert from "node:assert/strict";
import { spawn, execFileSync } from "node:child_process";
import { existsSync } from "node:fs";
import { request } from "node:http";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

export const REPO = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
// WASD_BIN_DIR: test another build's exe (release.ps1 points it at build\release).
export const BIN = process.env.WASD_BIN_DIR || join(REPO, "build");
export const CLI = join(BIN, "wasd-cli.exe");
export const skip = process.platform !== "win32" ? "the relay host is Windows only"
  : !existsSync(CLI) ? `${CLI} isn't built: run build.ps1`
  : false;
export const sleep = ms => new Promise(r => setTimeout(r, ms));

// Kills a process and its children.
export function kill(p) {
  if (!p || p.exitCode !== null) return;
  p.removeAllListeners("exit");
  try { execFileSync("taskkill", ["/pid", String(p.pid), "/T", "/F"], { stdio: "ignore" }); } catch { /* gone */ }
}

// The relay on 127.0.0.1:`port` with `timings` ({ staleMs, emptyRoomMs,
// unjoinedRoomMs }). Resolves once it listens; a port still held by the
// last run is tried again for a few seconds.
export async function startHost(port, timings = {}) {
  const args = ["relay-host", "--port", String(port)];
  for (const [k, v] of Object.entries(timings)) args.push("--" + k.replace(/[A-Z]/g, c => "-" + c.toLowerCase()), String(v));
  for (let tries = 0; ; ++tries) {
    const host = spawn(CLI, args, { cwd: REPO });
    let output = "";
    const result = await new Promise(resolve => {
      const timer = setTimeout(() => resolve("timeout"), 15_000);
      const look = d => {
        output += d;
        if (/^relay http/m.test(output)) { clearTimeout(timer); resolve("ok"); }
      };
      host.stdout.on("data", look);
      host.stderr.on("data", look);
      host.on("exit", () => { clearTimeout(timer); resolve("exited"); });
    });
    if (result === "ok") return host;
    kill(host);
    if (tries >= 10) throw new Error(`relay-host didn't start (${result}):\n${output}`);
    await sleep(500);
  }
}
export const stopHost = kill;

export async function newSession(base) {
  const r = await fetch(`${base}/session`, { method: "POST" });
  assert.equal(r.status, 201);
  return (await r.json()).code;
}

// The HTTP status a WebSocket upgrade to `url` gets (101 when accepted).
export function upgradeStatus(url) {
  return new Promise((resolve, reject) => {
    const req = request(url, {
      headers: { Connection: "Upgrade", Upgrade: "websocket", "Sec-WebSocket-Version": "13",
                 "Sec-WebSocket-Key": "dGhlIHNhbXBsZSBub25jZQ==" },
    });
    req.on("response", res => { res.resume(); resolve(res.statusCode); });
    req.on("upgrade", (res, socket) => { socket.destroy(); resolve(101); });
    req.on("error", reject);
    req.end();
  });
}

// A client in room `path` ("/session/K7DP4X" or "/pair/<key>"; a bare code
// means its session): its frames queued as they arrive, and how it closed.
export function joinRoom(port, path) {
  if (!path.startsWith("/")) path = `/session/${path}`;
  const ws = new WebSocket(`ws://127.0.0.1:${port}${path}`);
  ws.binaryType = "arraybuffer";
  const c = { ws, frames: [], waiters: [], closed: null };
  c.opened = new Promise((resolve, reject) => {
    ws.onopen = resolve;
    ws.onerror = () => reject(new Error("couldn't join " + path));
  });
  c.closedP = new Promise(resolve => {
    ws.onclose = e => { c.closed = { code: e.code, reason: e.reason }; resolve(c.closed); };
  });
  ws.onmessage = e => {
    const frame = typeof e.data === "string" ? e.data : new Uint8Array(e.data);
    const w = c.waiters.shift();
    if (w) w(frame); else c.frames.push(frame);
  };
  c.next = (ms = 3000) => c.frames.length ? Promise.resolve(c.frames.shift()) : new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error("no frame within " + ms + " ms")), ms);
    c.waiters.push(f => { clearTimeout(t); resolve(f); });
  });
  return c;
}
