// The live page's co-op session end to end, without the game: the built
// `wasd-cli.exe overlay` (the guide, with its own data folder) driven
// through the page's own HTTP calls, and made-up partners from
// `wasd-cli.exe relay ... --sync`. The first guide hosts the relay itself
// (host_relay=1); the second pairs automatically by a Seamless Co-op
// password, through a relay run on its own (`relay-host`).
// Skipped until build.ps1 has built the exe.
import { test } from "node:test";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import assert from "node:assert/strict";
import { skip, startHost, stopHost, sleep } from "./host.mjs";
import { run, until, startGuide, pairKeyOf, fakeGameDir } from "./guide.mjs";

const HOSTED_PORT = 8799, PAIR_PORT = 8796;

test("the live page's co-op session, on the relay this guide hosts", { skip }, async t => {
  const BASE = `http://127.0.0.1:${HOSTED_PORT}`;
  const g = await startGuide("sessiontest", { host_relay: 1, relay_port: HOSTED_PORT, live_port: 18765, auto_pair: 0 });
  const session = params => g.post("/api/coop/session", params);
  try {
    await t.test("no session to start with; the relay is hosted here", async () => {
      const c = await g.coop();
      assert.equal(c.state, "none");
      assert.equal(c.relay, true);
      const { on, running, port, error } = c.host;
      assert.deepEqual({ on, running, port, error }, { on: true, running: true, port: HOSTED_PORT, error: "" });
      assert.equal(c.partner, null);
      assert.equal(await (await fetch(`${BASE}/`)).text(), "WASD co-op relay\n");
    });

    await t.test("a code that can't be one is refused at once", async () => {
      const r = await session("do=join&code=AB");
      assert.equal(r.ok, false);
      assert.match(r.error, /6 letters and digits/);
    });

    await t.test("a code with no session behind it says so", async () => {
      assert.equal((await session("do=join&code=ZZZZZZ")).ok, true);
      const c = await until(g.coop, c => c.error, "an error");
      assert.equal(c.state, "none");
      assert.equal(c.error, "wrong-code");
    });

    await t.test("create, a partner joins, their state arrives, they drop, you leave", async () => {
      assert.equal((await session("do=create")).ok, true);
      let c = await until(g.coop, c => c.state === "hosting" && c.code, "hosting with a code");
      assert.match(c.code, /^[A-Z0-9]{6}$/);
      assert.equal(c.error, "");  // the last session's error is gone

      const partner = run(["relay", c.code, "--sync", "--name", "Isolde", "--url", BASE, "--seconds", "60"]);
      try {
        c = await until(g.coop, c => c.state === "connected" && c.partner && c.partner.have, "the partner's state");
        assert.equal(c.partner.name, "Isolde");
        assert.equal(c.partner.level, 30);
        assert.equal(c.partner.hp[1], 500);
        assert.equal(c.partner.deaths, 0);
        c = await until(g.coop, c => c.rttMs >= 0, "a round trip");
        // It keeps changing (HP drains): the page sees new values.
        const hp = c.partner.hp[0];
        await until(g.coop, c => c.partner && c.partner.hp[0] !== hp, "HP changing");
        // The results page compares the live partner.
        const r = await (await fetch(`${g.page}/api/coop`)).json();
        assert.equal(r.live, true);
        assert.equal(r.loaded, true);
        assert.equal(r.partner.name, "Isolde");
      } finally {
        partner.kill();
      }
      c = await until(g.coop, c => c.state === "reconnecting", "reconnecting after the partner drops");
      assert.equal(c.partner.have, true);  // their last values stay
      assert.equal(c.partner.name, "Isolde");

      assert.equal((await session("do=leave")).ok, true);
      c = await until(g.coop, c => c.state === "none", "no session after leaving");
      assert.equal(c.partner, null);
      assert.equal(c.error, "");
      assert.equal((await (await fetch(`${g.page}/api/coop`)).json()).live, false);
    });

    await t.test("join a partner's session by code", async () => {
      const partner = run(["relay", "create", "--sync", "--still", "--name", "Corwen", "--url", BASE, "--seconds", "60"]);
      try {
        const code = (await until(async () => partner.lines.map(l => /^code ([A-Z0-9]{6})$/.exec(l)).find(Boolean),
                                  Boolean, "the partner's code"))[1];
        assert.equal((await session(`do=join&code=${code.toLowerCase()}`)).ok, true);  // typed in lower case
        const c = await until(g.coop, c => c.state === "connected" && c.partner && c.partner.have, "connected");
        assert.equal(c.code, code);
        assert.equal(c.partner.name, "Corwen");
        assert.equal((await session("do=leave")).ok, true);
        await until(g.coop, c => c.state === "none", "left");
      } finally {
        partner.kill();
      }
    });

    await t.test("stop hosting: no relay; host again", async () => {
      assert.equal((await g.post("/api/coop/host", "on=0")).ok, true);
      let c = await g.coop();
      assert.equal(c.host.on, false);
      assert.equal(c.relay, false);
      await assert.rejects(fetch(`${BASE}/`));  // nothing listens any more
      const r = await session("do=create");
      assert.equal(r.ok, false);
      assert.match(r.error, /no relay/);
      assert.equal((await g.post("/api/coop/host", "on=1")).ok, true);
      c = await g.coop();
      assert.deepEqual([c.host.on, c.host.running, c.relay], [true, true, true]);
      assert.equal((await fetch(`${BASE}/`)).status, 200);
    });
  } finally {
    await g.stop();
  }
});

test("auto pairing by the Seamless Co-op password", { skip }, async t => {
  const BASE = `http://127.0.0.1:${PAIR_PORT}`;
  const game = fakeGameDir("Ashen-42");
  const relay = await startHost(PAIR_PORT);
  // No relay_url: the partner pastes the host's line into the page, as a player would.
  let g;
  try {
    g = await startGuide("pairtest", { live_port: 18775, auto_pair: 1, game_dir: game.dir });
  } catch (e) {
    stopHost(relay);  // nothing left running to hold up the test run
    game.remove();
    throw e;
  }
  const partner = run(["relay", `pair:${pairKeyOf("Ashen-42")}`, "--sync", "--name", "Isolde", "--url", BASE, "--seconds", "90"]);
  try {
    await t.test("no relay yet: the host's relay_url line pasted into the page sets it", async () => {
      let c = await g.coop();
      assert.deepEqual([c.relay, c.relayUrl, c.state], [false, "", "none"]);
      let r = await g.post("/api/coop/relay", `url=${encodeURIComponent("ws://nope")}`);
      assert.equal(r.ok, false);
      assert.match(r.error, /relay_url line/);
      r = await g.post("/api/coop/relay", `url=${encodeURIComponent(` relay_url=${BASE} `)}`);
      assert.equal(r.ok, true);
      c = await g.coop();
      assert.deepEqual([c.relay, c.relayUrl], [true, BASE]);
      assert.match(readFileSync(join(g.userDir, "settings.ini"), "utf8"), new RegExp(`^relay_url=${BASE}\r?$`, "m"));  // saved
    });

    await t.test("with no session, it waits in the pairing room: no code", async () => {
      const c = await until(g.coop, c => c.pair.available && (c.state === "pairing" || c.state === "connected"), "pairing");
      assert.equal(c.paired, true);
      assert.equal(c.code, "");
      assert.equal(c.pair.paused, false);
    });

    await t.test("a partner with the same password is met there", async () => {
      const c = await until(g.coop, c => c.state === "connected" && c.partner && c.partner.have, "the partner");
      assert.equal(c.partner.name, "Isolde");
      assert.equal(c.paired, true);
    });

    await t.test("Disconnect pauses auto pairing; Pair automatically resumes it", async () => {
      assert.equal((await g.post("/api/coop/session", "do=leave")).ok, true);
      let c = await until(g.coop, c => c.state === "none", "left");
      assert.equal(c.pair.paused, true);
      await sleep(6000);  // longer than the 5 s check: it stays out
      c = await g.coop();
      assert.equal(c.state, "none");
      assert.equal((await g.post("/api/coop/session", "do=pair")).ok, true);
      c = await until(g.coop, c => c.state === "connected" && c.partner && c.partner.have, "paired again");
      assert.equal(c.pair.paused, false);
    });

    await t.test("a changed password moves it to that password's room", async () => {
      game.setPassword("Lothric-7");
      const c = await until(g.coop, c => c.state === "pairing", "alone in the new room", 15_000);
      assert.equal(c.partner, null);
    });
  } finally {
    partner.kill();
    await g.stop();
    stopHost(relay);
    game.remove();
  }
});
