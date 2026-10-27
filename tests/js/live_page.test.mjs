// The live page's view logic (templates/live.html): derive() turns one
// /api/state payload into what every theme shows. These pin the spoiler
// rules agreed with the user -- boss numbers at Full only, the chart only
// for the closest boss still to fight, "later" bosses apart, item kinds
// from Category -- plus the small text parsers.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { load, definition, scriptOf, ROOT } from "./extract.mjs";

const { derive, parseUpgrade, coopView, coopPopView, coopError } = load("templates/live.html", [
  "esc", "num", "MINUS", "dur", "pct", "ATTR", "ATTR_FULL", "DMG", "DMGS", "STAT", "TIERS", "TIERDESC",
  "parseUpgrade", "coAgo", "coopView", "coopError", "coopPopView", "derive",
]);

// A payload shaped like the real /api/state (High Wall, level 10).
function state(over = {}) {
  return {
    status: "", paused: false, haveValues: true, tier: 2, character: "Ashen One",
    level: 10, souls: 2399, nextLevelCost: 840, hp: [627, 627], fp: [93, 93], stamina: [95, 95],
    attrs: [13, 10, 11, 15, 13, 12, 9, 9, 7], plan: { gain: [], detail: [], best: "" },
    equipLoad: [33.3, 55.0], roll: "medium roll", spell: "", quickItem: "Estus Flask",
    area: { region: "High Wall of Lothric", name: "High Wall of Lothric", bonfire: "Tower on the Wall", mapKey: 60 },
    items: { found: 3, total: 30, scope: "in High Wall of Lothric" }, nearby: [],
    bosses: [], bossesHere: [0, 0, 0],
    progress: { bossesBase: [2, 19], bossesDlc: [0, 6], shards: "Estus 1/11 | Bone 0/10", keys: "1 / 35 key items (1 held)",
                deathsTotal: 0, sessionDeaths: 0, soulsLost: 0, soulsRecovered: 0, soulsPerHour: 0, sessionSeconds: 60, areaSeconds: 30 },
    weapons: [], route: { hint: "", steps: [], hidden: 0 }, missables: { alert: "", hidden: 0, items: [] },
    perf: { read: "", latencyUs: 10, rateHz: 10, cpu: 0.5, memMB: 25 },
    ...over,
  };
}

const absorb = [10, 10, 10, 10, 20, -20, 10, 30];
const vordtDead = (statsLevel) => ({ name: "Vordt of the Boreal Valley", defeated: true, optional: false, nearest: false, human: false,
  later: false, opens: "", statsLevel, dist: -1, weak: statsLevel ? "weak: Dark -27%" : "",
  ...(statsLevel === 2 ? { absorb, resist: [999, 999, 200, 999, 63] } : {}) });
const dancerLater = (name = "") => ({ name, defeated: false, optional: false, nearest: false, human: false, later: true,
  opens: name ? "Opens after 3 of Abyss Watchers / ..." : "Comes later in the game", statsLevel: 0, dist: 40, weak: "" });
const aliveBoss = (statsLevel, over = {}) => ({ name: statsLevel === 2 ? "Curse-rotted Greatwood" : "", defeated: false, optional: true,
  nearest: true, human: false, later: false, opens: "", statsLevel, dist: 85, weak: statsLevel ? "weak: Fire -20%" : "",
  ...(statsLevel === 2 ? { absorb, resist: [999, 999, 200, 999, 63] } : {}), ...over });

test("Category in High Wall after Vordt: list with the Dancer as later, no chart", () => {
  const v = derive(state({ tier: 2, bosses: [vordtDead(0), dancerLater()], bossesHere: [1, 1, 1] }));
  assert.equal(v.show.bossFull, true);
  assert.equal(v.show.bossChart, false);
  assert.equal(v.t.bossNoChart, "No bosses left to fight here for now.");
  const [vordt, dancer] = v.lists.bossList;
  assert.equal(vordt.tag, "Defeated");
  assert.equal(dancer.tag, "Later");
  assert.equal(dancer.name, "A boss for later");
  assert.equal(dancer.dist, "later");
  assert.equal(dancer.title, "Comes later in the game");
  assert.equal(dancer.later, true);
  assert.equal(v.t.bossMetaA, "1 / 1 here (+1 later) · 2 / 19 base");
});

test("Full never charts a defeated boss (user's call, 2026-10-07)", () => {
  const v = derive(state({ tier: 3, bosses: [vordtDead(2), dancerLater("Dancer of the Boreal Valley")], bossesHere: [1, 1, 1] }));
  assert.equal(v.show.bossChart, false);
  assert.equal(v.t.bossNoChart, "No bosses left to fight here for now.");
  assert.equal(v.lists.bossList[1].name, "Dancer of the Boreal Valley");
  assert.match(v.lists.bossList[1].title, /^Opens after 3 of/);
});

test("Full charts the closest boss still to fight", () => {
  const v = derive(state({ tier: 3, bosses: [vordtDead(2), aliveBoss(2)], bossesHere: [1, 2, 0] }));
  assert.equal(v.show.bossChart, true);
  assert.equal(v.t.w1Label, "Weak to");
  assert.equal(v.t.w1, "Fire −20%");
  assert.equal(v.t.s1, "Dark +30%");
  assert.deepEqual(v.lists.resist.map(r => r.val), ["immune", "immune", "200", "immune", "63"]);
  assert.equal(v.t.bossMetaA, "1 / 2 here · 2 / 19 base");
});

test("Category with a boss still to fight: no numbers, says they show at Full", () => {
  const v = derive(state({ tier: 2, bosses: [aliveBoss(0)], bossesHere: [0, 1, 0] }));
  assert.equal(v.show.bossChart, false);
  assert.equal(v.t.bossNoChart, "Weaknesses and resistances show at spoiler tier Full.");
  assert.equal(v.lists.bossList[0].name, "Optional boss");
  assert.equal(v.lists.bossList[0].dist, "85 m");
});

test("a later boss is never picked as the boss ahead", () => {
  const near = { ...dancerLater("Dancer of the Boreal Valley"), statsLevel: 2, absorb, resist: [0, 0, 0, 0, 0] };
  const v = derive(state({ tier: 3, bosses: [near], bossesHere: [0, 0, 1] }));
  assert.equal(v.show.bossChart, false);
});

test("Off and Vague: counts and defeated bosses only", () => {
  for (const tier of [0, 1]) {
    const v = derive(state({ tier, bosses: [vordtDead(0), dancerLater()], bossesHere: [1, 1, 1] }));
    assert.equal(v.show.bossOff, true, `tier ${tier}`);
    assert.deepEqual(v.lists.bossList, []);
    assert.deepEqual(v.lists.bossDead.map(b => b.name), ["Vordt of the Boreal Valley"]);
    assert.equal(v.t.bossOffB, "1 boss in this area (+1 later) · 1 defeated");
    // Vague is not "off": it says where weaknesses show instead.
    assert.equal(v.t.bossOffNote, tier === 0 ? "Guidance is off. Raise the tier to see weaknesses."
      : "Weaknesses and resistances show at spoiler tier Full.");
    assert.equal(v.t.bossOffC, `1 of 1 defeated (+1 later) · ${tier === 0 ? "guidance off" : "weaknesses at Full"}`);
  }
});

test("human-type boss at Full says its defences come from gear", () => {
  const v = derive(state({ tier: 3, bosses: [aliveBoss(2, { human: true, absorb: undefined })], bossesHere: [0, 1, 0] }));
  assert.equal(v.show.bossChart, false);
  assert.match(v.t.bossNoChart, /^Human-type boss/);
});

const items = [
  { found: false, label: "a ring", category: "Ring", clock: "3 o'clock", dist: 21.7, dy: 4, showHeight: true, angle: 1, x: 0, z: 0 },
  { found: false, label: "souls", category: "Souls", clock: "12 o'clock", dist: 40.2, dy: 0.2, showHeight: true, angle: 0, x: 0, z: 0 },
  { found: true, label: "Estus Shard" },
];

test("items: counts only at Off and Vague", () => {
  for (const tier of [0, 1]) {
    const v = derive(state({ tier, nearby: items }));
    assert.deepEqual(v.lists.unfound, [], `tier ${tier}`);
    assert.equal(v.show.itemsBlock, true);
    assert.equal(v.t.remain, "2");
    assert.deepEqual(v.lists.found.map(f => f.label), ["Estus Shard"]);  // found items are always named
  }
});

test("items: Category shows kinds and directions, Full adds the category", () => {
  const cat = derive(state({ tier: 2, nearby: items }));
  assert.equal(cat.lists.unfound[0].meta, "3 o'clock · ↑ 4 m");
  assert.equal(cat.lists.unfound[1].meta, "12 o'clock · level");
  assert.equal(cat.lists.unfound[0].distM, "22 m");
  const full = derive(state({ tier: 3, nearby: items }));
  assert.equal(full.lists.unfound[0].meta, "Ring · 3 o'clock · ↑ 4 m");
});

test("level-up notice and progress tiles", () => {
  const v = derive(state({ souls: 900, nextLevelCost: 840 }));
  assert.equal(v.show.canLevel, true);
  assert.equal(derive(state({ souls: 100, nextLevelCost: 840 })).show.canLevel, false);
  const tiles = Object.fromEntries(v.lists.tiles.map(t => [t.k, t]));
  assert.equal(tiles["Estus shards"].v, "1 / 11");
  assert.equal(tiles["Key items"].v, "1 / 35");
  assert.equal(tiles["Key items"].s, "1 held");
});

test("status pill", () => {
  assert.equal(derive(state()).t.status, "live");
  assert.equal(derive(state({ paused: true })).t.status, "paused");
  assert.equal(derive(state({ haveValues: false, status: "Not in game (main menu or loading) -- waiting." })).t.status, "waiting for game");
});

test("footer shows the app version from the state", () => {
  assert.equal(derive(state({ version: "0.1.0" })).t.version, "WASD 0.1.0");
  assert.equal(derive(state({ version: undefined })).t.version, "");  // an older guide without the field
});

test("every theme's footer has the version slot", () => {
  assert.match(definition(scriptOf("templates/live.html"), "footer"), /data-b="version"/);
});

test("every theme's header opens the results page and has the co-op pill (user's call, 2026-10-26)", () => {
  const src = scriptOf("templates/live.html");
  assert.match(definition(src, "resultsBtn"), /href="\/results"/);
  assert.equal((src.match(/\$\{resultsBtn\(\)\}/g) || []).length, 3);
  assert.equal((src.match(/\$\{coBtn\(\)\}\$\{ovBtn\(\)\}/g) || []).length, 3);  // just before Overlay
  // The partner: a strip in Bonfire and Ledger, a card in Cartographer, a map chip and item line in each.
  assert.equal((src.match(/\$\{pstrip\(\)\}/g) || []).length, 2);
  assert.equal((src.match(/\$\{pcard\(\)\}/g) || []).length, 1);
  assert.equal((src.match(/\$\{pedge\(\)\}/g) || []).length, 3);
  assert.equal((src.match(/\$\{pcount\(\)\}/g) || []).length, 3);
});

// /api/state's "coop" as the guide sends it.
const coop = (over = {}) => ({
  relay: true, host: { on: false, running: false, port: 8790, error: "" }, relayUrl: "http://my-pc.tail1234.ts.net:8790",
  pair: { available: false, paused: false }, state: "none", error: "", errorText: "", code: "", paired: false,
  openSeconds: 0, goneSeconds: 0, rttMs: -1, lastChangeMs: -1, updatesPerSecond: 0, protocol: 1, partnerProtocol: 0,
  partner: null, ...over,
});
const isolde = (over = {}) => ({
  here: true, have: true, name: "Isolde", level: 31, ng: 0, hp: [702, 868], fp: [61, 112], stamina: [104, 118], deaths: 11,
  place: { area: "High Wall of Lothric", region: "High Wall of Lothric", bonfire: "Tower on the Wall", elsewhere: false, beyond: "" },
  sameMap: true, pose: { x: 1, z: 2, facing: 0 }, trail: [[0, 0], [1, 2]],
  itemsHere: { area: "High Wall of Lothric", found: 11, total: 18, mine: 7 }, bosses: [13000800], ...over,
});

test("co-op: no session is the pill alone", () => {
  const v = coopView(undefined, 2);  // an older guide without "coop"
  assert.equal(v.t.coPill, "Co-op");
  assert.equal(v.show.coOn, false);
  assert.equal(v.cls, "");
  assert.equal(coopView(coop({ error: "wrong-code" }), 2).cls, "err");  // the dot turns red after an error
  assert.equal(derive(state()).show.coOn, false);
});

test("co-op: hosting shows the code and placeholders", () => {
  const v = coopView(coop({ state: "hosting", code: "K7DP4X", openSeconds: 42 }), 2);
  assert.equal(v.t.coPill, "K7DP4X · waiting");
  assert.equal(v.cls, "ring");
  assert.equal(v.show.coOn, true);
  assert.equal(v.t.coName, "Waiting for partner…");
  assert.equal(v.t.coHpT, "—");
  assert.equal(v.w.coHp, "0%");
  assert.equal(v.t.coArea, "No partner yet");
  assert.equal(v.t.coArea2, "Share code K7DP4X to pair");
  assert.equal(v.t.coStat, "code K7DP4X");
  const pop = coopPopView(coop({ state: "hosting", code: "K7DP4X", openSeconds: 42 }), 2);
  assert.equal(pop.view, "host");
  assert.equal(pop.title, "Hosting a session");
  assert.equal(pop.age, "open 0:42");
});

test("co-op: connected partner in the strip, the pill and the popover", () => {
  const c = coop({ state: "connected", code: "K7DP4X", rttMs: 38, lastChangeMs: 400, updatesPerSecond: 9.6, partner: isolde() });
  const v = coopView(c, 2);
  assert.equal(v.t.coPill, "Isolde · 38 ms");
  assert.equal(v.cls, "on");
  assert.equal(v.t.coSub, "Partner · SL 31");
  assert.equal(v.t.coHpT, "702 / 868");
  assert.equal(v.t.coArea, "High Wall of Lothric");
  assert.equal(v.t.coArea2, "Rested at Tower on the Wall");
  assert.equal(v.t.coStat, "38 ms · 0.4 s ago");
  assert.equal(v.show.coLive, true);
  assert.equal(v.show.coElse, false);  // same map: the arrow, not the chip
  assert.equal(v.t.coItemsT, "Isolde 11 / 18 in High Wall of Lothric");
  const pop = coopPopView(c, 2);
  assert.equal(pop.view, "conn");
  assert.deepEqual(pop.rows.map(r => r[1]), ["0.4 s ago", "38 ms", "9.6 / s", "1 · matches", "K7DP4X"]);
  assert.equal(pop.connNote, "Your spoiler tier (Category) controls everything you see about Isolde.");
});

test("co-op: a dropped partner dims and counts up, keeping their values", () => {
  const c = coop({ state: "reconnecting", code: "K7DP4X", goneSeconds: 12, lastChangeMs: 12400, partner: isolde({ here: false }) });
  const v = coopView(c, 2);
  assert.equal(v.show.coDim, true);
  assert.equal(v.t.coPill, "Isolde · 12 s");
  assert.equal(v.t.coStat, "reconnecting… 12 s");
  assert.equal(v.t.coHpT, "702 / 868");
  assert.equal(v.mapLabel, "Isolde · last seen 12 s ago");
  const pop = coopPopView(c, 2);
  assert.equal(pop.title, "Partner reconnecting");
  assert.equal(pop.rows[0][1], "reconnecting… 12 s");
  assert.equal(pop.rows[1][1], "12 s ago");
});

test("co-op: where the partner is follows the server's tier filtering", () => {
  const away = place => coop({ state: "connected", partner: isolde({ sameMap: false, pose: null, trail: [], place }) });
  const hidden = { area: "", region: "", bonfire: "", elsewhere: true, beyond: "" };
  let v = coopView(away(hidden), 1);
  assert.equal(v.show.coElse, true);
  assert.equal(v.t.coElseTxt, "is in an area you haven't reached");
  assert.equal(v.t.coArea, "An area you haven't reached");
  assert.equal(v.t.coArea2, "location hidden at Vague");
  v = coopView(away({ ...hidden, beyond: "High Wall of Lothric" }), 2);
  assert.equal(v.t.coElseTxt, "is in a new area beyond High Wall of Lothric");
  v = coopView(away({ area: "Undead Settlement", region: "", bonfire: "", elsewhere: false, beyond: "" }), 3);
  assert.equal(v.t.coElseTxt, "is in Undead Settlement");
});

test("co-op: connected but not in the game yet", () => {
  const v = coopView(coop({ state: "connected", partner: { here: true, have: false } }), 2);
  assert.equal(v.t.coName, "Partner connected");
  assert.equal(v.t.coArea, "Not in the game yet");
  assert.equal(v.show.coItems, false);
});

test("co-op: the partner's bosses and deaths beside yours", () => {
  const vordt = { ...vordtDead(0), flag: 13000800 }, dancer = { ...dancerLater(), flag: 13000890 };
  const v = derive(state({ tier: 2, bosses: [vordt, dancer], bossesHere: [1, 1, 1], coop: coop({ state: "connected", partner: isolde() }) }));
  assert.equal(v.lists.bossList[0].pmark, " · Isolde ✓");
  assert.equal(v.lists.bossList[1].pmark, "");
  assert.equal(v.t.bossMetaA, "1 / 1 here (+1 later) · 2 / 19 base · Isolde 1 / 1");
  const deaths = v.lists.tiles.find(t => t.k === "Deaths");
  assert.equal(deaths.v, "0 / 11");
  assert.equal(deaths.s, "yours / Isolde's");
  // Solo: unchanged.
  assert.equal(derive(state({ bosses: [vordt] })).lists.bossList[0].pmark, "");
});

test("co-op: errors in the popover's words", () => {
  assert.equal(coopError(coop()), null);
  assert.equal(coopError(coop({ error: "wrong-code" })).title, "No session with that code");
  assert.equal(coopError(coop({ error: "room-full" }), "K7DP4X").title, "Session K7DP4X is full");
  assert.equal(coopError(coop({ error: "version", errorText: "your partner's WASD speaks protocol 2; yours speaks 1" })).body,
    "Your partner's WASD speaks protocol 2; yours speaks 1. Update both copies to the same version.");
  assert.match(coopError(coop({ error: "relay" })).body, /then try again/);
  assert.match(coopError(coop({ error: "relay", state: "joining" })).body, /Retrying automatically/);
  assert.equal(coopError(coop({ error: "ended", errorText: "your partner was away for 5 minutes" })).body,
    "Your partner was away for 5 minutes.");
  assert.match(coopError(coop({ error: "relay" })).body, /Tailscale/);
  assert.equal(coopError(coop({ error: "room-full", errorText: "pairing room 9be78da5 already has two players" })).title,
    "Pairing room is full");
  const noRelay = coopPopView(coop({ relay: false, relayUrl: "" }), 2);
  assert.equal(noRelay.canStart, false);
  assert.match(noRelay.soloNote, /relay_url/);
  assert.equal(noRelay.relayNote, "Joining a host? Paste the relay_url line they send you:");
  assert.equal(noRelay.hostBtn, "Host the relay here");
});

test("co-op: pairing by the Seamless password has no code", () => {
  const c = coop({ state: "pairing", paired: true, openSeconds: 5, pair: { available: true, paused: false } });
  const v = coopView(c, 2);
  assert.equal(v.t.coPill, "pairing · waiting");
  assert.equal(v.cls, "ring");
  assert.equal(v.t.coName, "Waiting for partner…");
  assert.equal(v.t.coArea2, "same Seamless password pairs you");
  assert.equal(v.t.coStat, "pairing by password");
  const pop = coopPopView(c, 2);
  assert.equal(pop.view, "host");
  assert.equal(pop.title, "Pairing automatically");
  assert.equal(pop.showCode, false);
  assert.equal(pop.leaveTxt, "Stop pairing");
  assert.match(pop.hostNote, /same Seamless Co-op password/);
  // Connected and then dropped: the room stays open, the session row says how you paired.
  const recon = coopPopView(coop({ state: "reconnecting", paired: true, goneSeconds: 400, partner: isolde({ here: false }) }), 2);
  assert.equal(recon.rows[4][1], "by password");
  assert.match(recon.connNote, /pairing room stays open/);
  // Disconnected: auto pairing is paused, and the popover offers it back.
  const solo = coopPopView(coop({ pair: { available: true, paused: true } }), 2);
  assert.equal(solo.showPair, true);
  assert.match(solo.soloNote, /pair automatically/);
  assert.equal(coopPopView(coop(), 2).showPair, false);
});

test("co-op: where the relay is, in the popover", () => {
  const set = coopPopView(coop(), 2);
  assert.equal(set.relayNote, "Relay: http://my-pc.tail1234.ts.net:8790. To change it, paste another:");
  assert.equal(set.showRelayInput, true);  // the partner pastes the host's line here
  const none = coopPopView(coop({ relay: false, relayUrl: "" }), 2);
  assert.equal(none.relayNote, "Joining a host? Paste the relay_url line they send you:");
  assert.equal(none.showRelayInput, true);
  const hosting = coopPopView(coop({ host: { on: true, running: true, port: 8790, error: "" }, relayUrl: "" }), 2);
  assert.equal(hosting.relayNote, "Relay: this PC, port 8790. Your partner reaches it over Tailscale.");
  assert.equal(hosting.showRelayInput, false);  // hosting: your own relay
  assert.equal(hosting.hostBtn, "Stop hosting the relay");
  const taken = coopPopView(coop({ host: { on: true, running: false, port: 8790, error: "port 8790 is taken" } }), 2);
  assert.equal(taken.relayNote, "Relay: can't host on port 8790 (port 8790 is taken).");
  assert.equal(taken.tsNote, "");  // not hosting after all: nothing about Tailscale
});

test("co-op: hosting, the popover says what Tailscale still needs", () => {
  const hosting = ts => coopPopView(coop({ host: { on: true, running: true, port: 8790, error: "", tailscale: ts }, relayUrl: "" }), 2);
  const on = { installed: true, running: true, name: "my-pc.tail1234.ts.net", ip: "100.101.102.103", serving: true };
  let v = hosting(on);
  assert.equal(v.tsNote, "Your partner's relay_url (settings.ini):");
  assert.equal(v.tsText, "relay_url=http://my-pc.tail1234.ts.net:8790");
  assert.equal(hosting({ ...on, name: "" }).tsText, "relay_url=http://100.101.102.103:8790");  // no MagicDNS name: the address
  // Forwarding: the button turns it off again.
  assert.deepEqual([v.showForward, v.forwardOn, v.forwardBtn], [true, false, "Stop forwarding with Tailscale"]);
  v = hosting({ ...on, forwardError: "access denied" });
  assert.equal(v.tsNote, "Tailscale didn't stop forwarding: access denied. Your partner's relay_url (settings.ini):");
  v = hosting({ ...on, serving: false, forwardError: "" });
  assert.match(v.tsNote, /isn't forwarding/);
  assert.deepEqual([v.showForward, v.forwardOn, v.forwardBtn], [true, true, "Forward it with Tailscale"]);
  assert.equal(v.tsText, "tailscale serve --bg --tcp 8790 tcp://localhost:8790");  // or by hand
  v = hosting({ ...on, serving: false, forwardError: "Serve is not enabled on your tailnet https://login.tailscale.com/f/serve" });
  assert.equal(v.tsNote, "Tailscale didn't forward it: Serve is not enabled on your tailnet https://login.tailscale.com/f/serve. Or run, in a terminal:");
  assert.equal(v.showForward, true);
  v = hosting({ ...on, running: false, serving: false });
  assert.match(v.tsNote, /Tailscale is off/);
  assert.equal(v.tsText, "");
  assert.equal(v.showForward, false);  // nothing to forward with
  assert.match(hosting({ installed: false, running: false, name: "", ip: "", serving: false }).tsNote, /Install Tailscale/);
  assert.equal(hosting(null).tsNote, "");  // not read yet
});


test("parseUpgrade", () => {
  const one = parseUpgrade("+1 -> +2: Titanite Shard 1/2");
  assert.equal(one.from, "+1 → +2");
  assert.equal(one.mat, "Titanite Shard");
  assert.equal(one.val, "1 / 2");
  assert.equal(one.have, 1);
  assert.equal(one.need, 2);
  const two = parseUpgrade("+3 -> +4: Large Titanite Shard 0/1 Titanite Shard 2/3");
  assert.equal(two.mat, "Large Titanite Shard + Titanite Shard");
  assert.equal(two.need, 0);  // pips only for a single material
  assert.equal(parseUpgrade("+10 (max)").text, "+10, fully upgraded");
  assert.equal(parseUpgrade("--").text, "—");
  assert.equal(parseUpgrade("").text, "—");
  assert.equal(parseUpgrade("something else").text, "something else");
});
