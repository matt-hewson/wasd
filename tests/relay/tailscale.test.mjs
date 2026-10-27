// Co-op over Tailscale on one PC, the way two players set it up: one guide
// hosts the relay (host_relay=1, 127.0.0.1 only) and the other reaches it
// only through this PC's Tailscale address, which `tailscale serve --bg
// --tcp <port> tcp://localhost:<port>` forwards to the relay. Both pair
// automatically by the same Seamless Co-op password.
//
// Opt-in: needs Tailscale running. Set
//   $env:WASD_TAILSCALE_RELAY = "http://<this PC's Tailscale name>:8790"
// (the port must be free for the host guide). If Tailscale isn't forwarding
// that port, the test has the host guide's "Forward it with Tailscale" turn
// it on, and turns it off again at the end.
import { test } from "node:test";
import assert from "node:assert/strict";
import { skip as noExe, sleep } from "./host.mjs";
import { run, until, startGuide, pairKeyOf, fakeGameDir } from "./guide.mjs";

const URL_ = process.env.WASD_TAILSCALE_RELAY || "";
const PORT = URL_ ? Number(new URL(URL_).port || 80) : 0;
const skip = noExe || (!URL_ && "set WASD_TAILSCALE_RELAY to this PC's Tailscale relay address to run it");
const PASSWORD = "Tailscale-test-7";

test("co-op over Tailscale: a hosting guide and one reached through the tailnet", { skip }, async t => {
  const hostGame = fakeGameDir(PASSWORD), partnerGame = fakeGameDir(PASSWORD);
  const hostSettings = { host_relay: 1, relay_port: PORT, live_port: 18785, auto_pair: 1, game_dir: hostGame.dir };
  let host = await startGuide("tshost", hostSettings), partner;
  try {
    partner = await startGuide("tspartner", { relay_url: URL_, live_port: 18795, auto_pair: 1, game_dir: partnerGame.dir });
  } catch (e) {
    await host.stop();  // nothing left running to hold up the test run
    hostGame.remove();
    partnerGame.remove();
    throw e;
  }
  let fake, forwardedHere = false;
  try {
    await t.test("the host's own button forwards the relay's port, if it isn't yet", async () => {
      let c = await until(host.coop, c => c.host.tailscale, "Tailscale read", 15_000);
      assert.ok(c.host.tailscale.running, "Tailscale isn't running: " + JSON.stringify(c.host.tailscale));
      if (!c.host.tailscale.serving) {
        assert.equal((await host.post("/api/coop/forward", "on=1")).ok, true);
        forwardedHere = true;
        c = await until(host.coop, c => c.host.tailscale.serving || c.host.tailscale.forwardError, "forwarded", 15_000);
        assert.deepEqual([c.host.tailscale.serving, c.host.tailscale.forwardError], [true, ""]);
      }
    });

    await t.test("the relay answers on the Tailscale address", async () => {
      assert.equal((await host.coop()).host.running, true);
      assert.equal(await (await fetch(`${URL_}/`)).text(), "WASD co-op relay\n");
    });

    await t.test("the host's popover sees the forward; asking for it again is harmless", async () => {
      let c = await until(host.coop, c => c.host.tailscale, "Tailscale read", 15_000);
      assert.equal(c.host.tailscale.serving, true);
      assert.ok(URL_.includes(c.host.tailscale.name) || URL_.includes(c.host.tailscale.ip), JSON.stringify(c.host.tailscale));
      assert.equal((await host.post("/api/coop/forward", "")).ok, true);
      await sleep(1500);
      c = await host.coop();
      assert.deepEqual([c.host.tailscale.serving, c.host.tailscale.forwardError], [true, ""]);
      assert.equal(await (await fetch(`${URL_}/`)).text(), "WASD co-op relay\n");
    });

    await t.test("both pair by the password, no code", async () => {
      const ok = c => c.state === "connected" && c.paired && c.partner && c.partner.here;
      const [a, b] = await Promise.all([until(host.coop, ok, "the host paired", 20_000),
                                        until(partner.coop, ok, "the partner paired", 20_000)]);
      assert.equal(a.code, "");
      assert.equal(b.code, "");
      const rtt = (await until(partner.coop, c => c.rttMs >= 0, "a round trip over the tailnet")).rttMs;
      t.diagnostic(`partner's round trip to the relay over Tailscale: ${rtt} ms`);
    });

    await t.test("the host closes WASD and comes back: they pair again by themselves", async () => {
      await host.stop();
      await until(partner.coop, c => c.state === "reconnecting", "the partner reconnecting", 15_000);
      await sleep(2000);
      host = await startGuide("tshost", hostSettings);
      await until(partner.coop, c => c.state === "connected" && c.partner && c.partner.here, "paired again", 45_000);
      await until(host.coop, c => c.state === "connected", "the host paired again", 15_000);
    });

    await t.test("state crosses the tailnet: a made-up character's arrives at the host", async () => {
      // The guides have no game, so they send nothing: swap the partner guide for a made-up character.
      assert.equal((await partner.post("/api/coop/session", "do=leave")).ok, true);
      await until(host.coop, c => c.state === "reconnecting", "the host's partner gone", 15_000);
      fake = run(["relay", `pair:${pairKeyOf(PASSWORD)}`, "--sync", "--name", "Isolde", "--url", URL_, "--seconds", "60"]);
      const c = await until(host.coop, c => c.state === "connected" && c.partner && c.partner.have, "Isolde's state", 20_000);
      assert.equal(c.partner.name, "Isolde");
      const hp = c.partner.hp[0];
      await until(host.coop, c => c.partner && c.partner.hp[0] !== hp, "her HP changing", 10_000);
    });
  } finally {
    if (fake) fake.kill();
    if (forwardedHere) {  // as it was: not forwarding
      await host.post("/api/coop/forward", "on=0").catch(() => {});
      await until(host.coop, c => c.host.tailscale && !c.host.tailscale.serving, "forward off again", 15_000).catch(() => {});
    }
    await partner.stop();
    await host.stop();
    hostGame.remove();
    partnerGame.remove();
  }
});
