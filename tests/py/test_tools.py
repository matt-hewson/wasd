"""Tests for the map-file tools (tools/ds3_archive.py, tools/extract_treasures.py).

No game files needed: archives, DCX containers and MSB map files are built
here in memory to the layouts the tools document, with a toy RSA key
standing in for FromSoftware's. Run: python -I -m unittest discover -s tests/py
"""
import base64
import collections
import contextlib
import io
import os
import shutil
import struct
import sys
import tempfile
import unittest
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests", "shared"))

import ds3_archive  # noqa: E402
import extract_treasures as et  # noqa: E402
import ds3_fixtures  # noqa: E402
from ds3_fixtures import (E, N, event, make_bnd4, make_dcx, make_fmg, make_msb, make_param, part,  # noqa: E402
                          rsa_public_b64, rsa_sign_blocks)


# ---- ds3_archive -----------------------------------------------------------

class NameHashTest(unittest.TestCase):
    def test_formula(self):
        self.assertEqual(ds3_archive.name_hash("ab"), (97 * 37 + 98) & 0xFFFFFFFF)
        self.assertEqual(ds3_archive.name_hash(""), 0)

    def test_case_and_separators_do_not_matter(self):
        a = ds3_archive.name_hash("/map/mapstudio/m40_00_00_00.msb.dcx")
        self.assertEqual(a, ds3_archive.name_hash("/MAP/MapStudio/M40_00_00_00.MSB.DCX"))
        self.assertEqual(a, ds3_archive.name_hash("\\map\\mapstudio\\m40_00_00_00.msb.dcx"))

    def test_wraps_to_32_bits(self):
        self.assertLess(ds3_archive.name_hash("x" * 200), 2 ** 32)


class RsaTest(unittest.TestCase):
    def test_der_lengths(self):
        self.assertEqual(ds3_archive._der_len(bytes([0x05]), 0), (5, 1))
        self.assertEqual(ds3_archive._der_len(bytes([0x82, 0x01, 0x0A]), 0), (266, 3))

    def test_shipped_keys_parse(self):
        for name, key in ds3_archive.RSA_KEYS.items():
            n, e = ds3_archive._parse_rsa_public(key)
            self.assertEqual(n.bit_length(), 2048, name)
            self.assertEqual(e % 2, 1, name)

    def test_toy_key_round_trip(self):
        self.assertEqual(ds3_archive._parse_rsa_public(rsa_public_b64(N, E)), (N, E))

    def test_block_decrypt(self):
        plain = bytes(range(256)) * 2  # several blocks, last one padded
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "x.bhd")
            with open(path, "wb") as f:
                f.write(rsa_sign_blocks(plain))
            out = ds3_archive._rsa_decrypt_file(path, rsa_public_b64(N, E))
        self.assertEqual(out[:len(plain)], plain)


@unittest.skipUnless(os.name == "nt", "AES goes through Windows bcrypt.dll")
class AesTest(unittest.TestCase):
    def test_fips_197_vector(self):
        key = bytes(range(16))
        cipher = bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")
        self.assertEqual(ds3_archive._aes_ecb_decrypt(key, cipher), bytes.fromhex("00112233445566778899aabbccddeeff"))

    @unittest.skipUnless(os.name == "nt", "AES comes from Windows")
    def test_cbc_256_nist_vector(self):
        # NIST SP 800-38A, F.2.6 CBC-AES256.Decrypt, first two blocks.
        key = bytes.fromhex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4")
        iv = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
        cipher = bytes.fromhex("f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d")
        plain = bytes.fromhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51")
        self.assertEqual(ds3_archive._aes_cbc_decrypt(key, iv, cipher), plain)
        self.assertEqual(ds3_fixtures.aes_cbc_encrypt(key, iv, plain), cipher)


class ArchiveTest(unittest.TestCase):
    """A two-file archive: a BHD5 header (RSA-signed with the toy key) and its .bdt."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        files = {"/map/a.txt": b"hello map", "/param/b.bin": bytes(range(40))}
        bdt, entries = b"", []
        for path, data in files.items():
            padded = data + b"\0" * (-len(data) % 16)
            entries.append(struct.pack("<IIqqqq", ds3_archive.name_hash(path), len(padded), len(bdt), 0, 0, len(data)))
            bdt += padded
        bucket_off, entry_off = 0x40, 0x48
        hdr = bytearray(0x48)
        hdr[:4] = b"BHD5"
        struct.pack_into("<ii", hdr, 16, 1, bucket_off)
        struct.pack_into("<ii", hdr, bucket_off, len(entries), entry_off)
        hdr += b"".join(entries)
        with open(os.path.join(self.tmp.name, "Test.bhd"), "wb") as f:
            f.write(rsa_sign_blocks(bytes(hdr)))
        with open(os.path.join(self.tmp.name, "Test.bdt"), "wb") as f:
            f.write(bdt)
        ds3_archive.RSA_KEYS["Test"] = rsa_public_b64(N, E)
        self.files = files

    def tearDown(self):
        del ds3_archive.RSA_KEYS["Test"]
        self.tmp.cleanup()

    def test_reads_files_by_path(self):
        ar = ds3_archive.Archive(self.tmp.name, "Test")
        for path, data in self.files.items():
            self.assertTrue(ar.has(path))
            self.assertEqual(ar.read(path), data)  # trimmed to its size, not the padded length
        self.assertTrue(ar.has("/MAP/A.TXT"))
        self.assertFalse(ar.has("/map/missing.txt"))

    def test_rejects_a_header_that_is_not_bhd5(self):
        with open(os.path.join(self.tmp.name, "Test.bhd"), "wb") as f:
            f.write(rsa_sign_blocks(b"NOPE" + b"\0" * 60))
        with self.assertRaises(ValueError):
            ds3_archive.Archive(self.tmp.name, "Test")


class DcxTest(unittest.TestCase):
    def test_deflate(self):
        payload = b"MSB " + bytes(range(256)) * 4
        self.assertEqual(ds3_archive.dcx_decompress(make_dcx(payload)), payload)

    def test_not_dcx_passes_through(self):
        self.assertEqual(ds3_archive.dcx_decompress(b"MSB raw"), b"MSB raw")

    def test_other_compression_is_refused(self):
        with self.assertRaises(NotImplementedError):
            ds3_archive.dcx_decompress(make_dcx(b"x", fmt=b"KRAK"))


# ---- extract_treasures -----------------------------------------------------

class Bnd4Test(unittest.TestCase):
    FILES = [(10, "N:\\FDP\\data\\msg\\engUS\\アイテム名.fmg", b"abc"), (11, "b.fmg", b""), (250, "c.fmg", bytes(range(40)))]

    def test_round_trip(self):
        self.assertEqual(ds3_archive.read_bnd4(make_bnd4(self.FILES)), self.FILES)

    def test_refuses_other_layouts(self):
        good = bytearray(make_bnd4(self.FILES))
        for offset, value in ((0x31, 0x54), (0x09, 1), (0x30, 0), (0x20, 0x18)):
            bad = bytearray(good)
            bad[offset] = value
            with self.assertRaises(NotImplementedError, msg=hex(offset)):
                ds3_archive.read_bnd4(bytes(bad))
        with self.assertRaises(ValueError):
            ds3_archive.read_bnd4(b"BND3" + bytes(good[4:]))

    def test_refuses_compressed_and_broken_entries(self):
        good = make_bnd4(self.FILES)
        bad = bytearray(good)
        struct.pack_into("<q", bad, 0x40 + 16, 99)  # uncompressed size differs
        with self.assertRaises(NotImplementedError):
            ds3_archive.read_bnd4(bytes(bad))
        bad = bytearray(good)
        struct.pack_into("<i", bad, 0x40 + 4, 0)  # the -1 marker
        with self.assertRaises(ValueError):
            ds3_archive.read_bnd4(bytes(bad))
        with self.assertRaises(ValueError):
            ds3_archive.read_bnd4(good[:-16])  # last file cut short (past its 8 bytes of padding)
        bad = bytearray(good)
        struct.pack_into("<i", bad, 0x0C, 1000)  # more files than fit
        with self.assertRaises(ValueError):
            ds3_archive.read_bnd4(bytes(bad))


class FmgTest(unittest.TestCase):
    def test_round_trip_with_groups_gaps_and_empty_text(self):
        texts = {100: "Havel's Ring", 101: "", 103: "Ring of Favor", 2000000: "Dagger", 90000: "Torch"}
        fmg = make_fmg(texts, no_text=(102,))
        self.assertEqual(ds3_archive.read_fmg(fmg), texts)  # 102 has no text: left out

    def test_non_ascii_text(self):
        self.assertEqual(ds3_archive.read_fmg(make_fmg({1: "Café — 竜"})), {1: "Café — 竜"})

    def test_refuses_other_versions_and_broken_files(self):
        good = make_fmg({1: "a", 2: "b", 5: "c"})
        for offset, value in ((2, 1), (1, 1)):  # version 1; big-endian
            bad = bytearray(good)
            bad[offset] = value
            with self.assertRaises(NotImplementedError):
                ds3_archive.read_fmg(bytes(bad))
        with self.assertRaises(ValueError):
            ds3_archive.read_fmg(good + b"\0\0")  # size doesn't match
        bad = bytearray(good)
        struct.pack_into("<i", bad, 0x28 + 8, 50)  # group 0's last id far past its strings
        with self.assertRaises(ValueError):
            ds3_archive.read_fmg(bytes(bad))
        bad = bytearray(good)
        struct.pack_into("<iii", bad, 0x28 + 16, 2, 1, 1)  # group 1 repeats id 1
        with self.assertRaises(ValueError):
            ds3_archive.read_fmg(bytes(bad))
        bad = bytearray(good)
        struct.pack_into("<q", bad, 0x18, 0x30)  # string offsets don't follow the groups
        with self.assertRaises(ValueError):
            ds3_archive.read_fmg(bytes(bad))

    def test_unterminated_string(self):
        fmg = bytearray(make_fmg({1: "abc"}))
        fmg[-2:] = b"x\0"  # overwrite the terminator
        with self.assertRaises(ValueError):
            ds3_archive.read_fmg(bytes(fmg))


def item_msgbnd(overrides=None):
    """A synthetic item_dlc2.msgbnd: every name FMG the extractor reads, with
    a few names each. overrides: {bnd id: {id: text}}."""
    texts = {
        11: {2000000: "Dagger", 2000100: " Heavy Dagger "}, 211: {9000000: "Follower Sabre"}, 251: {9500000: "Murky Hand Scythe"},
        12: {1000: "Shaved"}, 212: {}, 252: {},
        13: {20020: "Havel's Ring", 20030: "Ring of Favor"}, 213: {}, 253: {},
        10: {1000: "Titanite Shard", 490: "Dark Sigil", 7: "\t"}, 210: {}, 250: {2101: "Holy Remains"},
        14: {3000: "Soul Arrow"}, 214: {}, 254: {},
        19: {3000: "High Wall of Lothric", 1000: "Depths", 11000: " "}, 216: {}, 256: {5400: "Round Plaza"},
        18: {}, 215: {}, 255: {},
    }
    texts.update(overrides or {})
    stems = {}
    for category, ids, stem in et.ITEM_NAME_FMGS + [("Place",) + et.PLACE_NAME_FMGS, ("NPC",) + et.NPC_NAME_FMGS]:
        for fid, suffix in zip(ids, ("", "_dlc1", "_dlc2")):
            stems[fid] = "N:\\FDP\\data\\INTERROOT_win64\\msg\\engUS\\64bit\\%s%s.fmg" % (stem, suffix)
    return make_dcx(make_bnd4([(fid, stems[fid], make_fmg(texts[fid])) for fid in sorted(texts)]))


class FakeArchive:
    def __init__(self, files):
        self.files = files

    def has(self, path):
        return path in self.files

    def read(self, path):
        return self.files[path]


class ItemNamesTest(unittest.TestCase):
    def test_every_category_merged_trimmed_and_sorted(self):
        rows = et.read_item_names(FakeArchive({et.ITEM_MSGBND: item_msgbnd()}))
        self.assertEqual(rows, [
            ("Weapon", 2000000, "Dagger"), ("Weapon", 2000100, "Heavy Dagger"),
            ("Weapon", 9000000, "Follower Sabre"), ("Weapon", 9500000, "Murky Hand Scythe"),
            ("Protector", 1000, "Shaved"),
            ("Accessory", 20020, "Havel's Ring"), ("Accessory", 20030, "Ring of Favor"),
            ("Goods", 490, "Dark Sigil"), ("Goods", 1000, "Titanite Shard"), ("Goods", 2101, "Holy Remains"),
            ("Magic", 3000, "Soul Arrow"),
        ])  # goods 7 is only a tab: no name

    def test_a_later_file_wins(self):
        rows = et.read_item_names(FakeArchive({et.ITEM_MSGBND: item_msgbnd({251: {2000000: "Dagger (DLC)"}})}))
        self.assertIn(("Weapon", 2000000, "Dagger (DLC)"), rows)
        self.assertNotIn(("Weapon", 2000000, "Dagger"), rows)

    def test_clean_text(self):
        self.assertEqual(et.clean_text(" a\tb\r\nc "), "a b  c")
        self.assertEqual(et.clean_text(" "), "")

    def test_place_names_without_blank_slots(self):
        self.assertEqual(et.read_place_names(FakeArchive({et.ITEM_MSGBND: item_msgbnd()})),
                         [(1000, "Depths"), (3000, "High Wall of Lothric"), (5400, "Round Plaza")])

    def test_file_with_the_wrong_name_is_refused(self):
        bnd = make_bnd4([(11, "x\\防具名.fmg", make_fmg({1: "a"}))])
        with self.assertRaisesRegex(ValueError, "expected 武器名.fmg"):
            et.read_fmgs(ds3_archive.read_bnd4(bnd), (11,), "武器名")

    def test_missing_file_is_refused(self):
        with self.assertRaisesRegex(ValueError, "not found"):
            et.read_fmgs([], (11,), "武器名")

    def test_extract_names_writes_the_table(self):
        with tempfile.TemporaryDirectory() as tmp:
            logged = []
            n = et.extract_names(tmp, tmp, archive=FakeArchive({et.ITEM_MSGBND: item_msgbnd()}), log=logged.append)
            self.assertEqual(n, (11, None))  # no Data0.bdt here: no bonfire names
            self.assertIn("bonfire names: no Data0.bdt (skipped)", logged[-1])
            path = os.path.join(tmp, "item_names.tsv")
            self.assertEqual(ds3_fixtures.read_tsv(path)[:2], [("Weapon", 2000000, "Dagger"), ("Weapon", 2000100, "Heavy Dagger")])
            with open(path, encoding="utf-8") as f:
                self.assertTrue(f.readline().startswith("# Generated locally"))
            self.assertEqual(sorted(os.listdir(tmp)), ["item_names.tsv", "place_names.tsv"])  # no .tmp left over

    def test_no_data1_archive_is_skipped(self):
        with tempfile.TemporaryDirectory() as tmp:
            logged = []
            self.assertEqual(et.extract_names(tmp, os.path.join(tmp, "out"), log=logged.append), (None, None))
            self.assertIn("no Data1.bhd", logged[0])
            self.assertFalse(os.path.exists(os.path.join(tmp, "out")))


class ParamTest(unittest.TestCase):
    ROWS = [(0, b"\x01" * 8), (5, b"\x02" * 8), (4000000, b"abcdefgh")]

    def test_round_trip(self):
        size, rows = ds3_archive.read_param(make_param("TEST_PARAM_ST", self.ROWS, 8), "TEST_PARAM_ST")
        self.assertEqual((size, rows), (8, self.ROWS))

    def test_no_rows(self):
        self.assertEqual(ds3_archive.read_param(make_param("TEST_PARAM_ST", [], 8), "TEST_PARAM_ST"), (0, []))

    def test_wrong_type_is_refused(self):
        with self.assertRaisesRegex(ValueError, "expected 'OTHER_ST'"):
            ds3_archive.read_param(make_param("TEST_PARAM_ST", self.ROWS, 8), "OTHER_ST")

    def test_broken_headers_are_refused(self):
        good = make_param("TEST_PARAM_ST", self.ROWS, 8)
        bad = bytearray(good)
        bad[0x2C] = 1  # big-endian
        with self.assertRaises(NotImplementedError):
            ds3_archive.read_param(bytes(bad), "TEST_PARAM_ST")
        bad = bytearray(good)
        struct.pack_into("<H", bad, 0x0A, 4)  # one row more than there is
        with self.assertRaises(ValueError):
            ds3_archive.read_param(bytes(bad), "TEST_PARAM_ST")
        bad = bytearray(good)
        struct.pack_into("<I", bad, 0x00, struct.unpack_from("<I", good, 0)[0] - 1)  # rows don't fill evenly
        with self.assertRaises(ValueError):
            ds3_archive.read_param(bytes(bad), "TEST_PARAM_ST")
        bad = bytearray(good)
        struct.pack_into("<q", bad, 0x40 + 24 + 8, 0x40)  # row 1 points somewhere else
        with self.assertRaisesRegex(ValueError, "not where expected"):
            ds3_archive.read_param(bytes(bad), "TEST_PARAM_ST")


REG_NAME = "N:\\FDP\\data\\INTERROOT_win64\\Param\\RemoveParamDesc\\dlc2\\%s"


def bonfire_row(flag, warp, text):
    return struct.pack("<iii", flag, warp, text)


def menu_msgbnd(texts_by_fid):
    stems = {200: "", 232: "_dlc1", 272: "_dlc2"}
    return make_dcx(make_bnd4([(fid, "x\\FDP_メニューテキスト%s.fmg" % stems[fid], make_fmg(texts_by_fid.get(fid, {})))
                               for fid in (200, 232, 272)]))


class RegulationTest(unittest.TestCase):
    FILES = [(0, REG_NAME % "BonfireWarpParam.param", make_param("BONFIRE_WARP_PARAM_ST", [(1, b"x")], 64)),
             (1, REG_NAME % "EquipParamGoods.param", b"goods")]

    @unittest.skipUnless(os.name == "nt", "AES comes from Windows")
    def test_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            ds3_fixtures.write_regulation(tmp, self.FILES)
            self.assertEqual(ds3_archive.read_regulation(tmp), self.FILES)

    @unittest.skipUnless(os.name == "nt", "AES comes from Windows")
    def test_wrong_key_or_size_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, ds3_archive.REGULATION_FILE)
            with open(path, "wb") as f:
                f.write(bytes(16) + ds3_fixtures.aes_cbc_encrypt(b"k" * 32, bytes(16), make_dcx(b"BND4") + bytes(8)))
            with self.assertRaisesRegex(ValueError, "did not decrypt"):
                ds3_archive.read_regulation(tmp)
            with open(path, "wb") as f:
                f.write(bytes(40))
            with self.assertRaisesRegex(ValueError, "not an encrypted regulation"):
                ds3_archive.read_regulation(tmp)

    def test_regulation_file_by_name(self):
        self.assertEqual(ds3_archive.regulation_file(self.FILES, "EquipParamGoods.param"), b"goods")
        with self.assertRaisesRegex(ValueError, "0 files named"):
            ds3_archive.regulation_file(self.FILES, "Goods.param")  # a name, not a suffix of one
        with self.assertRaisesRegex(ValueError, "2 files named"):
            ds3_archive.regulation_file(self.FILES + self.FILES[:1], "BonfireWarpParam.param")


class BonfireNamesTest(unittest.TestCase):
    MENU = {200: {211200: "Firelink Shrine", 211205: "Iudex Gundyr", 211211: " Tower on the Wall "}, 272: {211300: "Filianore's Rest"}}

    def names(self, rows, menu=None):
        reg = [(0, REG_NAME % "BonfireWarpParam.param", make_param("BONFIRE_WARP_PARAM_ST", rows, 64))]
        return et.read_bonfire_names(reg, FakeArchive({et.MENU_MSGBND: menu_msgbnd(menu or self.MENU)}))

    def test_keyed_by_the_last_bonfire_id(self):
        rows = [(0, bonfire_row(14000000, 4001950, 211200)), (1, bonfire_row(14000000, 4001950, 211200)),  # same bonfire twice
                (3, bonfire_row(14000002, 4001952, 211205)), (10, bonfire_row(13000000, 3001955, 211211)),
                (90, bonfire_row(15110000, 5111951, 211300))]
        self.assertEqual(self.names(rows), [(3002955, "Tower on the Wall"), (4002950, "Firelink Shrine"),
                                            (4002952, "Iudex Gundyr"), (5112951, "Filianore's Rest")])

    def test_problems_are_refused(self):
        with self.assertRaisesRegex(ValueError, "two names"):
            self.names([(0, bonfire_row(1, 4001950, 211200)), (1, bonfire_row(1, 4001950, 211205))])
        with self.assertRaisesRegex(ValueError, "not in the menu text"):
            self.names([(0, bonfire_row(1, 4001950, 299999))])
        with self.assertRaisesRegex(ValueError, "no name or warp id"):
            self.names([(0, bonfire_row(1, 0, 211200))])
        with self.assertRaisesRegex(ValueError, "no name or warp id"):
            self.names([(0, bonfire_row(1, 4001950, 211200))], menu={200: {211200: " "}})

    def test_extract_names_writes_both_tables(self):
        reg = [(0, REG_NAME % "BonfireWarpParam.param",
                make_param("BONFIRE_WARP_PARAM_ST", [(3, bonfire_row(14000002, 4001952, 211205))], 64))]
        archive = FakeArchive({et.ITEM_MSGBND: item_msgbnd(), et.MENU_MSGBND: menu_msgbnd(self.MENU)})
        with tempfile.TemporaryDirectory() as tmp:
            n = et.extract_names(None, tmp, archive=archive, regulation=reg, log=lambda *_: None)
            self.assertEqual(n, (11, 1))
            self.assertEqual(ds3_fixtures.read_tsv(os.path.join(tmp, "bonfire_names.tsv")), [(4002952, "Iudex Gundyr")])
            self.assertEqual(sorted(os.listdir(tmp)), ["bonfire_names.tsv", "item_names.tsv", "place_names.tsv"])


def region_row(group):
    return struct.pack("<i", group)


class AreaRulesTest(unittest.TestCase):
    PLACES = {3000: "High Wall of Lothric", 3010: "Lothric Castle", 3015: "Consumed King's Garden",
              3100: "Undead Settlement", 3700: "Irithyll of the Boreal Valley", 3705: "Anor Londo",
              4010: "Untended Graves", 4600: "Grand Roof"}

    def decide(self, regions, groups=None, floors=None):
        floors = {r: collections.Counter(c) for r, c in (floors or {}).items()}
        return et.decide_areas(regions, groups or {}, floors, self.PLACES)

    def test_floors_decide_by_majority(self):
        got = self.decide([300022], floors={300022: {3015: 4, 3010: 1}})
        self.assertEqual(got, {300022: (3015, "its floors")})

    def test_a_tie_falls_through_to_the_group(self):
        got = self.decide([300020, 300021, 300023], groups={300020: 300020, 300021: 300020, 300023: 300020},
                          floors={300020: {3010: 1, 3015: 1}, 300021: {3015: 1}, 300023: {3015: 1}})
        self.assertEqual(got[300020], (3015, "its region group"))

    def test_group_counts_only_floor_named_regions(self):
        # 370003's group has one floor-named region; 370004, decided by the group, doesn't vote.
        got = self.decide([370003, 370004, 370005], groups={370003: 370010, 370004: 370010, 370005: 370010},
                          floors={370003: {3705: 1}})
        self.assertEqual(got[370004], (3705, "its region group"))
        self.assertEqual(got[370005], (3705, "its region group"))

    def test_regions_decided_by_other_rules_do_not_vote_in_the_group(self):
        # Two Untended-world regions (decided by that rule) share a group with one
        # floor-named region; the group's answer must come from the floors alone.
        self.PLACES = {**self.PLACES, 4005: "Firelink Shrine"}
        group = {r: 400020 for r in (400001, 400002, 400101, 400150)}
        got = self.decide(list(group), groups=group, floors={400101: {4005: 1}})
        self.assertEqual(got[400150], (4005, "its region group"))

    def test_map_place_last(self):
        got = self.decide([310020, 301005, 341005, 460000])
        self.assertEqual(got[310020], (3100, "its map's place"))   # m31_00 -> 3100
        self.assertEqual(got[301005], (3010, "its map's place"))   # m30_01 -> 3010
        self.assertEqual(got[460000], (4600, "its map's place"))
        self.assertNotIn(341005, got)  # 3410 has no text: no area

    def test_untended_world_and_overrides_come_first(self):
        got = self.decide([400002, 400102, 300006, 370010], groups={370010: 370020},
                          floors={400002: {3000: 9}, 400102: {3000: 9}, 300006: {3010: 1}})
        self.assertEqual(got[400002], (4010, "m40's Untended Graves world"))
        self.assertEqual(got[400102], (3000, "its floors"))  # the normal-world twin
        self.assertEqual(got[300006][0], 3000)  # by hand: the Dancer's arena
        self.assertTrue(got[300006][1].startswith("by hand: "))
        self.assertEqual(got[370010][0], 3700)

    def test_overrides_have_reasons_and_real_places(self):
        for r, (place, reason) in et.AREA_OVERRIDES.items():
            self.assertTrue(reason, r)
            self.assertIn(place, (3000, 3700, 3900), r)

    def test_nearest_bonfire(self):
        bonfires = [("A", (0, 0, 0)), ("B", (10, 0, 0))]
        self.assertEqual(et.nearest_bonfire([(8, 0, 0), (9, 0, 0)], bonfires), "B")
        self.assertEqual(et.nearest_bonfire([(1, 0, 0), (7, 0, 0)], bonfires), "A")  # middle 4: A
        self.assertIsNone(et.nearest_bonfire([], bonfires))
        self.assertIsNone(et.nearest_bonfire([(1, 1, 1)], []))


def m40_msb():
    """m40: a shared floor (both worlds) named Firelink Shrine, an Untended-only
    floor, a normal-world bonfire standing on the shared floor, an Untended-only
    enemy, and a bonfire parked far below the map."""
    parts = [part("h0000", 5, (0, 0, 0), layer=0b11, region=400002, place=-4005),
             part("h0001", 5, (0, 0, 0), layer=0b10, region=400001),
             part("o000100_0000", 1, (5, 0, 5), layer=0b01, coll=0, entity=4001950),
             part("c1000_0000", 2, (50, 0, 50), layer=0b10, coll=1, entity=4000100),
             part("o000100_0009", 1, (0, -999, 0), layer=0b01, coll=0, entity=4001959)]
    return make_msb([("MODEL_PARAM_ST", []), ("EVENT_PARAM_ST", []), ("PARTS_PARAM_ST", parts)])


class RegionsTest(unittest.TestCase):
    def test_m40_floors_carry_both_worlds(self):
        floors, placed, entities = et.region_evidence(m40_msb(), "m40_00_00_00")
        self.assertEqual(sorted(floors), [(400001, None), (400002, 4005), (400102, 4005)])
        self.assertEqual(sorted(r for r, _ in placed), [400001, 400102, 400102])  # enemy; both bonfires (normal world)
        self.assertIn((4001950, (5.0, 0.0, 5.0), 0b01), entities)

    def archives(self):
        regulation = [
            (0, REG_NAME % "PlayRegionParam.param", make_param("PLAY_REGION_PARAM_ST", [
                (0, region_row(0)), (400001, region_row(0)), (400002, region_row(0)), (400101, region_row(0)),
                (400102, region_row(0)), (310000, region_row(0)), (999999, region_row(0))], 96)),
            (1, REG_NAME % "BonfireWarpParam.param", make_param("BONFIRE_WARP_PARAM_ST", [
                (0, bonfire_row(14000000, 4001950, 211200)), (1, bonfire_row(14000009, 4001959, 211201))], 64)),
        ]
        maps = FakeArchive({"/map/mapstudio/m40_00_00_00.msb.dcx": make_dcx(m40_msb()),
                            "/map/mapstudio/m31_00_00_00.msb.dcx": make_dcx(make_msb([
                                ("MODEL_PARAM_ST", []), ("EVENT_PARAM_ST", []),
                                ("PARTS_PARAM_ST", [part("h0000", 5, (0, 0, 0), region=310000, place=3100)])]))})
        text = FakeArchive({et.ITEM_MSGBND: item_msgbnd({19: {3100: "Undead Settlement", 4005: "Firelink Shrine",
                                                              4010: "Untended Graves"}}),
                            et.MENU_MSGBND: menu_msgbnd({200: {211200: "Firelink Shrine", 211201: "Parked"}})})
        return [("Fake", maps)], regulation, text

    def test_labels_and_areas(self):
        maps, regulation, text = self.archives()
        labels, areas = et.read_regions(maps, regulation, dict(et.read_place_names(text)),
                                        dict(et.read_bonfire_names(regulation, text)))
        # 400101 has no floor here (only in the param), 999999 no floor at all: left out.
        self.assertEqual(labels, [(310000, "Undead Settlement"), (400001, "Untended Graves"),
                                  (400002, "Untended Graves"),
                                  (400102, "Firelink Shrine")])  # "(near Firelink Shrine)" dropped: same name
        self.assertEqual(areas, [(310000, "Undead Settlement"), (400001, "Untended Graves"),
                                 (400002, "Untended Graves"), (400102, "Firelink Shrine")])

    def test_extract_regions_writes_both_files(self):
        maps, regulation, text = self.archives()
        with tempfile.TemporaryDirectory() as tmp:
            areas_out = os.path.join(tmp, "region_areas.tsv")
            n = et.extract_regions(None, tmp, archives=maps, text_archive=text, regulation=regulation,
                                   areas_out=areas_out, log=lambda *_: None)
            self.assertEqual((len(n[0]), len(n[1])), (4, 4))  # (labels, areas)
            self.assertEqual(ds3_fixtures.read_tsv(os.path.join(tmp, "regions.tsv"))[0], (310000, "Undead Settlement"))
            with open(areas_out, encoding="utf-8") as f:
                text_out = f.read()
            self.assertTrue(text_out.startswith(et.AREAS_HEADER))
            self.assertIn("400102\tFirelink Shrine\n", text_out)
            self.assertEqual(sorted(os.listdir(tmp)), ["region_areas.tsv", "regions.tsv"])

    def test_arenas_are_labelled_but_have_no_area(self):
        maps, regulation, text = self.archives()
        regulation[0] = (0, REG_NAME % "PlayRegionParam.param",
                         make_param("PLAY_REGION_PARAM_ST", [(460000, region_row(460010))], 96))
        maps[0][1].files["/map/mapstudio/m46_00_00_00.msb.dcx"] = make_dcx(make_msb([
            ("MODEL_PARAM_ST", []), ("EVENT_PARAM_ST", []), ("PARTS_PARAM_ST", [part("h0000", 5, (0, 0, 0), region=460000)])]))
        labels, areas = et.read_regions(maps, regulation, {4600: "Grand Roof"}, {})
        self.assertEqual((labels, areas), ([(460000, "Grand Roof")], []))


class EmevdTest(unittest.TestCase):
    def test_finds_the_wanted_instructions(self):
        e = ds3_fixtures.make_emevd([(2003, 11, ds3_fixtures.health_bar(4000800, 905110)),
                                     (1000, 3, b"\x01\x00\x00\x00"),
                                     (2003, 11, ds3_fixtures.health_bar(4000830, 905115, slot=1))])
        found = et.read_emevd_instructions(e, (2003, 11))
        self.assertEqual([struct.unpack("<B3xihxxi", a)[1:] for a in found], [(4000800, 0, 905110), (4000830, 1, 905115)])
        self.assertEqual(et.read_emevd_instructions(e, (9, 9)), [])

    def test_refuses_other_files(self):
        good = ds3_fixtures.make_emevd([(2003, 11, ds3_fixtures.health_bar(1, 2))])
        with self.assertRaises(NotImplementedError):
            et.read_emevd_instructions(b"EVD\0\x01" + good[5:], (2003, 11))  # big-endian
        with self.assertRaises(NotImplementedError):
            et.read_emevd_instructions(ds3_fixtures.make_emevd([], version=0xCC), (2003, 11))
        with self.assertRaises(ValueError):
            et.read_emevd_instructions(good[:-8], (2003, 11))  # arguments cut short


def boss_maps(extra_enemies=()):
    """m40 with Iudex (4000800) and Champion Gundyr (4000830) on floors of
    regions 400100 / 400001; m33 with the Abyss Watchers' parts (3300801-2)."""
    m40 = [part("h0000", 5, (0, 0, 0), layer=0b01, region=400000),   # normal world: 400100
           part("h0001", 5, (0, 0, 0), layer=0b10, region=400001),   # Untended only
           part("c5110_0000", 2, (1, 0, 1), layer=0b01, coll=0, entity=4000800, npc=511000),
           part("c5110_0001", 2, (2, 0, 2), layer=0b10, coll=1, entity=4000830, npc=511100)]
    m33 = [part("h0000", 5, (0, 0, 0), region=330010),
           part("c3040_0000", 2, (0, 0, 0), coll=0, entity=3300801),
           part("c3040_0001", 2, (0, 0, 0), coll=0, entity=3300802)] + list(extra_enemies)
    files = {}
    for m, parts in (("m40_00_00_00", m40), ("m33_00_00_00", m33)):
        files["/map/mapstudio/%s.msb.dcx" % m] = make_dcx(make_msb(
            [("MODEL_PARAM_ST", []), ("EVENT_PARAM_ST", []), ("PARTS_PARAM_ST", parts)]))
    return [("Fake", FakeArchive(files))]


def boss_text(bars_m33=None, npc=None):
    bar = ds3_fixtures.health_bar
    names = {905110: "Iudex Gundyr", 905115: "Champion Gundyr", 903040: "Abyss Watchers", 905251: "Twin"}
    names.update(npc or {})
    return FakeArchive({
        et.ITEM_MSGBND: item_msgbnd({18: names, 215: {}, 255: {}}),
        "/event/m40_00_00_00.emevd.dcx": make_dcx(ds3_fixtures.make_emevd(
            [(2003, 11, bar(4000800, 905110)), (2003, 11, bar(4000800, 905110)),  # the same bar twice
             (2003, 11, bar(4000830, 905115)), (2003, 11, bar(4000830, 905110, state=0))])),  # a hidden bar: ignored
        "/event/m33_00_00_00.emevd.dcx": make_dcx(ds3_fixtures.make_emevd(
            bars_m33 if bars_m33 is not None else [(2003, 11, bar(3300801, 903040))])),
    })


AREAS = {400100: "Cemetery of Ash", 400001: "Untended Graves", 330010: "Farron Keep"}


class BossesTest(unittest.TestCase):
    def test_names_and_areas(self):
        rows = et.read_bosses(boss_maps(), boss_text(), [14000800, 14000830, 13300800], AREAS)
        self.assertEqual(rows, [(14000800, "Iudex Gundyr", "Cemetery of Ash"),
                                (14000830, "Champion Gundyr", "Untended Graves"),  # its own bar, its own world
                                (13300800, "Abyss Watchers", "Farron Keep")])      # bar on entity + 1

    def test_several_names_need_a_call(self):
        text = boss_text(bars_m33=[(2003, 11, ds3_fixtures.health_bar(3300801, 903040)),
                                   (2003, 11, ds3_fixtures.health_bar(3300802, 905251))])
        with self.assertRaisesRegex(ValueError, "add a call to BOSS_NAME_CALLS"):
            et.read_bosses(boss_maps(), text, [13300800], AREAS)
        calls = dict(et.BOSS_NAME_CALLS)
        try:
            et.BOSS_NAME_CALLS[13300800] = (903040, 905251)
            self.assertEqual(et.read_bosses(boss_maps(), text, [13300800], AREAS)[0][1], "Abyss Watchers and Twin")
            et.BOSS_NAME_CALLS[13300800] = (999999,)
            with self.assertRaisesRegex(ValueError, "not in the NPC-name text"):
                et.read_bosses(boss_maps(), text, [13300800], AREAS)
        finally:
            et.BOSS_NAME_CALLS.clear()
            et.BOSS_NAME_CALLS.update(calls)

    def test_problems_are_refused(self):
        with self.assertRaisesRegex(ValueError, "no enemy"):
            et.read_bosses(boss_maps(), boss_text(), [15000800], AREAS)
        with self.assertRaisesRegex(ValueError, "no name"):
            et.read_bosses(boss_maps(), boss_text(bars_m33=[]), [13300800], AREAS)
        with self.assertRaisesRegex(ValueError, "no region with an area"):
            et.read_bosses(boss_maps(), boss_text(), [13300800], {})

    def test_calls_are_text_ids_with_a_reason(self):
        for flag, ids in et.BOSS_NAME_CALLS.items():
            self.assertTrue(ids and all(isinstance(i, int) and i > 0 for i in ids), flag)

    def test_boss_flags_and_extract(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "bosses.tsv")
            with open(path, "w", encoding="utf-8") as f:
                f.write("# flag\tarea\tstatus\tnote\n14000800\tCemetery of Ash\trequired\n\n13300800\tFarron Keep\trequired\n")
            self.assertEqual(et.boss_flags(path), [14000800, 13300800])
            rows = et.extract_bosses(None, tmp, AREAS, archives=boss_maps(), text_archive=boss_text(),
                                     flags=[14000800, 13300800], log=lambda *_: None)
            self.assertEqual(len(rows), 2)
            self.assertEqual(ds3_fixtures.read_tsv(os.path.join(tmp, "boss_names.tsv")),
                             [(14000800, "Iudex Gundyr"), (13300800, "Abyss Watchers")])

    def test_the_real_boss_list_has_flags(self):
        flags = et.boss_flags()
        self.assertEqual(len(flags), 25)
        self.assertEqual(len(set(flags)), 25)


class KeyItemIdsTest(unittest.TestCase):
    def goods(self, rows):
        def row(category):
            b = bytearray(128)
            b[et.GOODS_CATEGORY] = category
            return bytes(b)
        return [(0, REG_NAME % "EquipParamGoods.param",
                 make_param("EQUIP_PARAM_GOODS_ST", [(i, row(c)) for i, c in rows], 128))]

    def test_the_category_plus_the_ones_by_hand(self):
        reg = self.goods([(1000, 2), (2010, 1), (2013, 1), (2118, 0), (3000, 5), (240, 0)])
        self.assertEqual(et.read_key_item_ids(reg), [2010, 2013, 2118])  # 2118: Loretta's Bone, by hand

    def test_by_hand_entries_have_reasons(self):
        for i, reason in et.KEY_ITEMS_BY_HAND.items():
            self.assertTrue(reason, i)

    def test_the_committed_list_has_one_row_per_id(self):
        with open(os.path.join(ROOT, "data", "key_items.tsv"), encoding="utf-8") as f:
            rows = [l.rstrip("\n").split("\t") for l in f if l.strip() and not l.startswith("#")]
        ids = [int(r[0]) for r in rows]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertIn(2118, ids)
        self.assertTrue(all(len(r) == 3 and r[1] in ("key", "tome", "quest", "shop", "other") and r[2] for r in rows))


class MsbTest(unittest.TestCase):
    def msb(self, parts, events):
        return make_msb([("MODEL_PARAM_ST", []), ("EVENT_PARAM_ST", events), ("PARTS_PARAM_ST", parts)])

    def test_params_by_name(self):
        params = et.read_params(self.msb([part("c0", 5, (0, 0, 0), region=1)], [event(4)]))
        self.assertEqual(sorted(params), ["EVENT_PARAM_ST", "MODEL_PARAM_ST", "PARTS_PARAM_ST"])
        self.assertEqual(len(params["PARTS_PARAM_ST"]), 1)
        with self.assertRaises(ValueError):
            et.read_params(b"NOPE" + b"\0" * 32)

    def test_treasure_takes_its_part_position_and_play_region(self):
        parts = [part("h0000", 5, (0, 0, 0), region=300001),
                 part("o0001", 1, (10.5, -2.0, 33.25), coll=0)]
        events = [event(4, part_idx=1, lot1=4000050, lot2=0),
                  event(3, part_idx=1, lot1=999),             # not a treasure
                  event(4, part_idx=1, lot1=0, lot2=-1)]      # no lot: skipped
        self.assertEqual(et.treasures(self.msb(parts, events), "m30_00_00_00"),
                         [(4000050, 0, 10.5, -2.0, 33.25, 300001, "o0001")])

    def test_untended_graves_and_cemetery_share_m40(self):
        # Collisions carry the Untended id; layer bit 0 set = the normal world (+100).
        parts = [part("h0000", 5, (0, 0, 0), region=400001),
                 part("normal", 1, (1, 0, 0), layer=1, coll=0),
                 part("untended", 1, (2, 0, 0), layer=0, coll=0)]
        events = [event(4, part_idx=1, lot1=1), event(4, part_idx=2, lot1=2)]
        regions = {t[0]: t[5] for t in et.treasures(self.msb(parts, events), "m40_00_00_00")}
        self.assertEqual(regions, {1: 400101, 2: 400001})
        # Outside m40 the layer bit means nothing.
        regions = {t[0]: t[5] for t in et.treasures(self.msb(parts, events), "m30_00_00_00")}
        self.assertEqual(regions, {1: 400001, 2: 400001})

    def test_unlinked_part_takes_the_nearest_linked_region(self):
        parts = [part("h0", 5, (0, 0, 0), region=300001),
                 part("h1", 5, (0, 0, 0), region=300002),
                 part("near0", 1, (0, 0, 0), coll=0),
                 part("near1", 1, (100, 0, 0), coll=1),
                 part("dummy", 9, (90, 0, 0))]  # no collision link
        events = [event(4, part_idx=4, lot1=7)]
        self.assertEqual(et.treasures(self.msb(parts, events), "m30_00_00_00")[0][5], 300002)

    def test_bad_part_index_gives_unknown_position(self):
        events = [event(4, part_idx=5, lot1=7)]
        (t,) = et.treasures(self.msb([part("h0", 5, (0, 0, 0), region=1)], events), "m30_00_00_00")
        self.assertEqual((t[0], t[5], t[6]), (7, -1, ""))
        self.assertNotEqual(t[2], t[2])  # NaN

    def test_enemies(self):
        parts = [part("h0", 5, (0, 0, 0), region=1),
                 part("c1000_0000", 2, (4, 5, 6), coll=0, entity=3000800, think=100000, npc=100100)]
        self.assertEqual(et.enemies(self.msb(parts, [])), [(3000800, 100100, 100000, 4.0, 5.0, 6.0, "c1000_0000")])


class ExtractTest(unittest.TestCase):
    """The extractor end to end on a synthetic game folder."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="wasd extract tést ")
        self.game = os.path.join(self.tmp, "Game")
        self.out = os.path.join(self.tmp, "out")
        self.keys_before = dict(ds3_archive.RSA_KEYS)
        self.env_before = os.environ.get("WASD_RSA_KEYS_FILE")
        os.environ["WASD_RSA_KEYS_FILE"] = ds3_fixtures.make_game(self.game)

    def tearDown(self):
        ds3_archive.RSA_KEYS.clear()
        ds3_archive.RSA_KEYS.update(self.keys_before)
        if self.env_before is None:
            os.environ.pop("WASD_RSA_KEYS_FILE", None)
        else:
            os.environ["WASD_RSA_KEYS_FILE"] = self.env_before
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_writes_both_files_into_out(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(et.main([self.game, "--out", self.out]), 0)
        self.assertEqual(ds3_fixtures.read_tsv(os.path.join(self.out, "treasures.tsv")), ds3_fixtures.EXPECTED_TREASURES)
        self.assertEqual(ds3_fixtures.read_tsv(os.path.join(self.out, "enemies.tsv")), ds3_fixtures.EXPECTED_ENEMIES)
        self.assertEqual(sorted(os.listdir(self.out)), ["enemies.tsv", "treasures.tsv"])  # no .tmp left over

    def test_out_leaves_the_repo_alone(self):
        repo_file = os.path.join(ROOT, "data", "generated", "treasures.tsv")
        before = os.path.getmtime(repo_file) if os.path.exists(repo_file) else None
        with contextlib.redirect_stdout(io.StringIO()):
            et.main([self.game, "--out", self.out])
        self.assertEqual(before, os.path.getmtime(repo_file) if os.path.exists(repo_file) else None)

    def test_missing_game_folder_and_missing_archives(self):
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            self.assertEqual(et.main([os.path.join(self.tmp, "nope"), "--out", self.out]), 2)
            os.makedirs(os.path.join(self.tmp, "empty"))
            self.assertEqual(et.main([os.path.join(self.tmp, "empty"), "--out", self.out]), 3)
        self.assertIn("Game folder not found", printed.getvalue())
        self.assertIn("No map archives", printed.getvalue())
        self.assertFalse(os.path.exists(self.out))

    def test_unreadable_archive(self):
        with open(os.path.join(self.game, "Data5.bhd"), "wb") as f:
            f.write(b"\0" * 27)  # decrypts to garbage, not BHD5
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            self.assertEqual(et.main([self.game, "--out", self.out]), 4)
        self.assertIn("Couldn't read the game's archives", printed.getvalue())

    def test_extract_with_given_archives(self):
        class Fake:
            def __init__(self, files):
                self.files = files

            def has(self, path):
                return path in self.files

            def read(self, path):
                return self.files[path]

        msb = make_dcx(ds3_fixtures.game_msb())
        logged = []
        n = et.extract(None, self.out, archives=[("Fake", Fake({"/map/mapstudio/m30_00_00_00.msb.dcx": msb}))],
                       log=logged.append)
        self.assertEqual(n, (1, 1))
        self.assertTrue(any("m31_00_00_00: not found (skipped)" in line for line in logged))


if __name__ == "__main__":
    unittest.main()
