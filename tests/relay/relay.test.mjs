// The co-op relay the host's WASD runs (`wasd-cli.exe relay-host`), with its
// timings shortened, driven by real WebSocket clients.
import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { skip, startHost, stopHost, newSession, upgradeStatus, joinRoom, sleep } from "./host.mjs";

const PORT = 8797;
const BASE = `http://127.0.0.1:${PORT}`;
const STALE_MS = 1500, EMPTY_ROOM_MS = 1500, UNJOINED_ROOM_MS = 3000;
const CODE_ALPHABET = "23456789ABCDEFGHJKMNPQRSTVWXYZ";  // no 0/O, 1/I/L, U
const KEY = "ab".repeat(32);  // a pairing key: 64 hex digits

let host;
before(async () => {
  if (!skip) host = await startHost(PORT, { staleMs: STALE_MS, emptyRoomMs: EMPTY_ROOM_MS, unjoinedRoomMs: UNJOINED_ROOM_MS });
});
after(() => stopHost(host));

const status = path => upgradeStatus(`${BASE}${path}`);
const join_ = code => joinRoom(PORT, code);

async function pair() {
  const code = await newSession(BASE);
  const a = join_(code);
  await a.opened;
  const b = join_(code.toLowerCase());  // codes are case-insensitive
  await b.opened;
  assert.equal(await a.next(), "partner:joined");
  assert.equal(await b.next(), "partner:joined");
  return { code, a, b };
}

test("relay", { skip }, async t => {
  await t.test("answers on / and refuses unknown paths", async () => {
    assert.equal((await fetch(`${BASE}/`)).status, 200);
    assert.equal((await fetch(`${BASE}/nope`)).status, 404);
    assert.equal((await fetch(`${BASE}/session`)).status, 405);
  });

  await t.test("POST /session gives a 6-character code from the alphabet", async () => {
    const code = await newSession(BASE);
    assert.match(code, new RegExp(`^[${CODE_ALPHABET}]{6}$`));
  });

  await t.test("refuses a bad code, an unknown room and a plain GET", async () => {
    assert.equal(await status("/session/AB"), 400);
    assert.equal(await status("/session/K7DP0X"), 400);  // 0 isn't in the alphabet
    assert.equal(await status("/session/ZZZZZZ"), 404);  // never created
    assert.equal((await fetch(`${BASE}/session/${await newSession(BASE)}`)).status, 426);
  });

  await t.test("passes binary frames both ways, byte for byte", async () => {
    const { a, b } = await pair();
    const one = new Uint8Array([1, 2, 3, 0, 255]);
    a.ws.send(one);
    assert.deepEqual(await b.next(), one);
    const big = new Uint8Array(64 * 1024).map((_, i) => i * 7);  // exactly the limit
    b.ws.send(big);
    assert.deepEqual(await a.next(), big);
    a.ws.close(); b.ws.close();
  });

  await t.test("answers the heartbeat", async () => {
    const { a, b } = await pair();
    a.ws.send("ping");
    assert.equal(await a.next(), "pong");
    assert.equal(b.frames.length, 0);  // not passed on
    a.ws.close(); b.ws.close();
  });

  await t.test("a third client is refused", async () => {
    const { code, a, b } = await pair();
    assert.equal(await status(`/session/${code}`), 409);
    a.ws.close(); b.ws.close();
  });

  await t.test("closes a socket that sends text or too big a frame", async () => {
    const { a, b } = await pair();
    a.ws.send("hello");
    assert.equal((await a.closedP).code, 1003);
    assert.equal(await b.next(), "partner:left");
    b.ws.send(new Uint8Array(64 * 1024 + 1));
    assert.equal((await b.closedP).code, 1009);
  });

  await t.test("tells the partner when you leave and come back", async () => {
    const { code, a, b } = await pair();
    a.ws.close();
    assert.equal(await b.next(), "partner:left");
    const a2 = join_(code);
    await a2.opened;
    assert.equal(await a2.next(), "partner:joined");
    assert.equal(await b.next(), "partner:joined");
    a2.ws.send(new Uint8Array([9]));
    assert.deepEqual(await b.next(), new Uint8Array([9]));
    a2.ws.close(); b.ws.close();
  });

  await t.test("a silent socket can be replaced, a pinging one can't", async () => {
    const { code, a, b } = await pair();
    const beat = setInterval(() => a.ws.send("ping"), 300);  // a keeps its heartbeat, b goes quiet
    try {
      await sleep(STALE_MS + 700);
      while (a.frames.length) a.frames.shift();
      const b2 = join_(code);  // b's replacement: b's connection "died"
      await b2.opened;
      // Straight away, while b2 is fresh: a and b2 are both live.
      assert.equal(await status(`/session/${code}`), 409);
      assert.equal((await b.closedP).code, 4000);
      b2.ws.close();
    } finally {
      clearInterval(beat);
      a.ws.close();
    }
  });

  await t.test("a pairing room is made by joining it, holds two, and refuses a bad key", async () => {
    assert.equal(await status(`/pair/${KEY.slice(1)}`), 400);
    assert.equal(await status(`/pair/${KEY.toUpperCase()}`), 400);
    const a = join_(`/pair/${KEY}`);
    await a.opened;  // nobody made it first
    const b = join_(`/pair/${KEY}`);
    await b.opened;
    assert.equal(await a.next(), "partner:joined");
    assert.equal(await b.next(), "partner:joined");
    a.ws.send(new Uint8Array([4, 2]));
    assert.deepEqual(await b.next(), new Uint8Array([4, 2]));
    assert.equal(await status(`/pair/${KEY}`), 409);
    // Another key is another room.
    const other = join_(`/pair/${"cd".repeat(32)}`);
    await other.opened;
    await sleep(200);
    assert.equal(other.frames.length, 0);
    a.ws.close(); b.ws.close(); other.ws.close();
    await Promise.all([a.closedP, b.closedP, other.closedP]);
  });

  await t.test("rooms are forgotten: never joined, and after the last one leaves", async () => {
    const unjoined = await newSession(BASE);
    const { code, a, b } = await pair();
    a.ws.close(); b.ws.close();
    await Promise.all([a.closedP, b.closedP]);
    await sleep(Math.max(UNJOINED_ROOM_MS, EMPTY_ROOM_MS) + 1500);
    assert.equal(await status(`/session/${unjoined}`), 404);
    assert.equal(await status(`/session/${code}`), 404);
  });
});
