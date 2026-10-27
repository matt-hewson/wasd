// Shared by the session tests: WASD's own processes with their output
// collected, polling, and the guide (overlay mode) with its own data folder.
import { spawn, execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { cpSync, existsSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { REPO, BIN, CLI, sleep } from "./host.mjs";

// A process with its output collected as lines.
export function run(args, env = {}) {
  const p = spawn(CLI, args, { cwd: REPO, env: { ...process.env, ...env } });
  const c = { p, lines: [], out: "" };
  const feed = d => {
    c.out += d;
    const parts = c.out.split(/\r?\n/);
    c.out = parts.pop();
    c.lines.push(...parts);
  };
  p.stdout.on("data", feed);
  p.stderr.on("data", feed);
  c.kill = () => {
    try { execFileSync("taskkill", ["/pid", String(p.pid), "/T", "/F"], { stdio: "ignore" }); } catch { /* gone */ }
  };
  return c;
}

// Polls `get` until `ok(value)`; the last value on timeout, in the error.
export async function until(get, ok, what, ms = 15_000) {
  const end = Date.now() + ms;
  let last;
  while (Date.now() < end) {
    try { last = await get(); if (ok(last)) return last; } catch (e) { last = e.message; }
    await sleep(150);
  }
  throw new Error(`${what}: not within ${ms} ms; last ${JSON.stringify(last)}`);
}

// The guide in overlay mode with its own data folder and `settings`; its
// live page's address, and calls to it.
export async function startGuide(name, settings) {
  const userDir = mkdtempSync(join(tmpdir(), `wasd-${name}-`));
  writeFileSync(join(userDir, "settings.ini"), Object.entries(settings).map(([k, v]) => `${k}=${v}\n`).join(""));
  // A build outside the checkout (WASD_BIN_DIR) runs as installed, with its map data in the data folder. Give it
  // the checkout's, as after an installed copy's first run; otherwise its first-run step would find the real
  // DS3, read it, and replace a test's made-up game_dir with the real one.
  if (BIN !== join(REPO, "build") && existsSync(join(REPO, "data", "generated")))
    cpSync(join(REPO, "data", "generated"), join(userDir, "generated"), { recursive: true });
  const guide = run(["overlay"], { WASD_USER_DIR: userDir, WASD_INSTANCE: name, WASD_NO_BROWSER: "1" });
  // Up to a minute: when its map data is older than the game's files, the guide reads it again (about 20 s)
  // before the live page starts. A guide that never gets there is killed, so the test run can still end.
  let m;
  try {
    m = await until(async () => guide.lines.map(l => /Live page: (http:\/\/localhost:\d+)\//.exec(l)).find(Boolean),
                    Boolean, `${name}'s live page`, 60_000);
  } catch (e) {
    guide.kill();
    rmSync(userDir, { recursive: true, force: true });
    throw new Error(`${e.message}\n${name}'s output:\n${guide.lines.join("\n")}`);
  }
  const page = m[1];
  return {
    guide, userDir, page,
    coop: async () => (await (await fetch(`${page}/api/state`)).json()).coop,
    post: async (path, params) => (await fetch(`${page}${path}?${params}`, { method: "POST" })).json(),
    async stop() {
      try { await fetch(`${page}/api/quit`, { method: "POST" }); } catch { /* already gone */ }
      await Promise.race([new Promise(r => guide.p.on("exit", r)), sleep(5000)]);
      guide.kill();
      rmSync(userDir, { recursive: true, force: true });
    },
  };
}

// The pairing key WASD makes from a Seamless Co-op password (CoopPairKey).
export const pairKeyOf = pw => createHash("sha256").update("wasd-coop-pair-1\n" + pw).digest("hex");

// A made-up DS3 Game folder holding only Seamless Co-op's settings, with
// `password`; setPassword changes it. Remove it with remove().
export function fakeGameDir(password) {
  const dir = mkdtempSync(join(tmpdir(), "wasd-pair-game-"));
  const setPassword = pw => {
    mkdirSync(join(dir, "SeamlessCoop"), { recursive: true });
    writeFileSync(join(dir, "SeamlessCoop", "ds3sc_settings.ini"),
                  `[GAMEPLAY]\r\nallow_invaders = 1\r\n\r\n[PASSWORD]\r\n; Session password\r\ncooppassword = ${pw}\r\n`);
  };
  setPassword(password);
  return { dir, setPassword, remove: () => rmSync(dir, { recursive: true, force: true }) };
}
