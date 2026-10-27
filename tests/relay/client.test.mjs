// WASD's own relay client (src/core/relay_client.cpp), through the built
// `wasd-cli.exe relay` command, against the relay the host's WASD runs
// (`wasd-cli.exe relay-host`). Skipped until build.ps1 has built the exe.
import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { REPO, CLI, skip, startHost, stopHost, newSession, upgradeStatus, joinRoom, sleep } from "./host.mjs";

const PORT = 8798;
const BASE = `http://127.0.0.1:${PORT}`;
const TIMINGS = { staleMs: 1500, emptyRoomMs: 60_000, unjoinedRoomMs: 60_000 };
const KEY = "5e".repeat(32);  // a pairing key

let host;
before(async () => {
  if (!skip) host = await startHost(PORT, TIMINGS);
});
after(() => stopHost(host));

// The relay stops (the host closed WASD) and comes back: its rooms are gone.
async function restartHost() {
  stopHost(host);
  await sleep(300);
  host = await startHost(PORT, TIMINGS);
}

// `wasd-cli relay ...` against this file's relay (cliAt: another one): its
// output lines, waited on one pattern at a time.
const cli = (...args) => cliAt(BASE, ...args);
function cliAt(base, ...args) {
  const p = spawn(CLI, ["relay", ...args, "--url", base], { cwd: REPO });
  const c = { lines: [], out: "", waiters: [], exited: null };
  const feed = d => {
    c.out += d;
    const parts = c.out.split(/\r?\n/);
    c.out = parts.pop();
    for (const line of parts) {
      c.lines.push(line);
      c.waiters = c.waiters.filter(w => !w(line));
    }
  };
  p.stdout.on("data", feed);
  p.stderr.on("data", feed);
  c.exit = new Promise(resolve => p.on("exit", code => { c.exited = code; resolve(code); }));
  // The first line (from now on, or already seen after `from`) matching `re`.
  c.waitFor = (re, ms = 10_000, from = 0) => {
    const seen = c.lines.slice(from).find(l => re.test(l));
    if (seen) return Promise.resolve(seen.match(re));
    return new Promise((resolve, reject) => {
      const t = setTimeout(() => reject(new Error(`no line like ${re} within ${ms} ms:\n${c.lines.join("\n")}`)), ms);
      c.waiters.push(line => {
        const m = line.match(re);
        if (m) { clearTimeout(t); resolve(m); }
        return !!m;
      });
    });
  };
  c.kill = () => { try { p.kill(); } catch { /* gone */ } };
  return c;
}

test("wasd-cli relay", { skip }, async t => {
  await t.test("creates a session, meets its partner and passes frames both ways", async () => {
    const w = cli("create", "--send", "0a0b", "--seconds", "20");
    try {
      const code = (await w.waitFor(/^code (\w{6})$/))[1];
      await w.waitFor(/^connected$/);
      const partner = joinRoom(PORT, code);
      await partner.opened;
      assert.equal(await partner.next(), "partner:joined");
      await w.waitFor(/^partner-joined$/);
      assert.deepEqual(await partner.next(), new Uint8Array([0x0a, 0x0b]));  // --send, once the partner is there
      partner.ws.send(new Uint8Array([1, 2, 3]));
      await w.waitFor(/^frame 010203$/);
      partner.ws.close();
      await w.waitFor(/^partner-left$/);
    } finally {
      w.kill();
    }
  });

  await t.test("says a session has ended, a code is bad, or a room is full, and stops", async () => {
    const ended = cli("ZZZZZZ", "--seconds", "10");  // never created
    await ended.waitFor(/^ended session ZZZZZZ is no longer on the relay$/);
    assert.equal(await ended.exit, 3);

    const bad = cli("AB", "--seconds", "10");
    await bad.waitFor(/^failed /);
    assert.equal(await bad.exit, 2);

    const code = await newSession(BASE);
    const a = joinRoom(PORT, code), b = joinRoom(PORT, code);
    await Promise.all([a.opened, b.opened]);
    const full = cli(code, "--seconds", "10");
    await full.waitFor(/^failed session \w+ already has two players$/);
    assert.equal(await full.exit, 2);
    a.ws.close(); b.ws.close();
  });

  await t.test("its heartbeat keeps its place while a silent partner is replaced", async () => {
    const code = await newSession(BASE);
    const silent = joinRoom(PORT, code);
    await silent.opened;
    const w = cli(code, "--heartbeat", "300", "--seconds", "20");
    try {
      await w.waitFor(/^partner-joined$/);
      await sleep(TIMINGS.staleMs + 1000);
      const from = w.lines.length;
      const replacement = joinRoom(PORT, code);  // takes the silent one's place...
      await replacement.opened;
      // ...but not the client's (straight away, while the replacement is fresh): it's pinging.
      assert.equal(await upgradeStatus(`${BASE}/session/${code}`), 409);
      assert.equal((await silent.closedP).code, 4000);
      await w.waitFor(/^partner-joined$/, 5000, from);  // the replacement, as its new partner
      assert.ok(!w.lines.slice(from).some(l => l.startsWith("disconnected")), w.lines.join("\n"));
      replacement.ws.close();
    } finally {
      w.kill();
    }
  });

  await t.test("joins a pairing room by key, and meets its partner there", async () => {
    const w = cli(`pair:${KEY}`, "--send", "0c", "--seconds", "20");
    try {
      await w.waitFor(/^connected$/);
      assert.ok(!w.lines.some(l => /^code /.test(l)), "a pairing room has no code");
      const partner = joinRoom(PORT, `/pair/${KEY}`);
      await partner.opened;
      await w.waitFor(/^partner-joined$/);
      assert.equal(await partner.next(), "partner:joined");
      assert.deepEqual(await partner.next(), new Uint8Array([0x0c]));
      partner.ws.close();
    } finally {
      w.kill();
    }
  });

  await t.test("after the relay restarts, a session by code has ended; a pairing room comes back", async () => {
    const byCode = cli("create", "--seconds", "60");
    const paired = cli(`pair:${KEY}`, "--seconds", "60");
    try {
      await byCode.waitFor(/^connected$/);
      await paired.waitFor(/^connected$/);
      const from = paired.lines.length;
      await restartHost();
      await byCode.waitFor(/^ended session \w{6} is no longer on the relay$/, 45_000);
      assert.equal(await byCode.exit, 3);
      await paired.waitFor(/^connected$/, 45_000, from);
      const partner = joinRoom(PORT, `/pair/${KEY}`);
      await partner.opened;
      await paired.waitFor(/^partner-joined$/, 10_000, from);
      partner.ws.close();
    } finally {
      byCode.kill();
      paired.kill();
    }
  });
});

// Two copies with live co-op links (`relay ... --sync`: a made-up character
// whose HP drains and refills, who walks and finds pickups).
test("wasd-cli relay --sync", { skip }, async t => {
  // Every "partner-state" line's fields, in order.
  const partnerStates = w => w.lines.filter(l => l.startsWith("partner-state ")).map(l => {
    const m = l.match(/^partner-state seq (\d+) name (\S+) hp (\d+)\/(\d+) found (\d+)\/(\d+)$/);
    return m && { seq: +m[1], name: m[2], hp: +m[3], found: +m[5] };
  });
  const stats = async w => {
    await w.exit;
    const line = w.lines.find(l => l.startsWith("sync sent "));
    const m = line && line.match(/^sync sent (\d+) packets \d+ bytes \((\d+) full, (\d+) deltas, (\d+) requests\), received (\d+) packets \((\d+) full\), rejected (\d+)$/);
    assert.ok(m, w.lines.join("\n"));
    return { sent: +m[1], full: +m[2], deltas: +m[3], requests: +m[4], received: +m[5], fullReceived: +m[6], rejected: +m[7] };
  };

  await t.test("each sees the other's state, and its changes arrive as deltas", async () => {
    const a = cli("create", "--sync", "--name", "Ashen", "--seconds", "8");
    const code = (await a.waitFor(/^code (\w{6})$/))[1];
    const b = cli(code, "--sync", "--name", "Isolde", "--seconds", "6");
    await b.waitFor(/^partner-state seq \d+ name Ashen /);
    await a.waitFor(/^partner-state seq \d+ name Isolde /);
    const [sa, sb] = [await stats(a), await stats(b)];
    const seen = partnerStates(b);
    assert.ok(new Set(seen.map(s => s.hp)).size > 5, "B saw A's HP change: " + seen.map(s => s.hp));
    assert.ok(seen.every((s, i) => i === 0 || s.seq > seen[i - 1].seq), "sequence numbers only go up");
    assert.equal(sa.full, 1);              // one snapshot when the partner arrived...
    assert.ok(sa.deltas > 20, `${sa.deltas} deltas`);  // ...then only changes (10 a second while moving)
    assert.equal(sb.fullReceived, 1);
    assert.equal(sa.rejected + sb.rejected + sa.requests + sb.requests, 0);
  });

  await t.test("an unchanged state sends nothing after the first snapshot", async () => {
    const a = cli("create", "--sync", "--still", "--seconds", "6");
    const code = (await a.waitFor(/^code (\w{6})$/))[1];
    const b = cli(code, "--sync", "--still", "--seconds", "5");
    const [sa, sb] = [await stats(a), await stats(b)];
    assert.deepEqual([sa.sent, sa.full, sa.deltas], [1, 1, 0]);
    assert.deepEqual([sb.sent, sb.full, sb.deltas], [1, 1, 0]);
    assert.equal(sb.received, 1);
  });

  await t.test("after the relay restarts, both send everything again", async () => {
    // A pairing room: made again when they reconnect (a session by code ends).
    const a = cli(`pair:${KEY}`, "--sync", "--name", "Ashen", "--seconds", "45");
    const b = cli(`pair:${KEY}`, "--sync", "--name", "Isolde", "--seconds", "40");
    try {
      await b.waitFor(/^partner-state .* name Ashen /);
      const from = b.lines.length;
      await restartHost();
      await b.waitFor(/^disconnected /, 15_000, from);
      await b.waitFor(/^partner-joined$/, 45_000, from);
      await b.waitFor(/^partner-state .* name Ashen /, 10_000, b.lines.length);
      await sleep(1000);
    } finally {
      a.kill(); b.kill();
    }
    // Killed: no stats line. The counts are covered above; here, a second
    // snapshot arrived (the partner-state lines after the reconnect) and
    // nothing was rejected.
    assert.ok(!b.lines.some(l => /rejected|another protocol/.test(l)), b.lines.join("\n"));
  });
});

// A connection that dies without closing (the host's PC sleeps, the network
// drops): the relay stops answering but keeps every socket open. Each client
// must notice from its heartbeat alone (--dead-ms, 30 s in the app), then
// reconnect to the same session once the relay answers again.
test("a relay that stops answering without closing", { skip }, async t => {
  const STALL_PORT = 8794, STALL_BASE = `http://127.0.0.1:${STALL_PORT}`;
  const stalling = await startHost(STALL_PORT, { staleMs: 1500, stallAfter: 3000, stallFor: 6000 });
  const lines = [];
  stalling.stdout.on("data", d => lines.push(...String(d).split(/\r?\n/)));
  const timing = ["--heartbeat", "300", "--dead-ms", "1500", "--seconds", "40"];
  const a = cliAt(STALL_BASE, "create", ...timing);
  let b;
  try {
    const code = (await a.waitFor(/^code (\w{6})$/))[1];
    b = cliAt(STALL_BASE, code, ...timing);
    await Promise.all([a.waitFor(/^partner-joined$/), b.waitFor(/^partner-joined$/)]);
    const [fromA, fromB] = [a.lines.length, b.lines.length];

    await t.test("both notice the silence from their heartbeat alone", async () => {
      // The relay stalls 3 s after it started; 1.5 s of silence is a dead connection.
      await Promise.all([a.waitFor(/^disconnected nothing heard for 1 s$/, 10_000, fromA),
                         b.waitFor(/^disconnected nothing heard for 1 s$/, 10_000, fromB)]);
      assert.ok(lines.includes("stalled"), lines.join("\n"));
      assert.ok(!lines.includes("resumed"), "noticed while the relay was still silent");
    });

    await t.test("once it answers again, both are back in the same session", async () => {
      await Promise.all([a.waitFor(/^partner-joined$/, 30_000, fromA + 1),
                         b.waitFor(/^partner-joined$/, 30_000, fromB + 1)]);
      assert.ok(lines.includes("resumed"));
      // Nothing ended or failed: the session code still works.
      for (const c of [a, b]) assert.ok(!c.lines.some(l => /^(ended|failed) /.test(l)), c.lines.join("\n"));
    });
  } finally {
    a.kill();
    if (b) b.kill();
    stopHost(stalling);
  }
});
