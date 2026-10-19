// The results page's data logic (templates/results_template.html):
// summarise() -- trail runs per area, warp attribution, time per area,
// sessions in order -- each section's model (tiles, bars per area, map
// areas, soul level, timeline, when you play, compare two sessions, the
// sessions table, tool performance), and the co-op status line and
// comparison model.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { load, definition, scriptOf, ROOT } from "./extract.mjs";

const { summarise, fmtDur, fmtHM, tilesModel, areaBars, mapAreasModel, levelModel, timelineModel, heatModel, compareModel,
        sessionRowsModel, perfModel, bossModel, recordsModel, gearModel, copiedText, coopStatus, coopModel,
        partnerSummary, partnerBossModel, togetherModel, pairTiles, pairAreaBars, pairBossRows, pairRecords, jointTimelineModel,
        whoFoundModel, pairSessionRows } = load("templates/results_template.html",
  ["sessionStart", "summarise", "fmtInt", "fmtDur", "pad2", "fmtHM", "fmtClock", "fmtDate", "fmtDow", "fmtTime", "AREA_HUES", "areaColor",
   "tilesModel", "areaBars", "mapAreasModel", "bossModel", "recordsModel", "gearModel", "levelModel", "timelineModel", "heatModel",
   "compareModel", "sessionRowsModel", "perfModel",
   "ago", "ATTRS", "KEY_KINDS", "copiedText", "coopStatus", "coopModel",
   "PARTNER_BOSS", "PARTNER_WEAPON", "partnerSummary", "partnerBossModel", "myCodeSession", "togetherModel", "pairTiles",
   "pairAreaBars", "pairBossRows", "pairRecords", "jointTimelineModel", "whoFoundModel", "pairSessionRows"]);

const session = events => ({ file: "s.jsonl", events: [{ type: "start", t: 0, character: "Test", level: 10 }, ...events] });
const pos = (t, x, y, z) => ({ type: "pos", t, x, y, z });
const runsIn = (S, area) => (S.trailsByArea.get(area) || []).map(run => run.map(p => p.t));

test("a walked trail is one run in its area", () => {
  const S = summarise([session([{ type: "enter", t: 0, area: "A" }, pos(1, 0, 0, 0), pos(2, 1, 0, 0), pos(3, 2, 0, 0)])]);
  assert.deepEqual(runsIn(S, "A"), [[1, 2, 3]]);
});

test("crossing an area border on foot starts a new run in the new area", () => {
  const S = summarise([session([
    { type: "enter", t: 0, area: "A" }, pos(1, 0, 0, 0), pos(2, 1, 0, 0),
    { type: "enter", t: 2.5, area: "B" }, pos(3, 2, 0, 0), pos(4, 3, 0, 0),
  ])]);
  assert.deepEqual(runsIn(S, "A"), [[1, 2]]);
  assert.deepEqual(runsIn(S, "B"), [[3, 4]]);
});

test("after a warp, points before the area change belong to the new area", () => {
  // Found 2026-10-07: the position moves a few seconds before the area name,
  // so one stray point per warp landed in the old area and doubled its map.
  const S = summarise([session([
    { type: "enter", t: 0, area: "Firelink Shrine" }, pos(1, 270, -56, 606), pos(2, 271, -56, 606),
    pos(20, -16.7, 41.2, -13.1),               // warped: far away, area not updated yet
    { type: "enter", t: 23, area: "High Wall of Lothric" },
    { type: "bonfire", t: 23, name: "High Wall of Lothric", x: -16.7, y: 41.2, z: -13.1 },
    pos(24, -15.7, 41.2, -13.1),
  ])]);
  assert.deepEqual(runsIn(S, "Firelink Shrine"), [[1, 2]]);
  assert.deepEqual(runsIn(S, "High Wall of Lothric"), [[20, 24]]);  // one run, continued across the enter
  assert.equal(S.bonfiresByArea.get("High Wall of Lothric").length, 1);
  assert.equal(S.bonfiresByArea.get("Firelink Shrine"), undefined);
});

test("a jump with no area change soon after stays in the area (respawn at a bonfire)", () => {
  const S = summarise([session([
    { type: "enter", t: 0, area: "A" }, pos(1, 0, 0, 0), pos(2, 1, 0, 0),
    pos(40, 300, 0, 0), pos(41, 301, 0, 0),
    { type: "enter", t: 60, area: "B" },       // much later: a separate change
  ])]);
  assert.deepEqual(runsIn(S, "A"), [[1, 2], [40, 41]]);
  assert.deepEqual(runsIn(S, "B"), []);
});

test("time per area comes from the enter events", () => {
  const S = summarise([session([
    { type: "enter", t: 0, area: "A" }, { type: "enter", t: 100, area: "B" }, { type: "enter", t: 160, area: "A" },
    { type: "end", t: 200, seconds: 200, gained: 0, level: 10, deathsTotal: 0, playTimeMs: 1 },
  ])]);
  assert.equal(S.areaTime.get("A"), 140);
  assert.equal(S.areaTime.get("B"), 60);
});

test("deaths are counted per area with souls lost", () => {
  const S = summarise([session([
    { type: "enter", t: 0, area: "A" },
    { type: "death", t: 5, area: "A", x: 0, y: 0, z: 0, soulsLost: 300, deathsTotal: 4 },
    { type: "bloodstain", t: 9, souls: 300 },
    { type: "death", t: 12, area: "A", x: 0, y: 0, z: 0, soulsLost: 0, deathsTotal: 5 },
  ])]);
  assert.equal(S.deaths, 2);
  assert.equal(S.areaDeaths.get("A"), 2);
  assert.equal(S.lost, 300);
  assert.equal(S.recovered, 300);
});

test("fmtDur", () => {
  assert.equal(fmtDur(0), "0m 00s");
  assert.match(fmtDur(3725), /^1h 02m/);
});

// Two sessions of one character, a day apart (local times), the older given second.
const at = (startedAt, events, extra = {}) => ({ file: "x.jsonl", ...extra, events: [{ type: "start", t: 0, character: "Ashen One", level: 10, startedAt }, ...events] });
const two = () => [
  at("2026-10-06T21:30:00", [
    { type: "enter", t: 0, area: "High Wall of Lothric" },
    { type: "death", t: 50, area: "High Wall of Lothric", x: 0, y: 0, z: 0, soulsLost: 100 },
    { type: "sample", t: 60, level: 11, playTimeMs: 7200000, deathsTotal: 3 },
    { type: "enter", t: 1800, area: "Undead Settlement" },
    { type: "end", t: 5400, seconds: 5400, gained: 9000, level: 13, deathsTotal: 4, playTimeMs: 12600000 },
  ]),
  at("2026-10-05T20:00:00", [
    { type: "enter", t: 0, area: "Cemetery of Ash" },
    { type: "enter", t: 600, area: "Firelink Shrine" },
    { type: "death", t: 650, area: "Firelink Shrine", x: 0, y: 0, z: 0, soulsLost: 50 },
    { type: "death", t: 700, area: "Firelink Shrine", x: 0, y: 0, z: 0, soulsLost: 0 },
    { type: "bloodstain", t: 720, souls: 50 },
    { type: "end", t: 1800, seconds: 1800, gained: 1200, level: 10, deathsTotal: 2, playTimeMs: 1800000 },
  ]),
];

test("sessions are numbered oldest first, with their own figures", () => {
  const S = summarise(two());
  assert.deepEqual(S.sessions.map(s => [s.n, s.dur, s.deaths, s.gained, s.levelStart, s.levelEnd]),
    [[1, 1800, 2, 1200, 10, 10], [2, 5400, 1, 9000, 10, 13]]);
  assert.deepEqual(S.rows.map(s => s.n), [2, 1]);  // the table: newest first
  assert.deepEqual(S.areaOrder, ["Cemetery of Ash", "Firelink Shrine", "High Wall of Lothric", "Undead Settlement"]);
  assert.deepEqual(S.sessions[1].segs, [{ area: "High Wall of Lothric", sec: 1800 }, { area: "Undead Settlement", sec: 3600 }]);
  // Soul level against recorded hours: S2's points come after S1's half hour.
  assert.deepEqual(S.levelRec.map(p => [+p.h.toFixed(3), p.level]), [[0, 10], [0.5, 10], [0.5, 10], [0.517, 11], [2, 13]]);
});

test("the start time falls back to the log's file name", () => {
  const S = summarise([{ file: "20261006-172855.jsonl", events: [{ type: "start", t: 0, character: "A" }] }]);
  const d = S.sessions[0].start;
  assert.deepEqual([d.getFullYear(), d.getMonth(), d.getDate(), d.getHours(), d.getMinutes()], [2026, 9, 6, 17, 28]);
});

test("fmtHM", () => {
  assert.equal(fmtHM(45), "45 s");
  assert.equal(fmtHM(48 * 60), "48 m");
  assert.equal(fmtHM(27 * 3600 + 48 * 60), "27 h 48 m");
});

test("tiles: totals, per hour, longest session, deadliest area", () => {
  const t = Object.fromEntries(tilesModel(summarise(two())).map(x => [x.k, x]));
  assert.equal(t["Time recorded"].v, "2 h 00 m");
  assert.equal(t["Time recorded"].s, "of 3 h 30 m in game");
  assert.equal(t["Deaths"].v, "3");
  assert.equal(t["Deaths"].s, "4 across the playthrough");
  assert.equal(t["Deaths per hour"].v, "1.5");
  assert.equal(t["Souls lost"].s, "50 recovered (33%)");
  assert.equal(t["Souls gained"].s, "5,100 per hour");
  assert.equal(t["Longest session"].v, "1:30");
  assert.match(t["Longest session"].s, /^S2 · .+ · High Wall of Lothric$/);
  assert.equal(t["Deadliest area"].v, "Firelink Shrine");
  assert.equal(t["Deadliest area"].s, "2 deaths there");
  const none = Object.fromEntries(tilesModel(summarise([])).map(x => [x.k, x]));
  assert.equal(none["Deadliest area"].v, "None yet");
  assert.equal(none["Deaths per hour"].v, "–");
});

test("bars per area: both lists in the order first visited", () => {
  const b = areaBars(summarise(two()));
  assert.deepEqual(b.time.map(r => [r.name, r.label]),
    [["Cemetery of Ash", "10 m"], ["Firelink Shrine", "20 m"], ["High Wall of Lothric", "30 m"], ["Undead Settlement", "1 h 00 m"]]);
  assert.equal(b.time[3].w, 1);
  assert.deepEqual(b.deaths.map(r => [r.name, r.v, r.w]),
    [["Cemetery of Ash", 0, 0], ["Firelink Shrine", 2, 1], ["High Wall of Lothric", 1, 0.5], ["Undead Settlement", 0, 0]]);
});

test("map areas: only areas with something to draw, first visited first", () => {
  const S = summarise([session([{ type: "enter", t: 0, area: "A" }, { type: "enter", t: 5, area: "B" }, pos(6, 0, 0, 0), pos(7, 1, 0, 0),
    { type: "death", t: 8, area: "B", x: 1, y: 0, z: 0, soulsLost: 1 }])]);
  assert.deepEqual(mapAreasModel(S).map(a => [a.name, a.deaths]), [["B", 1]]);
});

test("soul level: range, ticks and the ends", () => {
  const m = levelModel(summarise(two()));
  assert.equal(m.first, 10);
  assert.equal(m.last, 13);
  assert.deepEqual([m.y0, m.y1], [8, 14]);
  assert.equal(m.x1, 2);
  assert.deepEqual(m.xTicks, [0, 0.5, 1, 1.5, 2]);
  assert.equal(levelModel(summarise([])), null);
});

test("timeline: stretches per area as shares of the recorded time", () => {
  const m = timelineModel(summarise(two()));
  assert.equal(m.total, 7200);
  assert.deepEqual(m.segs.map(g => [g.area, +g.left.toFixed(4), +g.w.toFixed(4), g.first]),
    [["Cemetery of Ash", 0, 0.0833, true], ["Firelink Shrine", 0.0833, 0.1667, false],
     ["High Wall of Lothric", 0.25, 0.25, true], ["Undead Settlement", 0.5, 0.5, false]]);
  assert.deepEqual(m.ticks.map(t => [t.label, t.left]), [["S1", 0], ["S2", 0.25]]);
  assert.equal(m.segs[0].color, "oklch(0.58 0.075 70)");
  assert.deepEqual(m.legend.map(l => [l.area, l.time]).slice(-1), [["Undead Settlement", "1 h 00 m"]]);
});

test("when you play: minutes by weekday and hour, across midnight", () => {
  const m = heatModel(summarise(two()));
  // S1: Monday 5 Oct 2026, 20:00-20:30. S2: Tuesday, 21:30-23:00.
  assert.equal(Math.round(m.grid[0][20]), 30);
  assert.equal(Math.round(m.grid[1][21]), 30);
  assert.equal(Math.round(m.grid[1][22]), 60);
  assert.equal(Math.round(m.grid[1][23]), 0);
  assert.equal(Math.round(m.grid.flat().reduce((a, b) => a + b, 0)), 120);
  assert.equal(m.sum, "Most often Tue 22:00 · typical start 21:30 · latest finish 23:00 · 0% at weekends");
  assert.equal(heatModel(summarise([])), null);
});

test("compare two sessions: values and deltas, fewer deaths is better", () => {
  const S = summarise(two());
  const rows = Object.fromEntries(compareModel(S.sessions, 1, 2).map(r => [r.k, r]));
  assert.equal(rows["Areas"].b, "High Wall of Lothric, Undead Settlement");
  assert.deepEqual([rows["Length"].a, rows["Length"].b, rows["Length"].d, rows["Length"].tone], ["30 m", "1 h 30 m", "+1 h 00 m", "good"]);
  assert.deepEqual([rows["Deaths"].d, rows["Deaths"].tone], ["−1", "good"]);
  assert.deepEqual([rows["Deaths per hour"].a, rows["Deaths per hour"].b, rows["Deaths per hour"].tone], ["4.0", "0.7", "good"]);
  assert.deepEqual([rows["Levels gained"].b, rows["Levels gained"].d], ["SL 10 → 13", "+3"]);
  assert.equal(compareModel(S.sessions, 1, 1).find(r => r.k === "Deaths").d, "±0");
  assert.equal(compareModel(S.sessions, 1, 9), null);
});

test("sessions table: newest first", () => {
  const rows = sessionRowsModel(summarise(two()));
  assert.deepEqual(rows.map(r => [r.n, r.start, r.length, r.areas, r.deaths, r.level]),
    [["S2", "21:30", "1:30", "High Wall of Lothric → Undead Settlement", "1", "10 → 13"],
     ["S1", "20:00", "0:30", "Cemetery of Ash → Firelink Shrine", "2", "10 → 10"]]);
});

// Sessions with boss kills, attempts and weapons (the session log's boss,
// death.boss and gear events).
const fights = () => [
  at("2026-10-05T20:00:00", [
    { type: "gear", t: 0, slot: "R", id: 2010000, name: "Long Sword", upgrade: 0 },
    { type: "gear", t: 0, slot: "L", id: 21100000, name: "Knight Shield", upgrade: 0 },
    { type: "enter", t: 0, area: "Cemetery of Ash" },
    { type: "boss", t: 300, flag: 14000800, name: "Iudex Gundyr", area: "Cemetery of Ash", playTimeMs: 1 },
    { type: "enter", t: 600, area: "High Wall of Lothric" },
    { type: "death", t: 1200, area: "High Wall of Lothric", x: 0, y: 0, z: 0, soulsLost: 0, boss: 13000800, bossName: "Vordt of the Boreal Valley" },
    { type: "gear", t: 1500, slot: "R", id: 2010003, name: "Long Sword", upgrade: 3 },
    { type: "end", t: 3600, seconds: 3600, gained: 2000, level: 12, deathsTotal: 1, playTimeMs: 3600000 },
  ]),
  at("2026-10-06T20:00:00", [
    { type: "gear", t: 0, slot: "R", id: 2010003, name: "Long Sword", upgrade: 3 },
    { type: "enter", t: 0, area: "High Wall of Lothric" },
    { type: "death", t: 100, area: "High Wall of Lothric", x: 0, y: 0, z: 0, soulsLost: 0, boss: 13000800, bossName: "Vordt of the Boreal Valley" },
    { type: "boss", t: 400, flag: 13000800, name: "Vordt of the Boreal Valley", area: "High Wall of Lothric", playTimeMs: 2 },
    { type: "death", t: 500, area: "Undead Settlement", x: 0, y: 0, z: 0, soulsLost: 0, boss: 13100800, bossName: "Curse-rotted Greatwood" },
    { type: "gear", t: 600, slot: "R", id: 3010000, name: "Claymore", upgrade: 0 },
    { type: "end", t: 1200, seconds: 1200, gained: 800, level: 14, deathsTotal: 3, playTimeMs: 4800000 },
  ]),
];

test("bosses: kills in order, deaths before each, time to kill, attempts not yet killed", () => {
  const S = summarise(fights());
  const m = bossModel(S, 2);
  assert.deepEqual(m.rows.map(r => [r.name, r.killed, r.deaths, r.ttk]),
    [["Iudex Gundyr", true, 0, null], ["Vordt of the Boreal Valley", true, 2, 3600 + 400 - 1200], ["A boss in Undead Settlement", false, 1, null]]);
  assert.match(m.rows[1].session, /^S2 · /);
  assert.equal(m.meta, "2 bosses killed · 3 deaths at bosses");
  // At Full, a boss you've only died near is named.
  assert.equal(bossModel(S, 3).rows[2].name, "Curse-rotted Greatwood");
  assert.deepEqual(bossModel(summarise(two()), 3).rows, []);
});

test("firsts and records", () => {
  const S = summarise(fights());
  const r = Object.fromEntries(recordsModel(S, bossModel(S, 2)).map(c => [c.k, c]));
  assert.deepEqual([r["First-try kills"].v, r["First-try kills"].d], ["1 boss", "Iudex Gundyr"]);
  assert.equal(r["Fastest boss kill"].v, "47 m");  // Vordt: first attempt at 20 m, killed at 1 h 06 m 40 s
  assert.match(r["Fastest boss kill"].d, /^Vordt of the Boreal Valley · first try to kill · S2/);
  assert.equal(r["Toughest boss"].v, "2 deaths");
  assert.equal(r["Longest life"].v, "40 m");  // S1: from the death at 20 m to the end at 1 h
  assert.equal(r["Longest life"].d, "High Wall of Lothric · S1");
  assert.equal(r["Cleanest session"].v, "1.0 deaths / h");
  assert.equal(r["Best levelling session"].v, "+4 levels");
  assert.equal(r["Most souls in a session"].v, "2,000");
  assert.deepEqual([r["First death"].v, r["First death"].d], ["20 m in", "S1 · High Wall of Lothric"]);
  const none = Object.fromEntries(recordsModel(summarise(two()), { rows: [] }).map(c => [c.k, c]));
  assert.equal(none["First-try kills"].d, "No boss kills recorded yet");
  assert.equal(none["Fastest boss kill"].v, "–");
});

test("gear history: blocks per hand across sessions, upgrades lighter", () => {
  const m = gearModel(summarise(fights()));
  const right = m.rows[0];
  assert.equal(right.label, "Main hand");
  // Long Sword 0-25 m, Long Sword +3 from 25 m on, joined across the session break up to 1 h 10 m, then Claymore.
  assert.deepEqual(right.blocks.map(b => [b.label, +(b.left * 4800).toFixed(0), +(b.w * 4800).toFixed(0)]),
    [["Long Sword", 0, 1500], ["Long Sword +3", 1500, 2700], ["Claymore", 4200, 600]]);
  assert.match(right.blocks[1].title, /^Long Sword \+3 · S1–S2 · /);
  assert.notEqual(right.blocks[0].color, right.blocks[1].color);  // the upgrade is lighter
  assert.notEqual(right.blocks[1].color, right.blocks[2].color);  // another weapon
  assert.deepEqual(m.rows[1].blocks.map(b => b.label), ["Knight Shield"]);
  assert.deepEqual(m.ticks.map(t => [t.label, t.left]), [["S1", 0], ["S2", 0.75]]);
  assert.equal(gearModel(summarise(two())), null);
});

test("boss kills on the level line, the sessions table and compare", () => {
  const S = summarise(fights());
  assert.deepEqual(levelModel(S).kills.map(k => [k.name, +k.h.toFixed(3), k.level]),
    [["Iudex Gundyr", 0.083, 10], ["Vordt of the Boreal Valley", 1.111, 10]]);
  assert.deepEqual(sessionRowsModel(S).map(r => r.bosses), ["Vordt of the Boreal Valley", "Iudex Gundyr"]);
  const row = compareModel(S.sessions, 1, 2).find(r => r.k === "Bosses killed");
  assert.deepEqual([row.a, row.b, row.d], ["Iudex Gundyr", "Vordt of the Boreal Valley", "±0"]);
});

// [time, session_t, window_s, ticks, rate_hz, lat_avg_us, lat_max_us, cpu_pct_core, working_set_mb, private_mb]
const perfRow = (sec, { rate = 10, lat = 12, max = 30, cpu = 0.5, ws = 25 } = {}) => {
  const t = new Date(Date.UTC(2026, 9, 7, 10, 0, 0) + sec * 1000).toISOString().slice(0, 19);
  return [t, sec, 5, 50, rate, lat, max, cpu, ws, ws - 4];
};

test("tool performance: per-session figures and the verdict", () => {
  const sessions = two();
  sessions[0].perf = [perfRow(0, { rate: 9.6, max: 177, cpu: 1.1, ws: 12 }), perfRow(5, { rate: 9.8, lat: 14 })];
  sessions[1].perf = [perfRow(0), perfRow(5)];
  const m = perfModel(summarise(sessions));
  assert.deepEqual(m.rows.map(r => [r.n, +r.hz.toFixed(1), r.lat, r.worst]), [[1, 10, 12, 30], [2, 9.7, 13, 177]]);
  assert.equal(m.verdict, "Healthy. Held 10 Hz in 1 of 2 sessions (lowest 9.7); reads average 12.5 µs, worst 177 µs (S2); CPU ≤ 0.8%, memory ≤ 25.0 MB.");
  sessions[0].perf = [perfRow(0, { rate: 8 })];
  assert.match(perfModel(summarise(sessions)).verdict, /^Worth a look\./);
  assert.match(perfModel(summarise(two())).verdict, /^No perf figures yet/);
});

test("co-op card: WASD not running, your code not ready, nothing loaded", () => {
  assert.match(coopStatus(null), /^WASD isn't running/);
  assert.equal(coopStatus({ mineReady: false, loaded: false, loadedName: "", partner: null }),
    "Your code is ready a few seconds after you load into the game. No partner code loaded.");
});

test("co-op card: a loaded partner, before and after the game shows them", () => {
  assert.equal(coopStatus({ mineReady: true, loaded: true, loadedName: "Sir Partnerton", partner: null }),
    "Your code is ready to copy. Loaded: Sir Partnerton. Compare under Report for: Together.");
  assert.equal(coopStatus({ mineReady: true, loaded: true, loadedName: "Sir Partnerton", partner: { name: "Sir Partnerton", level: 55, age: 125 } }),
    "Your code is ready to copy. Loaded: Sir Partnerton, level 55, code from 2 min ago. Compare under Report for: Together.");
});

// /api/coop as WASD sends it: you, a partner, and the lists compared.
const player = over => ({
  name: "Ashen One", level: 42, ng: 0, hp: [812, 900], fp: [93, 120], stamina: [95, 110],
  attrs: [15, 10, 12, 10, 14, 13, 9, 9, 7], souls: 2399, deaths: 33, playSeconds: 84329, age: 0,
  area: "Firelink Shrine", region: "Firelink Shrine", bonfire: "Firelink Shrine",
  bosses: [2, 25], keys: [3, 35], held: 2, items: [120, 1005],
  areas: [{ a: "Firelink Shrine", s: 600, d: 2 }], levels: [[23.3, 40], [23.4, 42]], ...over,
});
const coopData = over => ({
  mineReady: true, loaded: true, loadedName: "Partner", tier: 2,
  me: player(), partner: player({ name: "Partner", level: 55, ng: 1, age: 125, deaths: 10,
    areas: [{ a: "Firelink Shrine", s: 300, d: 0 }, { a: "Other areas", s: 1000, d: 4 }] }),
  where: { sameMap: true, dist: 12.4, dy: 4, clock: "3 o'clock" },
  bosses: { compared: true, rows: [
    { n: "Iudex Gundyr", g: "Cemetery of Ash", m: true, p: true },
    { n: "Vordt of the Boreal Valley", g: "High Wall of Lothric", m: true, p: false },
    { n: "", g: "", m: false, p: true }, { n: "", g: "", m: false, p: false }] },
  keys: { compared: true, rows: [{ n: "Small Lothric Banner", g: "key", m: true, p: true, mh: true, ph: false }] },
  items: { compared: true, rows: [{ a: "Firelink Shrine", t: 30, m: 10, p: 12, b: 8 }] },
  notes: [], ...over,
});

test("co-op comparison: players, where you are, progress and character", () => {
  assert.equal(coopModel(null), null);
  assert.equal(coopModel(coopData({ partner: null })), null);  // nothing loaded: nothing to compare
  const m = coopModel(coopData());
  assert.equal(m.players[0].name, "Ashen One");
  assert.equal(m.players[1].sub, "level 55 · NG+1 · 23h 25m played");
  assert.equal(m.players[1].age, "code from 2 min ago");
  assert.equal(m.where, "12 m apart, 3 o'clock, above you");
  assert.deepEqual(m.progress.map(r => [r.label, r.a, r.b]).slice(0, 4),
    [["Bosses defeated", 2, 2], ["Key items obtained", 3, 3], ["Items found", 120, 120], ["Deaths", 33, 10]]);
  assert.equal(m.progress[0].max, 25);
  assert.deepEqual(m.character[1].map(r => r.label), ["Vigor", "Attunement", "Endurance", "Vitality", "Strength", "Dexterity", "Intelligence", "Faith", "Luck"]);
  assert.deepEqual(m.character[2].map(r => [r.label, r.a]), [["Max HP", 900], ["Max FP", 120], ["Max stamina", 110]]);
  assert.equal(m.waiting, "");
  assert.equal(coopModel(coopData({ where: { sameMap: false, dist: -1, dy: 0, clock: "" } })).where, "In different map sections");
  // Your partner somewhere you haven't been: no place names at all.
  const away = coopModel(coopData({ partner: player({ name: "Partner", area: "", region: "", bonfire: "", elsewhere: true }) }));
  assert.equal(away.players[1].where, "Somewhere you haven't been yet");
  // At Category, as the live page's route hints say it.
  const beyond = coopModel(coopData({ partner: player({ name: "Partner", area: "", region: "", bonfire: "", elsewhere: true, beyond: "High Wall of Lothric" }) }));
  assert.equal(beyond.players[1].where, "In a new area beyond High Wall of Lothric");
  assert.equal(away.players[1].bonfire, "");
});

test("co-op comparison: bosses and key items by group, unnamed ones summed", () => {
  const m = coopModel(coopData());
  assert.deepEqual(m.bosses.count, { both: 1, me: 1, them: 1, neither: 1 });
  assert.deepEqual(m.bosses.groups.map(g => g.name), ["Cemetery of Ash", "High Wall of Lothric"]);
  assert.deepEqual(m.bosses.hidden, { n: 2, m: 0, p: 1 });
  assert.equal(m.bosses.hiddenText, "2 more bosses you haven't reached yet, not named: Partner has 1 of them.");
  assert.equal(m.keys.hiddenText, "");
  assert.equal(m.keys.groups[0].name, "Keys");
  assert.deepEqual(m.keys.groups[0].rows[0], { name: "Small Lothric Banner", me: true, them: true, meHeld: true, themHeld: false });
  // Lists that differ: the partner's side isn't compared.
  const d = coopModel(coopData({ bosses: { compared: false, rows: [{ n: "Iudex Gundyr", g: "Cemetery of Ash", m: true, p: true }] } }));
  assert.equal(d.bosses.compared, false);
  assert.equal(d.bosses.groups[0].rows[0].them, false);
});

test("co-op comparison: items, time and deaths per area, levels", () => {
  const m = coopModel(coopData());
  assert.deepEqual(m.items, [{ label: "Firelink Shrine", a: 10, b: 12, max: 30, both: 8 }]);
  assert.deepEqual(m.time.map(r => [r.label, r.a, r.b]), [["Other areas", 0, 1000], ["Firelink Shrine", 600, 300]]);
  assert.deepEqual(m.deaths.map(r => [r.label, r.a, r.b]), [["Other areas", 0, 4], ["Firelink Shrine", 2, 0]]);
  assert.deepEqual(m.levels.map(s => [s.name, s.pts.length]), [["Ashen One", 2], ["Partner", 2]]);
  assert.equal(coopModel(coopData({ items: { compared: false, rows: [{ a: "Firelink Shrine", t: 30, m: 10, p: 0, b: 0 }] } })).items[0].b, null);
});

test("co-op comparison: before you're in the game, only your partner", () => {
  const m = coopModel(coopData({ me: null, bosses: { compared: false, rows: [] }, keys: { compared: false, rows: [] }, items: { compared: false, rows: [] } }));
  assert.equal(m.players[0], null);
  assert.equal(m.where, "");
  assert.equal(m.progress[0].a, null);
  assert.equal(m.progress[0].b, 2);
  assert.match(m.waiting, /^Load into the game/);
  assert.deepEqual(m.time.map(r => [r.label, r.a, r.b]), [["Other areas", null, 1000], ["Firelink Shrine", null, 300]]);
  const reading = coopModel(coopData({ bosses: { compared: false, rows: [] } }));
  assert.match(reading.waiting, /^Reading your bosses/);
});

test("the whole page script parses (a stray character outside a function broke it once)", () => {
  assert.doesNotThrow(() => new Function(scriptOf("templates/results_template.html").replace("__SESSIONS_JSON__;", "[];")));
  assert.doesNotThrow(() => new Function(scriptOf("templates/live.html")));
});

test("the header has the guide's spoiler tiers, in the live page's order", () => {
  const html = readFileSync(`${ROOT}/templates/results_template.html`, "utf8");
  const tiers = [...html.matchAll(/<button class="u" data-tier="(\d)" title="[^"]*">([^<]+)<\/button>/g)].map(m => [+m[1], m[2]]);
  assert.deepEqual(tiers, [[0, "Off"], [1, "Vague"], [2, "Category"], [3, "Full"]]);
  assert.match(definition(scriptOf("templates/results_template.html"), "setupTier"), /\/api\/tier\?t=/);
});

test("after Copy: the code's length, and what was left out to fit one chat message", () => {
  assert.equal(copiedText({ code: "WASD-" + "x".repeat(1495), leftOut: "" }), "Copied (1,500 characters). Paste it to your partner.");
  assert.equal(copiedText({ code: "WASD-x", leftOut: "the weapons per session and the 3 oldest sessions" }),
    "Copied (6 characters). Paste it to your partner. To fit one chat message it leaves out the weapons per session and the 3 oldest sessions.");
});

// ---- pair mode: your sessions (fights()) and a partner from /api/coop ----
const pairFixture = () => {
  const Sme = summarise(fights());
  Sme.name = "Corwen";
  const minute = d => Math.floor(d.getTime() / 60000);
  const s1 = minute(Sme.sessions[0].start), s2 = minute(Sme.sessions[1].start);
  const p1 = minute(new Date(2026, 9, 3, 18, 0, 0));
  const pp = {
    totals: { seconds: 5400, deaths: 3, lost: 100, recovered: 50, gained: 1500, longest: 3600 },
    sessions: [  // newest first
      { start: s2 + 5, minutes: 30, deaths: 2, gained: 1000, levels: [12, 15], areas: ["High Wall of Lothric", "Other areas"], kills: 1,
        weapons: [{ name: "Long Sword", upgrade: 2 }, null] },
      { start: p1, minutes: 60, deaths: 1, gained: 500, levels: [8, 12], areas: ["Cemetery of Ash"], kills: 1,
        weapons: [{ name: "", upgrade: 0 }, null] },
    ],
    bosses: [
      { name: "Iudex Gundyr", area: "Cemetery of Ash", deaths: 0, ttk: null, session: 1, kill: p1 + 20 },
      { name: "Vordt of the Boreal Valley", area: "High Wall of Lothric", deaths: 1, ttk: 5, session: 0, kill: s2 + 7 },
    ],
    life: { seconds: 1200, session: 0, area: "High Wall of Lothric" },
    firstDeath: { seconds: 300, session: 1, area: "Cemetery of Ash" },
  };
  const c = {
    partner: player({ name: "Isolde", deaths: 40, playSeconds: 20000, levels: [[1, 8], [5, 15]],
      areas: [{ a: "Cemetery of Ash", s: 3600, d: 1 }, { a: "High Wall of Lothric", s: 1800, d: 2 }] }),
    pair: { me: { sessions: [{ start: s2 }, { start: s1 }] }, partner: pp,
            together: { minutes: 15, blocks: [{ me: 0, partner: 0, start: s2 + 5, minutes: 15 }] } },
    items: { compared: true, rows: [{ a: "Firelink Shrine", t: 30, m: 10, p: 12, b: 8 }, { a: "Other areas", t: 500, m: 0, p: 20, b: 0 }] },
  };
  return { Sme, c, Sp: partnerSummary(pp, c.partner), s1, s2, p1 };
};

test("partner's report: their code made into the shape of your own sessions", () => {
  const { Sp, s2, p1 } = pairFixture();
  assert.equal(Sp.name, "Isolde");
  assert.deepEqual(Sp.sessions.map(s => [s.n, Math.floor(s.start.getTime() / 60000), s.dur, s.deaths]), [[1, p1, 3600, 1], [2, s2 + 5, 1800, 2]]);
  assert.deepEqual(Sp.sessions.map(s => s.kills.map(k => k.name)), [["Iudex Gundyr"], ["Vordt of the Boreal Valley"]]);
  assert.deepEqual(Sp.sessions[1].segs, [{ area: "High Wall of Lothric", sec: 900 }, { area: "Other areas", sec: 900 }]);
  assert.equal(Sp.sessions[0].gear[0].name, "A weapon you haven't found");
  assert.deepEqual([Sp.seconds, Sp.deathsTotal, Sp.lastPlay], [5400, 40, 20000000]);
  assert.deepEqual(Sp.areaOrder, ["Cemetery of Ash", "High Wall of Lothric"]);
  const records = Object.fromEntries(recordsModel(Sp, partnerBossModel(Sp)).map(c => [c.k, c]));
  assert.deepEqual([records["Longest life"].v, records["Longest life"].d], ["20 m", "High Wall of Lothric · S2"]);
  assert.deepEqual([records["First death"].v, records["First death"].d], ["5 m in", "S1 · Cemetery of Ash"]);
  assert.deepEqual([records["First-try kills"].v, records["First-try kills"].score], ["1 boss", 1]);
  assert.deepEqual(partnerBossModel(Sp).rows.map(r => [r.name, r.deaths, r.ttk]), [["Iudex Gundyr", 0, null], ["Vordt of the Boreal Valley", 1, 300]]);
  const lm = levelModel(Sp, "game");
  assert.deepEqual([lm.first, lm.last, lm.kills.length], [8, 15, 0]);
});

test("played together: overlap, deaths, share, bosses felled in the same overlap", () => {
  const { Sme, c, Sp } = pairFixture();
  const tg = togetherModel(Sme, Sp, c.pair, bossModel(Sme, 2));
  assert.equal(tg.time, "15 m");
  assert.deepEqual(tg.blocks.map(b => [b.myN, b.partnerN]), [[2, 2]]);
  assert.deepEqual(tg.together, ["Vordt of the Boreal Valley"]);
  assert.deepEqual(tg.stats.map(x => [x.k, x.v, x.s]), [
    ["Bosses together", "1", "Vordt of the Boreal Valley"],
    ["Deaths together", "4", "you 2 · Isolde 2"],
    ["Share of play", "19%", "of yours · 17% of Isolde's"],
  ]);
  assert.equal(tg.me, "1 h 20 m recorded, 1 solo session");
});

test("pair tiles, bars, bosses and records", () => {
  const { Sme, c, Sp } = pairFixture();
  const tiles = Object.fromEntries(pairTiles(Sme, Sp).map(x => [x.k, x]));
  assert.deepEqual([tiles["Time recorded"].a, tiles["Time recorded"].b], ["1 h 20 m", "1 h 30 m"]);
  const bars = pairAreaBars(Sme, Sp);
  assert.deepEqual(bars.time.map(r => [r.name, r.label]), [["Cemetery of Ash", "10 m · 1 h 00 m"], ["High Wall of Lothric", "1 h 10 m · 30 m"],
    ["Undead Settlement", "– · –"]]);
  assert.deepEqual(bars.deaths.map(r => [r.name, r.a, r.b]), [["Cemetery of Ash", 0, 1], ["High Wall of Lothric", 2, 2], ["Undead Settlement", 1, 0]]);
  // Time per area shows both values over the row on hover.
  const { pairBarTip } = load("templates/results_template.html", ["esc", "dotOf", "pairBarTip"]);
  assert.equal(pairBarTip(bars.time[0], ["Ann", "<Bo>"]),
    '<b>Cemetery of Ash</b><div class="tl"><i class="pd me"></i><span>Ann</span><span class="tv">10 m</span></div>' +
    '<div class="tl"><i class="pd them"></i><span>&lt;Bo&gt;</span><span class="tv">1 h 00 m</span></div>');
  const myBosses = bossModel(Sme, 2), tg = togetherModel(Sme, Sp, c.pair, myBosses);
  const rows = pairBossRows(myBosses.rows, partnerBossModel(Sp).rows, tg.together);
  assert.deepEqual(rows.map(r => [r.name, r.tag, !!r.me, !!r.them]), [
    ["Iudex Gundyr", "", true, true], ["Vordt of the Boreal Valley", "together", true, true], ["A boss in Undead Settlement", "", true, false]]);
  const recs = Object.fromEntries(pairRecords(recordsModel(Sme, myBosses), recordsModel(Sp, partnerBossModel(Sp))).map(x => [x.k, x]));
  assert.deepEqual(recs["Most souls in a session"].lines.map(l => [l.v, l.ahead]), [["2,000", true], ["1,000", false]]);
  assert.deepEqual(recs["First-try kills"].lines.map(l => l.ahead), [false, false]);  // a tie
  assert.deepEqual(recs["First death"].lines.map(l => l.ahead), [false, false]);      // never ahead
});

test("joint timeline, who found what and the sessions for both", () => {
  const { Sme, c, Sp } = pairFixture();
  const myBosses = bossModel(Sme, 2), tg = togetherModel(Sme, Sp, c.pair, myBosses);
  const jt = jointTimelineModel(Sme, Sp, tg);
  assert.equal(jt.days, 4);  // 3 Oct to 6 Oct
  assert.deepEqual(jt.lanes.map(l => [l.who, l.blocks.map(b => b.left * 4)]), [["me", [2]], ["together", [3]], ["them", [0]]]);
  const wf = whoFoundModel(c, myBosses, Sp, tg);
  assert.deepEqual(wf.rows.map(r => [r.name, r.me, r.them, r.both, r.only]), [["Firelink Shrine", 10, 12, 8, "2 · 4"], ["Other areas", 0, 20, 0, "0 · 20"]]);
  assert.deepEqual(wf.total, { me: 10, them: 32, both: 8, only: "2 · 24" });
  assert.deepEqual(wf.groups.map(g => [g.kind, g.items]), [["together", ["Vordt of the Boreal Valley"]], ["both", ["Iudex Gundyr"]], ["me", []], ["them", []]]);
  const rows = pairSessionRows(Sme, Sp, tg);
  assert.deepEqual(rows.map(r => [r.who, r.deaths]), [["together", "2 · 2"], ["me", "1"], ["them", "1"]]);
  assert.equal(rows[0].length, "0:15");
});

test("gear blocks never get thinner than a few pixels, and stay inside the track", () => {
  const { gearBlockStyle } = load("templates/results_template.html", ["pct", "GEAR_MIN_PX", "gearBlockStyle"]);
  // A 4-minute session at the end of 3 hours: 2% of the track.
  assert.equal(gearBlockStyle({ left: 0.9774, w: 0.0226 }), "left:min(97.74%, calc(100% - 8px));width:max(2.26%, 8px)");
  assert.equal(gearBlockStyle({ left: 0, w: 0.5 }), "left:min(0.00%, calc(100% - 8px));width:max(50.00%, 8px)");
});

test("as you play: new sessions are told apart by what they add, and the character kept", () => {
  const { tagCharacters, charactersOf, sessionsKey } = load("templates/results_template.html", ["tagCharacters", "charactersOf", "sessionsKey"]);
  const s = (file, character, n) => ({ file, events: [{ type: "start", character }, ...Array(n).fill({ type: "pos" })] });
  const before = tagCharacters([s("a", "Ann", 2), s("b", "Bo", 1), s("c", "Bo", 1)]);
  assert.deepEqual(before.map(x => x.character), ["Ann", "Bo", "Bo"]);
  assert.deepEqual(charactersOf(before), ["Bo", "Ann"]);  // most sessions first
  assert.equal(tagCharacters([{ file: "x", events: [] }])[0].character, "Unknown");
  const same = tagCharacters([s("a", "Ann", 2), s("b", "Bo", 1), s("c", "Bo", 1)]);
  assert.equal(sessionsKey(same), sessionsKey(before));
  const grown = tagCharacters([s("a", "Ann", 2), s("b", "Bo", 1), s("c", "Bo", 3)]);
  assert.notEqual(sessionsKey(grown), sessionsKey(before));
  assert.notEqual(sessionsKey([...before, s("d", "Ann", 0)]), sessionsKey(before));
  assert.notEqual(sessionsKey([{ ...before[0], perf: [[1]] }, ...before.slice(1)]), sessionsKey(before));
  // It asks WASD with the last stamp, so a poll with nothing new reads no files.
  assert.match(definition(scriptOf("templates/results_template.html"), "setupLive"), /\/api\/sessions\?have=\$\{stamp\}/);
});
