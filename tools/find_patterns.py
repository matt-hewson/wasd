"""Find our own byte patterns for the memory roots WASD reads.

Input: a copy of the game's loaded module (`wasd-cli.exe dumpmodule <file>`, read-only; the game's own code, kept
private). Image layout: file offset = RVA. For each target this lists every instruction in the executable sections
that refers to it, then grows a pattern from the start of each such instruction until it matches exactly once in
the code, and keeps the shortest.

Two kinds of target:
  rip:<rva>    a pointer cell the code loads RIP-relatively ([rip+disp32]). Only REX-prefixed one-byte-opcode forms
               are used (REX, opcode, ModRM 00-xxx-101, disp32 at +3, then any immediate), so a pattern resolves as
               cell = instruction + length + disp32, the way src/main.cpp's resolver reads them.
  disp:<value> a struct offset used as [reg+disp32] (REX, opcode, ModRM 10-xxx-rrr without SIB, disp32 at +3);
               the pattern yields the value itself (e.g. the XA offset).
In the pattern, the instruction's own displacement is a wildcard, and so are the rel32 of calls / jumps and the
displacements of other RIP-relative instructions in the context, which change whenever the code moves.

x86-64 encoding facts (REX prefixes, ModRM, RIP-relative addressing, immediate sizes per opcode) are from the Intel
and AMD manuals. Usage:
  find_patterns.py <module.bin> NAME=rip:0x4752f68 NAME2=disp:0x1f90 ... [--old NAME=PATTERN ...] [--max 48]
--old resolves an existing pattern in the same image and checks the new one lands on the same target.
"""
import argparse
import re
import struct
import sys

# One-byte opcodes that take a ModRM operand, with the size of the immediate that follows the displacement.
IMM_SIZE = {0x01: 0, 0x03: 0, 0x09: 0, 0x0B: 0, 0x21: 0, 0x23: 0, 0x29: 0, 0x2B: 0, 0x31: 0, 0x33: 0, 0x39: 0,
            0x3B: 0, 0x63: 0, 0x85: 0, 0x87: 0, 0x89: 0, 0x8B: 0, 0x8D: 0, 0xFF: 0,
            0x80: 1, 0x83: 1, 0xC6: 1, 0x81: 4, 0xC7: 4}
_REX = b"[\x40-\x4f]"
_OPS = b"[" + b"".join(re.escape(bytes([o])) for o in sorted(IMM_SIZE)) + b"]"
RIP_FORM = re.compile(_REX + _OPS + b"[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", re.S)  # ModRM mod=00 rm=101
DISP_FORM = re.compile(_REX + _OPS + b"[\x80-\xbf]", re.S)  # ModRM mod=10 (rm checked: no SIB)
CONTEXT_RIP = RIP_FORM  # other RIP-relative instructions in a pattern's context: wildcard their disp32


def parse_pattern(text):
    """'48 8B ?? 05' -> [0x48, 0x8B, None, 0x05]."""
    return [None if t in ("?", "??") else int(t, 16) for t in text.split()]


def format_pattern(pat):
    return " ".join("??" if b is None else "%02X" % b for b in pat)


def pattern_regex(pat):
    return re.compile(b"".join(b"." if b is None else re.escape(bytes([b])) for b in pat), re.S)


def code_sections(image):
    """[(start, end)] of the executable sections, from the PE headers at the start of the image."""
    if image[:2] != b"MZ":
        raise ValueError("not a PE image (no MZ header)")
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    if image[pe:pe + 4] != b"PE\0\0":
        raise ValueError("no PE signature")
    count, opt_size = struct.unpack_from("<H", image, pe + 6)[0], struct.unpack_from("<H", image, pe + 20)[0]
    table = pe + 24 + opt_size
    out = []
    for k in range(count):
        s = table + 40 * k
        vsize, rva = struct.unpack_from("<II", image, s + 8)
        flags = struct.unpack_from("<I", image, s + 36)[0]
        if flags & 0x20000000:  # IMAGE_SCN_MEM_EXECUTE
            out.append((rva, min(rva + vsize, len(image))))
    if not out:
        raise ValueError("no executable section")
    return out


def find_all(image, sections, pat):
    rx = pattern_regex(pat)
    hits = []
    for start, end in sections:
        hits += [m.start() for m in rx.finditer(image, start, end)]
    return hits


def references(image, sections, kind, target):
    """Instructions referring to the target: [(start, length)], displacement always at start + 3."""
    refs = []
    form = RIP_FORM if kind == "rip" else DISP_FORM
    for start, end in sections:
        for m in form.finditer(image, start, end - 7):
            i = m.start()
            op, modrm = image[i + 1], image[i + 2]
            if kind == "disp" and (modrm & 7) == 4:
                continue  # a SIB byte follows: displacement not at +3
            imm = IMM_SIZE[op]
            length = 7 + imm
            disp = struct.unpack_from("<i", image, i + 3)[0]
            if (kind == "rip" and i + length + disp == target) or (kind == "disp" and disp == target):
                refs.append((i, length))
    return refs


def resolve(image, at, kind, length):
    disp = struct.unpack_from("<i", image, at + 3)[0]
    return at + length + disp if kind == "rip" else disp


def _context_mask(image, at, n, length):
    """Byte pattern of image[at:at+n] with the instruction's disp32 and the context's moving parts wildcarded."""
    pat = list(image[at:at + n])
    for k in range(3, 7):
        pat[k] = None
    p = length
    while p < n:
        b = image[at + p]
        if b in (0xE8, 0xE9) and p + 5 <= n:  # call / jmp rel32
            for k in range(p + 1, p + 5):
                pat[k] = None
            p += 5
            continue
        if b == 0x0F and p + 6 <= n and 0x80 <= image[at + p + 1] <= 0x8F:  # jcc rel32
            for k in range(p + 2, p + 6):
                pat[k] = None
            p += 6
            continue
        if p + 7 <= n and CONTEXT_RIP.match(image, at + p):
            for k in range(p + 3, p + 7):
                pat[k] = None
            p += 7
            continue
        p += 1
    return pat


def unique_pattern(image, sections, at, length, max_len=48, min_len=16):
    """The shortest pattern starting at the instruction that matches exactly once, or None. The whole window is
    masked first, so a pattern cut short inside a call still has that call's target as wildcards."""
    window = _context_mask(image, at, min(max_len, len(image) - at), length)
    # One scan for the instruction itself, then narrow the hits byte by byte (no rescans of the code).
    hits = [h for h in find_all(image, sections, window[:length]) if h + len(window) <= len(image)]
    for n in range(length, len(window) + 1):
        b = window[n - 1]
        if n > length and b is not None:
            hits = [h for h in hits if image[h + n - 1] == b]
        if b is not None and n >= min_len and hits == [at]:
            return window[:n]  # never ends on a wildcard
    return None


def best_pattern(image, sections, kind, target, max_len=48, max_refs=400, min_len=16):
    """(pattern, instruction length, reference count) for the shortest unique pattern over the references (the
    first max_refs of them: a much-used root has thousands, and a short unique one turns up early)."""
    refs = references(image, sections, kind, target)
    best, best_key = None, None
    for at, length in refs[:max_refs]:
        pat = unique_pattern(image, sections, at, length, max_len, min_len)
        if not pat:
            continue
        # Prefer straight-line context: every wildcard after the instruction is a jump or another reference, and
        # code past a jump belongs elsewhere. Then the shortest.
        key = (sum(1 for b in pat[length:] if b is None), len(pat))
        if best is None or key < best_key:
            best, best_key = (pat, length), key
    return (best[0], best[1], len(refs)) if best else (None, None, len(refs))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("module")
    ap.add_argument("targets", nargs="+", help="NAME=rip:0xRVA or NAME=disp:0xVALUE")
    ap.add_argument("--old", action="append", default=[], help="NAME=PATTERN to compare against (resolved here)")
    ap.add_argument("--max", type=int, default=48)
    ap.add_argument("--min", type=int, default=16,
                    help="minimum pattern length: a short pattern can be unique by chance and break when code changes")
    a = ap.parse_args(argv)
    with open(a.module, "rb") as f:
        image = f.read()
    sections = code_sections(image)
    old = dict(o.split("=", 1) for o in a.old)
    failed = False
    for t in a.targets:
        name, spec = t.split("=", 1)
        kind, value = spec.split(":", 1)
        target = int(value, 16)
        pat, length, nrefs = best_pattern(image, sections, kind, target, a.max, min_len=a.min)
        if not pat:
            print("%-14s %s:0x%x  %d reference(s), none unique within %d bytes" % (name, kind, target, nrefs, a.max))
            failed = True
            continue
        at = find_all(image, sections, pat)[0]
        got = resolve(image, at, kind, length)
        print("%-14s %s:0x%x  %d reference(s); pattern at +0x%x, length %d, resolves to 0x%x%s\n    %s" % (
            name, kind, target, nrefs, at, length, got, "" if got == target else "  ** MISMATCH **",
            format_pattern(pat)))
        failed |= got != target
        if name in old:
            hits = find_all(image, sections, parse_pattern(old[name]))
            olds = sorted({resolve(image, h, kind, length if kind == "disp" else
                                   (11 if image[h + 1] == 0xC7 else 7)) for h in hits})
            print("    old pattern: %d match(es), resolves to %s" % (len(hits), ", ".join("0x%x" % o for o in olds)))
            failed |= olds != [target]
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
