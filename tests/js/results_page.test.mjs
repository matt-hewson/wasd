// The results page's data logic (templates/results_template.html):
// summarise() -- trail runs per area, warp attribution, time per area --
// and the perf card's one-line verdict.
import { test } from "node:test";
import assert from "node:assert/strict";
import { load } from "./extract.mjs";

const { summarise, perfSummary, fmtDur } = load("templates/results_template.html", ["fmtDur", "summarise", "perfSummary"]);

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

// [time, session_t, window_s, ticks, rate_hz, lat_avg, lat_max, cpu, working_set, private]
const perfRow = (sec, { rate = 10, lat = 12, max = 30, cpu = 0.5, ws = 25 } = {}) => {
  const t = new Date(Date.UTC(2026, 9, 7, 10, 0, 0) + sec * 1000).toISOString().slice(0, 19);
  return [t, sec, 5, 50, rate, lat, max, cpu, ws, ws - 4];
};

test("perf verdict: rates, latency, CPU and memory drift", () => {
  const rows = Array.from({ length: 8 }, (_, i) => perfRow(i * 5, { ws: 25 + i * 0.5, rate: i === 3 ? 9.5 : 10, max: i === 6 ? 400 : 30 }));
  const text = perfSummary(rows);
  assert.match(text, /poll rate 9\.94 Hz avg \(lowest 9\.50\)/);
  assert.match(text, /latency 12\.0 µs avg, 400\.0 µs worst/);
  assert.match(text, /CPU 0\.50%/);
  assert.match(text, /memory 25\.0 → 28\.5 MB \(\+3\.0 MB first to last quarter\)/);
});

test("perf verdict shows shrinking memory with a minus", () => {
  const rows = Array.from({ length: 8 }, (_, i) => perfRow(i * 5, { ws: 30 - i }));
  assert.match(perfSummary(rows), /\(−6\.0 MB first to last quarter\)/);
});
