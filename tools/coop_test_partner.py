"""A test partner for the results page's co-op reports, made from your own code.

With WASD running and you in the game (the results page open for a few
seconds, so WASD has read your bosses and items), this fetches your co-op
code from WASD, turns it into a believable partner and loads it as your
partner's code:

    python -I tools/coop_test_partner.py            # load it into WASD
    python -I tools/coop_test_partner.py --print    # only print the code

The partner has your lists (so everything compares) and differs from you:

- items: about a quarter of the world pickups you haven't found are theirs,
  and about a tenth of the ones you have found aren't;
- key items: two you haven't obtained (one still held), one of yours not;
- bosses: the first boss you haven't defeated is theirs (with a kill
  record), and their last kill of yours is undone;
- sessions: their two newest start when yours did and run 30 minutes
  longer (played together), the others are a day or more earlier; a few
  more levels, deaths and souls.

It reads and writes the code's documented layout (docs/TECHNICAL.md,
"Co-op codes"), independently of WASD's C++, so it doubles as a check of
that layout. Nothing here touches the game.
"""
import argparse
import base64
import json
import random
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request
import zlib

PREFIX = "WASD-"


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def get(self, bits):
        v = 0
        for k in range(bits):
            if self.pos >= len(self.data) * 8:
                raise ValueError("truncated")
            if (self.data[self.pos // 8] >> (self.pos % 8)) & 1:
                v |= 1 << k
            self.pos += 1
        return v


class Writer:
    def __init__(self):
        self.out, self.n = bytearray(), 0

    def put(self, value, bits):
        for k in range(bits):
            if self.n % 8 == 0:
                self.out.append(0)
            if (value >> k) & 1:
                self.out[-1] |= 1 << (self.n % 8)
            self.n += 1


def from_text(code):
    """The packet inside a code (prefix, base64url, CRC-32 checked)."""
    code = "".join(code.split())
    if not code.startswith(PREFIX):
        raise ValueError("not a WASD code")
    body = code[len(PREFIX):]
    raw = base64.urlsafe_b64decode(body + "=" * (-len(body) % 4))
    packet, crc = raw[:-4], struct.unpack("<I", raw[-4:])[0]
    if zlib.crc32(packet) & 0xFFFFFFFF != crc:
        raise ValueError("damaged")
    return packet


def to_text(packet):
    return PREFIX + base64.urlsafe_b64encode(packet + struct.pack("<I", zlib.crc32(packet) & 0xFFFFFFFF)).decode().rstrip("=")


def decode(packet):
    """A protocol-1 FullSnapshot as a dict (the fields in layout order)."""
    if packet[0] != 1 or packet[1] != 2:
        raise ValueError("not a protocol-1 snapshot")
    r = Reader(packet[6:])
    s = {"sequence": struct.unpack("<I", packet[2:6])[0], "captured": r.get(32)}
    s["name"] = bytes(r.get(8) for _ in range(r.get(6))).decode("utf-8")
    s["level"], s["hp"], s["maxHp"], s["fp"], s["maxFp"], s["stamina"], s["maxStamina"] = (r.get(16) for _ in range(7))
    s["attrs"] = [r.get(8) for _ in range(9)]
    s["souls"], s["deaths"], s["playSeconds"], s["ng"] = r.get(32), r.get(32), r.get(32), r.get(8)
    s["mapId"], s["region"], s["bonfire"] = r.get(32), r.get(32), r.get(32)
    s["pose"] = [r.get(32) for _ in range(4)] if r.get(1) else None  # raw float bits
    s["sets"] = []
    for _ in range(4):  # bosses defeated, key items obtained, key items held, pickups found
        fingerprint, count = r.get(32), r.get(12)
        s["sets"].append({"fingerprint": fingerprint, "bits": [r.get(1) for _ in range(count)]})
    s["areaFingerprint"] = r.get(32)
    s["areas"] = [{"index": r.get(6), "seconds": r.get(24), "deaths": r.get(16)} for _ in range(r.get(6))]
    s["levels"] = [{"minutes": r.get(20), "level": r.get(10)} for _ in range(r.get(6))]
    s["totals"] = [r.get(32) for _ in range(6)]
    count, s["areaBits"], s["weapons"] = r.get(6), r.get(6), r.get(1)
    s["sessions"] = []
    for _ in range(count):
        x = {"start": r.get(28), "minutes": r.get(12), "deaths": r.get(12), "gained": r.get(24),
             "levelStart": r.get(10), "levelEnd": r.get(10), "areaBits": sum(r.get(1) << b for b in range(s["areaBits"])),
             "kills": r.get(6)}
        if s["weapons"]:
            x["hands"] = [[r.get(18), r.get(5)], [r.get(18), r.get(5)]]
        s["sessions"].append(x)
    s["bosses"] = [{"boss": r.get(6), "deaths": r.get(10), "ttk": r.get(16), "session": r.get(6), "kill": r.get(28)}
                   for _ in range(r.get(6))]
    s["life"] = {"seconds": r.get(24), "session": r.get(6), "area": r.get(6)}
    s["firstDeath"] = {"seconds": r.get(24), "session": r.get(6), "area": r.get(6)}
    # Only zero padding to the byte boundary may follow.
    total = len(packet[6:]) * 8
    if total - r.pos >= 8 or any((packet[6 + p // 8] >> (p % 8)) & 1 for p in range(r.pos, total)):
        raise ValueError("unexpected data after the snapshot")
    return s


def encode(s, sequence=None):
    w = Writer()
    w.put(s["captured"], 32)
    name = s["name"].encode("utf-8")[:48]
    w.put(len(name), 6)
    for c in name:
        w.put(c, 8)
    for k in ("level", "hp", "maxHp", "fp", "maxFp", "stamina", "maxStamina"):
        w.put(min(s[k], 0xFFFF), 16)
    for a in s["attrs"]:
        w.put(min(a, 255), 8)
    w.put(s["souls"], 32), w.put(s["deaths"], 32), w.put(s["playSeconds"], 32), w.put(s["ng"], 8)
    w.put(s["mapId"], 32), w.put(s["region"], 32), w.put(s["bonfire"], 32)
    w.put(1 if s["pose"] else 0, 1)
    for v in s["pose"] or []:
        w.put(v, 32)
    for st in s["sets"]:
        w.put(st["fingerprint"], 32), w.put(len(st["bits"]), 12)
        for b in st["bits"]:
            w.put(b, 1)
    w.put(s["areaFingerprint"], 32), w.put(len(s["areas"]), 6)
    for a in s["areas"]:
        w.put(a["index"], 6), w.put(min(a["seconds"], 0xFFFFFF), 24), w.put(min(a["deaths"], 0xFFFF), 16)
    w.put(len(s["levels"]), 6)
    for p in s["levels"]:
        w.put(p["minutes"], 20), w.put(p["level"], 10)
    for v in s["totals"]:
        w.put(v, 32)
    w.put(len(s["sessions"]), 6), w.put(s["areaBits"], 6), w.put(s["weapons"], 1)
    for x in s["sessions"]:
        w.put(x["start"], 28), w.put(min(x["minutes"], 4095), 12), w.put(min(x["deaths"], 4095), 12)
        w.put(min(x["gained"], 0xFFFFFF), 24), w.put(x["levelStart"], 10), w.put(x["levelEnd"], 10)
        for b in range(s["areaBits"]):
            w.put((x["areaBits"] >> b) & 1, 1)
        w.put(x["kills"], 6)
        if s["weapons"]:
            for weapon, upgrade in x["hands"]:
                w.put(weapon, 18), w.put(upgrade, 5)
    w.put(len(s["bosses"]), 6)
    for b in s["bosses"]:
        w.put(b["boss"], 6), w.put(b["deaths"], 10), w.put(b["ttk"], 16), w.put(b["session"], 6), w.put(b["kill"], 28)
    for m in (s["life"], s["firstDeath"]):
        w.put(m["seconds"], 24), w.put(m["session"], 6), w.put(m["area"], 6)
    seq = s["sequence"] if sequence is None else sequence
    return bytes([1, 2]) + struct.pack("<I", seq) + bytes(w.out)


# The partner's weapons, as the code carries them: the param id / 100.
PARTNER_WEAPONS = {"Broadsword": 20200, "Claymore": 60200, "Grass Crest Shield": 212000, "Lothric Knight Shield": 210100}


def make_partner(mine, name="Test Partner", seed=7):
    """A partner from your decoded state: same lists, different progress."""
    rnd = random.Random(seed)
    s = json.loads(json.dumps(mine))
    s["name"] = name
    s["captured"] = max(0, s["captured"] - 120)
    s["level"] += 5
    s["maxHp"] += 60
    s["hp"] = s["maxHp"]
    s["attrs"] = [a + rnd.choice((0, 1, 2)) for a in s["attrs"]]
    s["souls"] += 4000
    s["deaths"] += 12
    s["playSeconds"] += 7200
    bosses, keys, held, pickups = (st["bits"] for st in s["sets"])
    # Items: a quarter of yours-unfound are theirs, a tenth of yours aren't.
    unfound = [i for i, b in enumerate(pickups) if not b]
    found = [i for i, b in enumerate(pickups) if b]
    for i in rnd.sample(unfound, len(unfound) // 4):
        pickups[i] = 1
    for i in rnd.sample(found, len(found) // 10):
        pickups[i] = 0
    # Key items: two you haven't (one still held), one of yours not.
    missing = [i for i, b in enumerate(keys) if not b]
    for n, i in enumerate(missing[:2]):
        keys[i] = 1
        held[i] = 1 if n == 0 else 0
    have = [i for i, b in enumerate(keys) if b and i not in missing[:2]]
    if have:
        keys[have[-1]] = held[have[-1]] = 0
    # Bosses: the first you haven't defeated is theirs; their last kill of yours undone.
    extra = next((i for i, b in enumerate(bosses) if not b), None)
    yours = [i for i, b in enumerate(bosses) if b]
    if len(yours) > 1:
        bosses[yours[-1]] = 0
        s["bosses"] = [b for b in s["bosses"] if b["boss"] != yours[-1]]
    if extra is not None:
        bosses[extra] = 1
    # Sessions: their two newest start when yours did and run 30 minutes
    # longer (so they overlap however short yours were); the rest move a day
    # or more earlier.
    for k, x in enumerate(s["sessions"]):
        if not x["start"]:
            continue
        if k < 2:
            x["minutes"] += 30
        else:
            x["start"] -= 1440 * k
        x["deaths"] += rnd.choice((0, 1, 2))
        x["gained"] += 500
    if s["sessions"] and s["sessions"][0]["start"]:
        newest = s["sessions"][0]
        if extra is not None and extra < 63:
            newest["kills"] += 1
            s["bosses"].append({"boss": extra, "deaths": 3, "ttk": 12, "session": 0,
                                "kill": newest["start"] + newest["minutes"] - 5})
    for b in s["bosses"]:
        b["deaths"] += rnd.choice((0, 1))
    # Areas: their own time (×0.5 to ×1.6 of yours) and deaths in each.
    for a in s["areas"]:
        a["seconds"] = min(a["seconds"] * rnd.randint(50, 160) // 100, (1 << 24) - 1)
        a["deaths"] = min(a["deaths"] + rnd.choice((0, 1, 2, 3)), (1 << 16) - 1)
    s["totals"][0] += 7200
    s["totals"][1] += 12
    s["totals"][4] += 4000
    # Weapons: their own, in every session (yours may predate weapon
    # logging), oldest first: a Broadsword upgraded as they go, a Claymore
    # for the newest third; a Grass Crest Shield, then a Lothric Knight Shield.
    timed = [x for x in s["sessions"] if x["start"]]
    for k, x in enumerate(reversed(timed)):
        f = k / max(1, len(timed) - 1)
        right = PARTNER_WEAPONS["Claymore" if f > 2 / 3 else "Broadsword"]
        left = PARTNER_WEAPONS["Grass Crest Shield" if f < 0.5 else "Lothric Knight Shield"]
        x["hands"] = [[right, round(f * 10)], [left, 0]]
    for x in s["sessions"]:
        x.setdefault("hands", [[0, 0], [0, 0]])
    s["weapons"] = 1
    # Records: a shorter longest life, a later first death, and their newest
    # session levels two more than yours did.
    if s["life"]["seconds"]:
        s["life"]["seconds"] = s["life"]["seconds"] * 3 // 4
    if s["firstDeath"]["seconds"]:
        s["firstDeath"]["seconds"] = min(s["firstDeath"]["seconds"] * 3 // 2, 0xFFFFFF)
    if timed:
        timed[0]["levelEnd"] = min(timed[0]["levelEnd"] + 2, (1 << 10) - 1)
    # Levels: their own curve, your clock stretched to their longer play
    # time (each level later) and pulling ahead of yours, up to their 5
    # more at the end.
    n = len(s["levels"])
    mine_min, theirs_min = max(1, mine["playSeconds"] // 60), s["playSeconds"] // 60
    for k, p in enumerate(s["levels"]):
        p["minutes"] = min(p["minutes"] * theirs_min // mine_min, theirs_min)
        p["level"] = min(p["level"] + 5 * (k + 1) // (n + 1), (1 << 10) - 1)
    if s["levels"]:
        s["levels"].append({"minutes": s["playSeconds"] // 60, "level": s["level"]})
        s["levels"] = s["levels"][-32:]
    return s


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--name", default="Test Partner")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--print", action="store_true", help="print the code instead of loading it")
    args = ap.parse_args(argv)
    base = f"http://localhost:{args.port}"
    try:
        with urllib.request.urlopen(f"{base}/api/coop/code", timeout=5) as r:
            answer = json.load(r)
    except urllib.error.HTTPError as e:
        answer = json.load(e)
    except OSError as e:
        print(f"WASD isn't answering on {base}: {e}", file=sys.stderr)
        return 2
    if not answer.get("ok"):
        print(f"No code from WASD: {answer.get('error', 'not ready')}", file=sys.stderr)
        return 2
    partner = make_partner(decode(from_text(answer["code"])), args.name, args.seed)
    code = to_text(encode(partner, sequence=1))
    if args.print:
        print(code)
        return 0
    req = urllib.request.Request(f"{base}/api/coop/partner?code={urllib.parse.quote(code)}", method="POST")
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            result = json.load(r)
    except urllib.error.HTTPError as e:
        result = json.load(e)
    if not result.get("ok"):
        print(f"WASD refused the partner code: {result.get('error')}", file=sys.stderr)
        return 1
    print(f"Loaded {result.get('name')} as your partner ({len(code)} characters). Open the results page: Report for Together or {args.name}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
