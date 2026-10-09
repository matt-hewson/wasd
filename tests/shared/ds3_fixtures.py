"""Synthetic DS3 game files for tests: archives signed with a toy RSA key,
DCX containers and MSB map files, built to the layouts tools/ds3_archive.py
and tools/extract_treasures.py document. Shared by tests/py and tests/pkg.

make_game(folder) writes a whole fake "Game" folder (Data5.bhd/.bdt with
one map) plus a keys file; point the extractor at the keys with the
test-only WASD_RSA_KEYS_FILE environment variable.
"""
import base64
import json
import os
import struct
import zlib

import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "tools"))
import ds3_archive  # noqa: E402

def der_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b


def der_int(v):
    b = v.to_bytes((v.bit_length() + 7) // 8 or 1, "big")
    if b[0] & 0x80:
        b = b"\0" + b
    return b"\x02" + der_len(len(b)) + b


def rsa_public_b64(n, e):
    body = der_int(n) + der_int(e)
    return base64.b64encode(b"\x30" + der_len(len(body)) + body).decode()


# Toy RSA key: two Mersenne primes (2^127-1, 2^89-1); n is 216 bits, so the
# tool reads 27-byte blocks and gives back 26 bytes each.
P, Q, E = 2 ** 127 - 1, 2 ** 89 - 1, 65537
N = P * Q
D = pow(E, -1, (P - 1) * (Q - 1))
KBYTES = (N.bit_length() + 7) // 8


def rsa_sign_blocks(plain):
    """What FromSoftware's tool does to a .bhd: m^d per block (the reader applies c^e)."""
    step = KBYTES - 1
    plain += b"\0" * (-len(plain) % step)
    out = b""
    for i in range(0, len(plain), step):
        m = int.from_bytes(plain[i:i + step], "big")
        out += pow(m, D, N).to_bytes(KBYTES, "big")
    return out


def utf16z(s):
    return s.encode("utf-16-le") + b"\0\0"


def make_dcx(payload, fmt=b"DFLT"):
    comp = zlib.compress(payload)
    return (b"DCX\0" + b"\0" * 20 + b"DCS\0" + struct.pack(">ii", len(payload), len(comp)) +
            b"DCP\0" + fmt + b"\0" * 24 + b"DCA\0" + struct.pack(">i", 8) + comp)


def part(name, ptype, pos, layer=1, coll=None, region=None, entity=0, think=0, npc=0, place=-1):
    """One PARTS entry, offsets relative to itself (as in MSB3)."""
    b = bytearray(0x1C0)
    struct.pack_into("<qI", b, 0, 0x100, ptype)
    struct.pack_into("<3f", b, 0x20, *pos)
    struct.pack_into("<I", b, 0x48, layer)
    struct.pack_into("<qq", b, 0xB0, 0x140, 0x180)
    b[0x100:0x100 + len(utf16z(name))] = utf16z(name)
    struct.pack_into("<i", b, 0x140, entity)
    t = 0x180
    if ptype == 1:
        struct.pack_into("<i", b, t + 0x08, coll)
    elif ptype == 2:
        struct.pack_into("<iii", b, t + 0x08, think, npc, 0)
        struct.pack_into("<i", b, t + 0x1C, coll)
    elif ptype == 5:
        struct.pack_into("<i", b, t + 0x38, region)
        struct.pack_into("<h", b, t + 0x24, place)  # place-name id (negated in the game's files); -1 none
    return bytes(b)


def event(etype, part_idx=0, lot1=0, lot2=0):
    b = bytearray(0x60)
    struct.pack_into("<qiI", b, 0, 0, 1, etype)
    struct.pack_into("<q", b, 0x20, 0x40)
    struct.pack_into("<i", b, 0x40 + 8, part_idx)
    struct.pack_into("<ii", b, 0x40 + 0x10, lot1, lot2)
    return bytes(b)


def make_msb(params):
    """params: [(name, [entry bytes, ...]), ...] -> MSB bytes."""
    out = bytearray(b"MSB " + b"\0" * 12)
    for k, (name, entries) in enumerate(params):
        pos = len(out)
        head = 16 + 8 * len(entries) + 8
        name_at = pos + head
        first = name_at + len(utf16z(name))
        first += -first % 8
        offsets, at = [], first
        for e in entries:
            offsets.append(at)
            at += len(e)
        nxt = at if k + 1 < len(params) else 0
        out += struct.pack("<iiq", 3, len(entries) + 1, name_at)
        out += struct.pack("<%dq" % len(entries), *offsets) + struct.pack("<q", nxt)
        out += utf16z(name)
        out += b"\0" * (first - len(out))
        for e in entries:
            out += e
    return bytes(out)



def make_bnd4(files):
    """A BND4 container in DS3's message-archive layout (format 0x74):
    files = [(id, name, bytes)]."""
    count = len(files)
    names_at = 0x40 + 0x24 * count
    names = b"".join(utf16z(n) for _, n, _ in files)
    data_at = names_at + len(names)
    data_at += -data_at % 16
    head = bytearray(0x40)
    head[:4] = b"BND4"
    head[0x0A] = 1  # as in the game's files
    struct.pack_into("<iq8sqq", head, 0x0C, count, 0x40, b"07D7R6\0\0", 0x24, data_at)
    head[0x30], head[0x31], head[0x32] = 1, 0x74, 4
    headers, blobs, name_off, off = b"", b"", names_at, data_at
    for fid, n, body in files:
        headers += struct.pack("<B3xiqqIiI", 0x40, -1, len(body), len(body), off, fid, name_off)
        name_off += len(utf16z(n))
        pad = body + b"\0" * (-len(body) % 16)
        blobs += pad
        off += len(pad)
    out = bytes(head) + headers + names
    return out + b"\0" * (data_at - len(out)) + blobs


def make_fmg(texts, no_text=()):
    """A version-2 FMG: texts = {id: text}; ids in no_text get a 0 offset
    (no text). Consecutive ids share a group, as in the game's files."""
    ids = sorted(set(texts) | set(no_text))
    groups, k = [], 0
    while k < len(ids):
        start = k
        while k + 1 < len(ids) and ids[k + 1] == ids[k] + 1:
            k += 1
        groups.append((start, ids[start], ids[k]))
        k += 1
    offsets_at = 0x28 + 16 * len(groups)
    strings_at = offsets_at + 8 * len(ids)
    offsets, blob = [], b""
    for i in ids:
        if i in no_text:
            offsets.append(0)
        else:
            offsets.append(strings_at + len(blob))
            blob += utf16z(texts[i])
    body = b"".join(struct.pack("<iiii", s, a, b, 0) for s, a, b in groups)
    body += struct.pack("<%dq" % len(offsets), *offsets) + blob
    size = 0x28 + len(body)
    head = struct.pack("<BBBBiBBBBiiiqq", 0, 0, 2, 0, size, 1, 0, 0, 0, len(groups), len(ids), 0xFF, offsets_at, 0)
    return head + body


def make_emevd(instructions, version=0xCD):
    """A DS3 event script (EMEVD, 64-bit little-endian) holding one event
    with these instructions: [(bank, id, args bytes)]."""
    header_size, event_size, inst_size = 0x90, 0x30, 0x20
    events_off = header_size
    inst_off = events_off + event_size
    args_off = inst_off + inst_size * len(instructions)
    args, inst = b"", b""
    for bank, iid, a in instructions:
        inst += struct.pack("<iiqqq", bank, iid, len(a), len(args), -1)
        args += a + b"\0" * (-len(a) % 4)
    event = struct.pack("<qqqqqI4x", 0, len(instructions), 0, 0, -1, 0)
    head = bytearray(header_size)
    head[:8] = b"EVD\0\0\xff\x01\xff"
    struct.pack_into("<ii", head, 8, version, header_size + len(event) + len(inst) + len(args))
    struct.pack_into("<qqqq", head, 0x10, 1, events_off, len(instructions), inst_off)
    struct.pack_into("<qq", head, 0x70, len(args), args_off)
    return bytes(head) + event + inst + args


def health_bar(entity, name_id, slot=0, state=1):
    """The arguments of instruction 2003:11 (show a boss health bar)."""
    return struct.pack("<B3xihxxi", state, entity, slot, name_id)


def make_param(param_type, rows, row_size):
    """A .param table in the game's layout: rows = [(id, bytes)], each padded
    to row_size; empty row names, the type name after the rows."""
    count = len(rows)
    data_start = 0x40 + 24 * count
    type_off = data_start + row_size * count
    names_off = type_off + len(param_type) + 1
    head = bytearray(0x40)
    struct.pack_into("<I", head, 0x00, type_off)
    struct.pack_into("<HH", head, 0x08, 2, count)
    struct.pack_into("<q", head, 0x10, type_off)
    head[0x2D], head[0x2E] = 0x85, 0x07
    struct.pack_into("<q", head, 0x30, data_start)
    entries = b"".join(struct.pack("<iiqq", rid, 0, data_start + row_size * i, names_off)
                       for i, (rid, _) in enumerate(rows))
    body = b"".join(b[:row_size] + b"\0" * (row_size - len(b)) for _, b in rows)
    return bytes(head) + entries + body + param_type.encode("ascii") + b"\0\0\0"


def aes_cbc_encrypt(key, iv, data):
    """AES-CBC encryption through Windows CNG (tests only; Windows only)."""
    import ctypes
    bc = ctypes.WinDLL("bcrypt")
    alg = ctypes.c_void_p()
    assert bc.BCryptOpenAlgorithmProvider(ctypes.byref(alg), "AES", None, 0) == 0
    mode = ctypes.create_unicode_buffer("ChainingModeCBC")
    assert bc.BCryptSetProperty(alg, "ChainingMode", mode, ctypes.sizeof(mode), 0) == 0
    h = ctypes.c_void_p()
    kb = ctypes.create_string_buffer(key, len(key))
    assert bc.BCryptGenerateSymmetricKey(alg, ctypes.byref(h), None, 0, kb, len(key), 0) == 0
    ivb = ctypes.create_string_buffer(iv, 16)
    inb = ctypes.create_string_buffer(data, len(data))
    outb = ctypes.create_string_buffer(len(data))
    got = ctypes.c_ulong()
    assert bc.BCryptEncrypt(h, inb, len(data), None, ivb, 16, outb, len(data), ctypes.byref(got), 0) == 0
    bc.BCryptDestroyKey(h)
    bc.BCryptCloseAlgorithmProvider(alg, 0)
    return outb.raw[:got.value]


def write_regulation(folder, files, iv=bytes(range(16))):
    """Data0.bdt as the game ships it: BND4 of files [(id, name, bytes)],
    DCX-compressed, AES-256-CBC encrypted with the regulation key, the IV
    first. Windows only (AES)."""
    plain = make_dcx(make_bnd4(files))
    plain += b"\0" * (-len(plain) % 16)
    with open(os.path.join(folder, ds3_archive.REGULATION_FILE), "wb") as f:
        f.write(iv + aes_cbc_encrypt(ds3_archive.REGULATION_KEY, iv, plain))


def write_archive(folder, name, files):
    """Writes <name>.bhd (BHD5 header, signed with the toy key) and <name>.bdt
    holding files {"/path": bytes}."""
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
    with open(os.path.join(folder, name + ".bhd"), "wb") as f:
        f.write(rsa_sign_blocks(bytes(hdr)))
    with open(os.path.join(folder, name + ".bdt"), "wb") as f:
        f.write(bdt)


# What make_game's one map holds, and what the extractor must write for it.
GAME_MAP = "m30_00_00_00"
EXPECTED_TREASURES = [("m30_00_00_00", 4000050, 0, 10.5, -2.0, 33.25, 300001, "o0001")]
EXPECTED_ENEMIES = [("m30_00_00_00", 3000800, 100100, 100000, 4.0, 5.0, 6.0, "c1000_0000")]


def game_msb():
    parts = [part("h0000", 5, (0, 0, 0), region=300001),
             part("o0001", 1, (10.5, -2.0, 33.25), coll=0),
             part("c1000_0000", 2, (4, 5, 6), coll=0, entity=3000800, think=100000, npc=100100)]
    events = [event(4, part_idx=1, lot1=4000050, lot2=0)]
    return make_msb([("MODEL_PARAM_ST", []), ("EVENT_PARAM_ST", events), ("PARTS_PARAM_ST", parts)])


def make_game(folder):
    """A fake DS3 'Game' folder: Data5 with one map. Returns the keys file to
    put in WASD_RSA_KEYS_FILE."""
    os.makedirs(folder, exist_ok=True)
    write_archive(folder, "Data5", {"/map/mapstudio/%s.msb.dcx" % GAME_MAP: make_dcx(game_msb())})
    keys = os.path.join(folder, "test_keys.json")
    with open(keys, "w", encoding="utf-8") as f:
        json.dump({"Data5": rsa_public_b64(N, E)}, f)
    return keys


def read_tsv(path):
    """Rows of a generated tsv, numbers parsed, comments skipped."""
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            cells = line.rstrip("\n").split("\t")
            rows.append(tuple(float(c) if "." in c else int(c) if c.lstrip("-").isdigit() else c for c in cells))
    return rows
