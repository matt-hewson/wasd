"""Tests for tools/find_patterns.py on a small PE image built here: no game code needed.
Run: python -I -m unittest discover -s tests/py
"""
import contextlib
import io
import os
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import find_patterns as fp  # noqa: E402

TEXT, DATA = 0x1000, 0x2000
CELL, OTHER_CELL, XA = 0x2010, 0x2020, 0x1F90


def pe_image(code):
    """A minimal PE image (layout as loaded): headers, an executable .text at 0x1000 holding `code` (rest int3),
    a .data at 0x2000."""
    img = bytearray(0x3000)
    img[0:2] = b"MZ"
    struct.pack_into("<I", img, 0x3C, 0x40)
    pe = 0x40
    img[pe:pe + 4] = b"PE\0\0"
    struct.pack_into("<HH", img, pe + 4, 0x8664, 2)  # machine, section count
    struct.pack_into("<H", img, pe + 20, 0xF0)  # optional header size
    table = pe + 24 + 0xF0
    for k, (name, rva, flags) in enumerate([(b".text", TEXT, 0x60000020), (b".data", DATA, 0xC0000040)]):
        s = table + 40 * k
        img[s:s + 8] = name.ljust(8, b"\0")
        struct.pack_into("<IIII", img, s + 8, 0x1000, rva, 0x1000, rva)
        struct.pack_into("<I", img, s + 36, flags)
    img[TEXT:TEXT + 0x1000] = b"\xCC" * 0x1000
    for at, data in code:
        img[at:at + len(data)] = data
    return bytes(img)


def rip(at, op, modrm, target, imm=b""):
    """REX.W op modrm disp32 [imm] at `at`, referring RIP-relatively to `target`."""
    length = 7 + len(imm)
    return at, bytes([0x48, op, modrm]) + struct.pack("<i", target - (at + length)) + imm


def with_tail(ins, tail):
    return ins[0], ins[1] + tail


CODE = [
    # mov rax,[rip+CELL]; test rax,rax; je; ret -- and the same head again with a different tail
    with_tail(rip(0x1100, 0x8B, 0x05, CELL), b"\x48\x85\xC0\x74\x05\xC3"),
    with_tail(rip(0x1200, 0x8B, 0x05, CELL), b"\x48\x8B\x40\x08\xC3"),
    # an unrelated load of another cell with the first tail: the head alone isn't unique
    with_tail(rip(0x1300, 0x8B, 0x05, OTHER_CELL), b"\x48\x85\xC0\x74\x05\xC3"),
    # mov qword [rip+OTHER_CELL], 0 (imm32): length 11
    rip(0x1400, 0xC7, 0x05, OTHER_CELL, b"\0\0\0\0"),
    # mov rax,[rbx+XA]; mov rdx,[rax]; call rel32; ret
    (0x1500, b"\x48\x8B\x83" + struct.pack("<i", XA) + b"\x48\x8B\x10\xE8\x11\x22\x33\x44\xC3"),
    (0x1600, b"\x48\x8B\x83" + struct.pack("<i", XA) + b"\x48\x8B\x10\xE8\x55\x66\x77\x08\x90"),
]


class FindPatternsTest(unittest.TestCase):
    def setUp(self):
        self.img = pe_image(CODE)
        self.sections = fp.code_sections(self.img)

    def test_code_sections_are_the_executable_ones(self):
        self.assertEqual(self.sections, [(TEXT, TEXT + 0x1000)])

    def test_references_find_every_rip_load_of_the_cell(self):
        self.assertEqual(fp.references(self.img, self.sections, "rip", CELL), [(0x1100, 7), (0x1200, 7)])
        self.assertEqual(fp.references(self.img, self.sections, "rip", OTHER_CELL), [(0x1300, 7), (0x1400, 11)])

    def test_references_find_struct_offsets(self):
        self.assertEqual(fp.references(self.img, self.sections, "disp", XA), [(0x1500, 7), (0x1600, 7)])

    def test_best_pattern_is_unique_and_resolves_to_the_target(self):
        pat, length, nrefs = fp.best_pattern(self.img, self.sections, "rip", CELL)
        self.assertEqual(nrefs, 2)
        hits = fp.find_all(self.img, self.sections, pat)
        self.assertEqual(len(hits), 1)
        self.assertEqual(fp.resolve(self.img, hits[0], "rip", length), CELL)
        self.assertEqual(pat[3:7], [None] * 4)  # the displacement is a wildcard

    def test_pattern_grows_past_a_shared_head(self):
        # 0x1100's head and tail are shared with 0x1300 (another cell): the head can't decide, so either the
        # shorter unique 0x1200 wins or 0x1100's pattern is longer than the bare instruction.
        pat, length, _ = fp.best_pattern(self.img, self.sections, "rip", CELL)
        self.assertGreater(len(pat), 7)

    def test_store_immediate_form_has_length_11(self):
        pat, length, _ = fp.best_pattern(self.img, self.sections, "rip", OTHER_CELL)
        at = fp.find_all(self.img, self.sections, pat)[0]
        self.assertEqual(fp.resolve(self.img, at, "rip", length), OTHER_CELL)

    def test_call_targets_in_the_context_are_wildcards(self):
        pat, length, _ = fp.best_pattern(self.img, self.sections, "disp", XA)
        self.assertEqual(fp.resolve(self.img, fp.find_all(self.img, self.sections, pat)[0], "disp", length), XA)
        text = fp.format_pattern(pat)
        self.assertIn("E8 ?? ?? ?? ??", text)

    def test_references_outside_code_are_ignored(self):
        img = bytearray(self.img)
        at, data = rip(DATA + 0x100, 0x8B, 0x05, CELL)
        img[at:at + len(data)] = data
        self.assertEqual(len(fp.references(bytes(img), self.sections, "rip", CELL)), 2)

    def test_parse_and_format_round_trip(self):
        self.assertEqual(fp.format_pattern(fp.parse_pattern("48 8b ?? 05")), "48 8B ?? 05")

    def test_not_a_pe_image(self):
        with self.assertRaises(ValueError):
            fp.code_sections(b"\0" * 0x100)

    def test_main_checks_old_pattern_and_reports(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "module.bin")
            with open(path, "wb") as f:
                f.write(self.img)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = fp.main([path, "CELL=rip:0x%x" % CELL, "XA=disp:0x%x" % XA,
                                "--old", "CELL=48 8B 05 ?? ?? ?? ?? 48 8B 40 08"])
            self.assertEqual(code, 0, out.getvalue())
            self.assertIn("old pattern: 1 match(es), resolves to 0x%x" % CELL, out.getvalue())
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(fp.main([path, "CELL=rip:0x%x" % CELL, "--old", "CELL=48 8B 05 ?? ?? ?? ??"]), 1)


if __name__ == "__main__":
    unittest.main()
