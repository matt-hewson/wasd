"""Real-data check, local and optional: the bundled,
trimmed Python (build\\python) and the dev Python (the one running this
test) run tools\\extract_treasures.py on your actual DS3 install, and must
write identical treasures.tsv, enemies.tsv and the name tables (item, bonfire
and place names).

The game folder: WASD_GAME_DIR, else game_dir in the repo's settings.ini
(saved by `wasd-cli.exe extract`). Skipped when neither has Data5.bhd.
Run: .\\test.ps1 -Only realdata
"""
import filecmp
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUNDLED = os.path.join(ROOT, "build", "python", "python.exe")
SCRIPT = os.path.join(ROOT, "tools", "extract_treasures.py")


def game_dir():
    d = os.environ.get("WASD_GAME_DIR")
    if not d and os.path.exists(os.path.join(ROOT, "settings.ini")):
        with open(os.path.join(ROOT, "settings.ini"), encoding="utf-8") as f:
            for line in f:
                if line.startswith("game_dir="):
                    d = line.strip()[len("game_dir="):]
    return d if d and os.path.exists(os.path.join(d, "Data5.bhd")) else None


def early_ends(ins, defeat, reached):
    """Where an event script can end before reached(bank, id, args) holds -- its id flag set with
    no fight or phase -- other than while the boss is defeated or the player is a phantom (an end on
    a condition group of character-type checks on the player). Walks every branch: 1000:1/2/3/4/101/103
    (skip / end / goto on a condition group or always), 1003:1/2/101 (on a flag; flag type != 0 is the
    event's own flag, off on its first run), 1003:12/13/14 (on the player's world: both ways), 1014
    (labels). ins: [(bank, id, args)]."""
    import struct

    def sb(v):
        return struct.unpack("<b", bytes([v]))[0]

    labels = {}
    for k, (b, i, _a) in enumerate(ins):
        if b == 1014:
            labels.setdefault(i, k)

    def phantom_group(k, g):
        conds = [(b, i, a) for b, i, a in ins[:k] if b in (3, 4) and a and sb(a[0]) == g]
        return conds and all((b, i) in ((4, 3), (4, 27)) and struct.unpack_from("<i", a, 4)[0] == 10000
                             for b, i, a in conds)

    bad, seen, stack = set(), set(), [(0, None)]  # (pc, defeated: True / False / None unknown)
    while stack:
        pc, dead = stack.pop()
        while (pc, dead) not in seen:
            seen.add((pc, dead))
            if pc >= len(ins):
                if not dead:
                    bad.add(pc)
                break
            b, i, a = ins[pc]
            if reached(b, i, a):
                break
            nxt = pc + 1
            flag = struct.unpack_from("<i", a, 4)[0] if len(a) >= 8 else None
            if b == 1003 and i in (1, 2, 101) and a[2] != 0:  # the event's own flag: off
                if a[1] == 0:
                    if i == 2:
                        if not dead:
                            bad.add(pc)
                        break
                    nxt = pc + 1 + a[0] if i == 1 else labels.get(a[0], len(ins))
            elif (b, i) in ((1000, 4), (1003, 13)):
                if not dead:
                    bad.add(pc)
                break
            elif (b, i) == (1000, 2):
                if not dead and not phantom_group(pc, sb(a[2])):
                    bad.add(pc)
            elif (b, i) == (1003, 2):
                if flag == defeat:
                    if a[1] == 0 and not dead:
                        bad.add(pc)
                    dead = a[1] != 1
                elif not dead:
                    bad.add(pc)
            elif (b, i) == (1000, 3):
                nxt = pc + 1 + a[0]
            elif (b, i) == (1000, 103):
                nxt = labels.get(a[0], len(ins))
            elif (b, i) in ((1000, 1), (1003, 1), (1003, 14), (1000, 101), (1003, 101), (1003, 12)):
                dest = pc + 1 + a[0] if i in (1, 14) else labels.get(a[0], len(ins))
                taken = dead
                if b == 1003 and i in (1, 101) and flag == defeat:
                    taken, dead = a[1] == 1, a[1] != 1
                stack.append((dest, taken))
            pc = nxt
    return sorted(bad)


@unittest.skipUnless(game_dir(), "DS3 not found: set WASD_GAME_DIR or run `wasd-cli.exe extract` once")
@unittest.skipUnless(os.path.exists(BUNDLED), "no build\\python: run tools\\fetch_python.ps1")
class RealDataTest(unittest.TestCase):
    def test_bundled_python_matches_the_dev_python(self):
        tmp = tempfile.mkdtemp(prefix="wasd realdata ")
        try:
            outs = {}
            for name, python in (("bundled", BUNDLED), ("dev", sys.executable)):
                outs[name] = os.path.join(tmp, name)
                r = subprocess.run([python, "-I", "-B", SCRIPT, game_dir(), "--out", outs[name],
                                    "--region-areas", os.path.join(outs[name], "region_areas.tsv")],
                                   capture_output=True, text=True, timeout=600)
                self.assertEqual(r.returncode, 0, "%s: %s%s" % (name, r.stdout, r.stderr))
            for f in ("treasures.tsv", "enemies.tsv", "item_names.tsv", "bonfire_names.tsv", "place_names.tsv",
                      "regions.tsv", "region_areas.tsv", "boss_names.tsv"):
                self.assertTrue(filecmp.cmp(os.path.join(outs["bundled"], f), os.path.join(outs["dev"], f), shallow=False),
                                "%s differs between the bundled and the dev Python" % f)
            with open(os.path.join(outs["bundled"], "treasures.tsv"), encoding="utf-8") as f:
                rows = [l for l in f if not l.startswith("#")]
            self.assertGreater(len(rows), 900)  # 1,005 on the full game with both DLCs
            # Item names from the game's own text: 5,907
            # on the full game; the rings the equip-load code finds by name.
            with open(os.path.join(outs["bundled"], "item_names.tsv"), encoding="utf-8") as f:
                names = [l.rstrip("\n").split("\t") for l in f if not l.startswith("#")]
            self.assertGreater(len(names), 5000)
            self.assertEqual(len({(c, i) for c, i, _ in names}), len(names), "duplicate ids")
            for ring in ("Havel's Ring", "Ring of Favor"):
                self.assertIn(ring, {n for c, _, n in names if c == "Accessory"})
            # Bonfire names (BonfireWarpParam -> menu text): 81 on the full game.
            # 4002952 is the bonfire seen in game at Iudex Gundyr (README "Current area").
            with open(os.path.join(outs["bundled"], "bonfire_names.tsv"), encoding="utf-8") as f:
                bonfires = dict(l.rstrip("\n").split("\t") for l in f if not l.startswith("#"))
            self.assertGreater(len(bonfires), 75)
            self.assertEqual(bonfires.get("4002952"), "Iudex Gundyr")
            self.assertEqual(bonfires.get("3702955"), "Distant Manor")
            # Place names (area banners): 63 on the full game.
            with open(os.path.join(outs["bundled"], "place_names.tsv"), encoding="utf-8") as f:
                places = dict(l.rstrip("\n").split("\t") for l in f if not l.startswith("#"))
            self.assertGreater(len(places), 50)
            self.assertEqual(places.get("4000"), "Cemetery of Ash")
            # Regions: 126 labelled; 400102 is where the
            # game put the player in normal Firelink Shrine (README "Current area").
            with open(os.path.join(outs["bundled"], "regions.tsv"), encoding="utf-8") as f:
                regions = dict(l.rstrip("\n").split("\t") for l in f if not l.startswith("#"))
            self.assertGreater(len(regions), 100)
            self.assertTrue(regions.get("400102", "").startswith("Firelink Shrine"))
            # The committed area table is exactly what the game's files give.
            self.assertTrue(filecmp.cmp(os.path.join(outs["bundled"], "region_areas.tsv"),
                                        os.path.join(ROOT, "data", "region_areas.tsv"), shallow=False),
                            "data/region_areas.tsv differs from what the game's files give: rewrite it with "
                            "tools/extract_treasures.py --region-areas data/region_areas.tsv and review the diff")
            # Bosses: a name for each, as on its health bar.
            with open(os.path.join(outs["bundled"], "boss_names.tsv"), encoding="utf-8") as f:
                boss_names = dict(l.rstrip("\n").split("\t") for l in f if not l.startswith("#"))
            self.assertEqual(len(boss_names), 25)
            self.assertEqual(boss_names.get("14000830"), "Champion Gundyr")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_committed_boss_areas_are_what_the_game_gives(self):
        # data/bosses.tsv's area column must equal where each boss stands in
        # the game's map files (extract_treasures.read_bosses).
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import ds3_archive
        import extract_treasures as et
        game = game_dir()
        archives = [(n, ds3_archive.Archive(game, n)) for n in ("DLC2", "DLC1", "Data5")]
        text = ds3_archive.Archive(game, "Data1")
        regulation = ds3_archive.read_regulation(game)
        _labels, areas = et.read_regions(archives, regulation, dict(et.read_place_names(text)),
                                         dict(et.read_bonfire_names(regulation, text)))
        derived = {flag: area for flag, _name, area in et.read_bosses(archives, text, et.boss_flags(), dict(areas))}
        with open(os.path.join(ROOT, "data", "bosses.tsv"), encoding="utf-8") as f:
            committed = {int(l.split("\t")[0]): l.rstrip("\n").split("\t")[1] for l in f if l.strip() and not l.startswith("#")}
        self.assertEqual(committed, derived)

    def test_committed_key_items_are_the_games_category(self):
        # data/key_items.tsv lists the game's key-item category plus the
        # ones added by hand (extract_treasures.read_key_item_ids).
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import ds3_archive
        import extract_treasures as et
        derived = et.read_key_item_ids(ds3_archive.read_regulation(game_dir()))
        with open(os.path.join(ROOT, "data", "key_items.tsv"), encoding="utf-8") as f:
            committed = sorted(int(l.split("\t")[0]) for l in f if l.strip() and not l.startswith("#"))
        self.assertEqual(committed, derived)
        self.assertEqual(len(derived), 67)

    def test_committed_boss_fight_and_phase_flags_are_in_the_event_scripts(self):
        # data/bosses.tsv's fight flags: each is a health-bar event of the
        # boss's map (its id is set when it ends) that can't end before the bar
        # (early_ends), or a flag set after a health bar and by no event that
        # shows none (one may set it just before its bar). Phase flags: an
        # event of that map that can't end before its first wait, or a flag
        # one of its events sets.
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import ds3_archive
        import extract_treasures as et
        text = ds3_archive.Archive(game_dir(), "Data1")
        bars = {(2003, 11), (2003, 80)}  # show a boss health bar (80: a rider's, the Gravetender's)

        def shows_bar(b, i, a):
            return (b, i) in bars and a[:1] == b"\x01"

        def waits(b, i, a):  # a wait on the main condition group
            return a[:1] == b"\x00" and (b in (3, 4) or (b, i) == (0, 0))

        def sets_on(bank, iid, args):  # the flag a set-flag instruction turns on, else None
            if (bank, iid) in ((2003, 2), (2003, 62)) and len(args) >= 5 and args[4] == 1:
                return int.from_bytes(args[:4], "little")
            return None

        with open(os.path.join(ROOT, "data", "bosses.tsv"), encoding="utf-8") as f:
            rows = [l.rstrip("\n").split("\t") for l in f if l.strip() and not l.startswith("#")]
        self.assertEqual(len(rows), 25)
        for row in rows:
            flag = int(row[0])
            m = "m%02d_%02d_00_00" % (flag // 100000 % 100, flag // 10000 % 10)
            events = et.read_emevd_events(ds3_archive.dcx_decompress(text.read("/event/%s.emevd.dcx" % m)))
            after_bar, barless_sets, phase_ok = set(), set(), set(events)
            for eid, ins in events.items():
                shown = next((k for k, x in enumerate(ins) if shows_bar(*x)), None)
                if shown is None:
                    barless_sets.update(sets_on(*x) for x in ins)
                else:
                    after_bar.update(sets_on(*x) for x in ins[shown + 1:])
                phase_ok.update(sets_on(*x) for x in ins)
            for f in map(int, row[4].split(",")):
                if f in events:
                    self.assertTrue(any(shows_bar(*x) for x in events[f]), "%d fight %d: no health bar" % (flag, f))
                    self.assertEqual(early_ends(events[f], flag, shows_bar), [], "%d fight %d ends early" % (flag, f))
                else:
                    self.assertIn(f, after_bar, "%d fight %d" % (flag, f))
                    self.assertNotIn(f, barless_sets, "%d fight %d: also set by an event with no bar" % (flag, f))
            for f in (map(int, row[5].split(",")) if len(row) > 5 and row[5] else []):
                self.assertIn(f, phase_ok, "%d phase %d" % (flag, f))
                if f in events:
                    self.assertEqual(early_ends(events[f], flag, waits), [], "%d phase %d ends early" % (flag, f))

if __name__ == "__main__":
    unittest.main()
