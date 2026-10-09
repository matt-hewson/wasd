// The live page's view logic (templates/live.html): derive() turns one
// /api/state payload into what every theme shows. These pin the spoiler
// rules agreed with the user -- boss numbers at Full only, the chart only
// for the closest boss still to fight, "later" bosses apart, item kinds
// from Category -- plus the small text parsers.
import { test } from "node:test";
import assert from "node:assert/strict";
import { load, definition, scriptOf } from "./extract.mjs";

const { derive, vagueLine, parseUpgrade } = load("templates/live.html", [
  "esc", "num", "MINUS", "dur", "pct", "ATTR", "ATTR_FULL", "DMG", "DMGS", "STAT", "TIERDESC",
  "vagueLine", "parseUpgrade", "derive",
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
  assert.equal(v.t.vagueLine, "");
});

test("Off and Vague: counts and defeated bosses only", () => {
  for (const tier of [0, 1]) {
    const v = derive(state({ tier, bosses: [vordtDead(0), dancerLater()], bossesHere: [1, 1, 1] }));
    assert.equal(v.show.bossOff, true, `tier ${tier}`);
    assert.deepEqual(v.lists.bossList, []);
    assert.deepEqual(v.lists.bossDead.map(b => b.name), ["Vordt of the Boreal Valley"]);
    assert.equal(v.t.bossOffB, "1 boss in this area (+1 later) · 1 defeated");
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

test("vagueLine", () => {
  assert.equal(vagueLine(null), "");
  assert.equal(vagueLine({ weak: "weak: Fire -25%, Strike -10%" }), "The boss ahead is weak to Fire and Strike.");
  assert.equal(vagueLine({ weak: "weak: Fire -25%, Strike -10%, Dark -5%" }), "The boss ahead is weak to Fire, Strike and Dark.");
  assert.equal(vagueLine({ weak: "least resisted: Lightning 5%" }), "The boss ahead has no weakness; Lightning does best.");
  assert.equal(vagueLine({ human: true }), "The boss ahead is human-type: its defences come from its gear.");
  assert.equal(vagueLine({ weak: "" }), "A boss is still alive in this area.");
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
