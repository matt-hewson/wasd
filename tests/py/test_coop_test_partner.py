"""tools/coop_test_partner.py: its own reading and writing of the co-op code
layout (docs/TECHNICAL.md, "Co-op codes"), and the partner it makes. When
the built wasd-cli.exe is there, the C++ decoder reads the partner's code
too, so the two implementations of the layout are checked against each
other."""
import os
import subprocess
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import coop_test_partner as ctp  # noqa: E402


def sample():
    """A small but complete state: every field of the layout set."""
    return {
        "sequence": 3, "captured": 1791800000, "name": "Ashen One",
        "level": 42, "hp": 812, "maxHp": 900, "fp": 93, "maxFp": 120, "stamina": 95, "maxStamina": 110,
        "attrs": [15, 10, 12, 10, 14, 13, 9, 9, 7], "souls": 2399, "deaths": 33, "playSeconds": 84329, "ng": 1,
        "mapId": 0x28000000, "region": 400110, "bonfire": 1400000, "pose": [1, 2, 3, 4],
        "sets": [
            {"fingerprint": 11, "bits": [1, 0, 0, 0]},     # bosses
            {"fingerprint": 22, "bits": [1, 0, 0]},        # key items obtained
            {"fingerprint": 22, "bits": [1, 0, 0]},        # held
            {"fingerprint": 33, "bits": [1, 0] * 20},      # pickups
        ],
        "areaFingerprint": 44, "areas": [{"index": 0, "seconds": 600, "deaths": 2}, {"index": 2, "seconds": 95, "deaths": 0}],
        "levels": [{"minutes": 1400, "level": 40}, {"minutes": 1405, "level": 42}],
        "totals": [695, 2, 1500, 500, 4000, 600],
        "areaBits": 3, "weapons": 1,
        "sessions": [
            {"start": 29300000, "minutes": 2, "deaths": 0, "gained": 1000, "levelStart": 40, "levelEnd": 42, "areaBits": 4, "kills": 1,
             "hands": [(20100, 3), (211000, 0)]},
            {"start": 29298000, "minutes": 10, "deaths": 2, "gained": 3000, "levelStart": 38, "levelEnd": 40, "areaBits": 1, "kills": 1,
             "hands": [(0, 0), (0, 0)]},
        ],
        "bosses": [{"boss": 0, "deaths": 0, "ttk": 0xFFFF, "session": 1, "kill": 29298005}],
        "life": {"seconds": 420, "session": 1, "area": 0},
        "firstDeath": {"seconds": 180, "session": 1, "area": 0},
    }


def as_decoded(s):
    """What decode gives back for `s`: each hand as a [weapon, upgrade] list."""
    s = dict(s)
    s["sessions"] = [dict(x, hands=[list(h) for h in x["hands"]]) for x in s["sessions"]]
    return s


class LayoutTest(unittest.TestCase):
    def test_round_trip(self):
        s = sample()
        self.assertEqual(ctp.decode(ctp.encode(s)), as_decoded(s))
        code = ctp.to_text(ctp.encode(s))
        self.assertTrue(code.startswith("WASD-"))
        self.assertEqual(ctp.from_text(code[:20] + "\n  " + code[20:]), ctp.encode(s))  # whitespace ignored

    def test_refusals(self):
        code = ctp.to_text(ctp.encode(sample()))
        with self.assertRaises(ValueError):
            ctp.from_text(code[:30] + ("A" if code[30] != "A" else "B") + code[31:])  # damaged
        with self.assertRaises(ValueError):
            ctp.decode(ctp.encode(sample()) + b"\0")  # a whole extra byte
        with self.assertRaises(ValueError):
            ctp.from_text(code[5:])  # no prefix


class PartnerTest(unittest.TestCase):
    def test_partner_differs_from_you_in_every_list(self):
        mine = sample()
        p = ctp.make_partner(ctp.decode(ctp.encode(mine)), "Isolde", seed=1)
        self.assertEqual(p["name"], "Isolde")
        mine_pick, their_pick = mine["sets"][3]["bits"], p["sets"][3]["bits"]
        only_theirs = sum(1 for a, b in zip(mine_pick, their_pick) if b and not a)
        only_mine = sum(1 for a, b in zip(mine_pick, their_pick) if a and not b)
        self.assertEqual(only_theirs, 20 // 4)  # a quarter of the 20 you haven't found
        self.assertEqual(only_mine, 20 // 10)   # a tenth of the 20 you have
        self.assertEqual(p["sets"][1]["bits"], [0, 1, 1])  # two you haven't, yours (the only one) gone
        self.assertEqual(p["sets"][2]["bits"], [0, 1, 0])  # the first of theirs still held
        self.assertEqual(p["sets"][0]["bits"], [1, 1, 0, 0])  # the first boss you haven't defeated is theirs
        self.assertIn({"boss": 1, "deaths": p["bosses"][-1]["deaths"], "ttk": 12, "session": 0,
                       "kill": p["sessions"][0]["start"] + p["sessions"][0]["minutes"] - 5}, p["bosses"])
        # Their two newest sessions start with yours and run 30 minutes longer: played together.
        for k in (0, 1):
            self.assertEqual(p["sessions"][k]["start"], mine["sessions"][k]["start"])
            self.assertEqual(p["sessions"][k]["minutes"], mine["sessions"][k]["minutes"] + 30)
        # The same areas, with their own time and deaths in them.
        self.assertEqual([a["index"] for a in p["areas"]], [a["index"] for a in mine["areas"]])
        self.assertNotEqual([a["seconds"] for a in p["areas"]], [a["seconds"] for a in mine["areas"]])
        self.assertTrue(all(t["deaths"] >= m["deaths"] for t, m in zip(p["areas"], mine["areas"])))
        # Their own weapons (oldest Broadsword +0 and Grass Crest Shield, newest
        # Claymore +10 and Lothric Knight Shield) and records.
        w = ctp.PARTNER_WEAPONS
        self.assertEqual([x["hands"] for x in p["sessions"]],
                         [[[w["Claymore"], 10], [w["Lothric Knight Shield"], 0]], [[w["Broadsword"], 0], [w["Grass Crest Shield"], 0]]])
        self.assertEqual((p["life"]["seconds"], p["firstDeath"]["seconds"], p["sessions"][0]["levelEnd"]), (315, 270, 44))
        # Their own level curve: later (your clock stretched to theirs), a little ahead, ending at their level.
        self.assertEqual([(x["minutes"], x["level"]) for x in p["levels"]], [(1519, 41), (1525, 45), (1525, 47)])
        self.assertEqual([st["fingerprint"] for st in p["sets"]], [st["fingerprint"] for st in mine["sets"]])  # same lists
        # Still a valid code, and the same partner from the same seed.
        self.assertEqual(ctp.decode(ctp.encode(p)), p)
        self.assertEqual(ctp.make_partner(ctp.decode(ctp.encode(mine)), "Isolde", seed=1), p)

    @unittest.skipUnless(os.path.exists(os.path.join(ROOT, "build", "wasd-cli.exe")), "needs the built wasd-cli.exe")
    def test_wasd_reads_the_partner_code(self):
        p = ctp.make_partner(ctp.decode(ctp.encode(sample())), "Isolde")
        code = ctp.to_text(ctp.encode(p, sequence=1))
        out = subprocess.run([os.path.join(ROOT, "build", "wasd-cli.exe"), "coop", code], capture_output=True, text=True,
                             timeout=30).stdout
        self.assertIn("character  Isolde", out)
        self.assertIn("level      47", out)
        self.assertIn("pickups found      23 / 40", out)  # 20 - 2 of yours + 5 of theirs
        self.assertIn("sessions   2, with weapons", out)
        self.assertIn("bosses     2 kill(s) recorded", out)


if __name__ == "__main__":
    unittest.main()
