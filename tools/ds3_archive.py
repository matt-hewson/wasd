"""Read files straight out of DS3's packed archives (DataN.bhd / DataN.bdt).

Offline, read-only: opens the game's archive files for reading and never
writes to the install. Nothing is unpacked to disk except what the caller
saves. Standard library only; AES comes from Windows (bcrypt.dll).

Formats as documented by BinderTool (Atvaark, MIT licence):
  - DataN.bhd is RSA-"encrypted" with a public key (each 256-byte block is
    c^e mod n, giving 255 bytes); the plain text is a BHD5 header.
  - BHD5 (DS3): "BHD5", version, unk, size, bucketCount, bucketOffset,
    saltLength, salt. Each bucket: entryCount, entryOffset. Each entry
    (40 bytes): nameHash u32, paddedSize u32, offset i64, shaOffset i64,
    aesOffset i64, size i64.
  - AES key block: key[16], rangeCount i32, ranges (start i64, end i64);
    the listed ranges of the file are AES-128-ECB encrypted.
  - Name hash: lower-case path with '/' separators, h = h*37 + c (u32).
  - DCX: big-endian container; "DFLT" = zlib.

The message (text) files, as documented by SoulsFormats (soulsmods/
SoulsFormatsNEXT, GPL-3.0; layout facts only, reimplemented here), checked
against DS3's own msg/engus/*.msgbnd.dcx (in Data1):
  - BND4 container, little-endian: "BND4", +0x0C fileCount i32, +0x10
    headerSize i64 (0x40), +0x18 version char[8], +0x20 fileHeaderSize
    i64, +0x28 dataStart i64, +0x30 unicode u8, format u8. DS3's message
    archives all use format 0x74 (ids, names, sizes, 32-bit offsets): file
    headers of 0x24 bytes at +0x40, each flags u8, 3 pad, -1 i32,
    compressedSize i64, uncompressedSize i64, dataOffset u32, id i32,
    nameOffset u32 (UTF-16 name). Other layouts are refused.
  - FMG version 2, little-endian: +0x02 version u8 (2), +0x04 fileSize i32,
    +0x0C groupCount i32, +0x10 stringCount i32, +0x18 stringOffsetsOffset
    i64; groups (16 bytes each) from +0x28: offsetIndex i32, firstId i32,
    lastId i32, pad; string offsets i64 (0 = no text); UTF-16 strings.
    Id firstId + k uses string offset offsetIndex + k.
"""
import base64
import ctypes
import os
import struct
import zlib

# Public RSA keys from BinderTool's DecryptionKeys.cs (the game ships the
# matching data; these only let us read the header).
RSA_KEYS = {
    "Data1": """MIIBCwKCAQEA05hqyboW/qZaJ3GBIABFVt1X1aa0/sKINklvpkTRC+5Ytbxvp18L
M1gN6gjTgSJiPUgdlaMbptVa66MzvilEk60aHyVVEhtFWy+HzUZ3xRQm6r/2qsK3
8wXndgEU5JIT2jrBXZcZfYDCkUkjsGVkYqjBNKfp+c5jlnNwbieUihWTSEO+DA8n
aaCCzZD3e7rKhDQyLCkpdsGmuqBvl02Ou7QeehbPPno78mOYs2XkP6NGqbFFGQwa
swyyyXlQ23N15ZaFGRRR0xYjrX4LSe6OJ8Mx/Zkec0o7L28CgwCTmcD2wO8TEATE
AUbbV+1Su9uq2+wQxgnsAp+xzhn9og9hmwIEC35bSQ==""",
    "Data5": """MIIBCwKCAQEAvKTlU3nka4nQesRnYg1NWovCCTLhEBAnjmXwI69lFYfc4lvZsTrQ
E0Y25PtoP0ZddA3nzflJNz1rBwAkqfBRGTeeTCAyoNp/iel3EAkid/pKOt3JEkHx
rojRuWYSQ0EQawcBbzCfdLEjizmREepRKHIUSDWgu0HTmwSFHHeCFbpBA1h99L2X
izH5XFTOu0UIcUmBLsK6DYsIj5QGrWaxwwXcTJN/X+/syJ/TbQK9W/TCGaGiirGM
1u2wvZXSZ7uVM3CHwgNhAMiqLvqORygcDeNqxgq+dXDTxka43j7iPJWdHs8b25fy
aH3kbUxKlDGaEENNNyZQcQrgz8Q76jIE0QIEFUsz9w==""",
    "DLC1": """MIIBCwKCAQEAsCGM9dFwzaIOUIin3DXy7xrmI2otKGLZJQyKi5X3znKhSTywpcFc
KoW6hgjeh4fJW24jhzwBosG6eAzDINm+K02pHCG8qZ/D/hIbu+ui0ENDKqrVyFhn
QtX5/QJkVQtj8M4a0FIfdtE3wkxaKtP6IXWIy4DesSdGWONVWLfi2eq62A5ts5MF
qMoSV3XjTYuCgXqZQ6eOE+NIBQRqpZxLNFSzbJwWXpAg2kBMkpy5+ywOByjmWzUw
jnIFl1T17R8DpTU/93ojx+/q1p+b1o5is5KcoP7QwjOqzjHJH8bTytzRbgmRcDMW
3ahxgI070d45TMXK2YwRzI6/JbM1P29anQIEFezyYw==""",
    "DLC2": """MIIBCwKCAQEAtCXU9a/GBMVoqtpQox9p0/5sWPaIvDp8avLFnIBhN7vkgTwulZHi
u64vZAiUAdVeFX4F+Qtk+5ivK488Mu2CzAMJcz5RvyMQJtOQXuDDqzIv21Tr5zuu
sswoErHxxP8TZNxkHm7Ram7Oqtn7LQnMTYxsBgZZ34yJkRtAmZnGoCu5YaUR5euk
8lF75idi97ssczUNV212tLzIMa1YOV7sxOb7+gc0VTIqs3pa+OXLPI/bMfwUc/KN
jur5aLDDntQHGx5zuNtc78gMGwlmPqDhgTusKPO4VyKvoL0kITYvukoXJATaa1HI
WVUjhLm+/uj8r8PNgolerDeS+8FM5Bpe9QIEHwCZLw==""",
}


def _der_len(b, i):
    n = b[i]
    i += 1
    if n & 0x80:
        k = n & 0x7F
        n = int.from_bytes(b[i:i + k], "big")
        i += k
    return n, i


def _parse_rsa_public(b64):
    """PKCS#1 RSAPublicKey DER -> (n, e)."""
    der = base64.b64decode("".join(b64.split()))
    assert der[0] == 0x30
    _, i = _der_len(der, 1)
    ints = []
    for _ in range(2):
        assert der[i] == 0x02
        n, i = _der_len(der, i + 1)
        ints.append(int.from_bytes(der[i:i + n], "big"))
        i += n
    return ints[0], ints[1]


def name_hash(path):
    h = 0
    for c in path.replace("\\", "/").lower():
        h = (h * 37 + ord(c)) & 0xFFFFFFFF
    return h


def _rsa_decrypt_file(path, key_b64):
    n, e = _parse_rsa_public(key_b64)
    kbytes = (n.bit_length() + 7) // 8
    out = bytearray()
    with open(path, "rb") as f:
        data = f.read()
    for i in range(0, len(data), kbytes):
        c = int.from_bytes(data[i:i + kbytes], "big")
        out += pow(c, e, n).to_bytes(kbytes - 1, "big")
    return bytes(out)


# ---- AES via Windows CNG ---------------------------------------------------
# ECB (archive files, 128-bit keys) and CBC (the regulation file, 256-bit).
# Loaded only on Windows, so the rest of this module (and its tests) also
# imports elsewhere, e.g. in a cloud container.
_bcrypt = ctypes.WinDLL("bcrypt") if os.name == "nt" else None


def _aes_ecb_decrypt(key, data):
    return _aes_decrypt(key, data)


def _aes_cbc_decrypt(key, iv, data):
    return _aes_decrypt(key, data, iv)


def _aes_decrypt(key, data, iv=None):
    """AES-ECB, or AES-CBC with this 16-byte IV; no padding removed."""
    assert len(data) % 16 == 0
    assert iv is None or len(iv) == 16
    if _bcrypt is None:
        raise OSError("AES needs Windows (bcrypt.dll)")
    alg = ctypes.c_void_p()
    if _bcrypt.BCryptOpenAlgorithmProvider(ctypes.byref(alg), "AES", None, 0):
        raise OSError("BCryptOpenAlgorithmProvider failed")
    try:
        mode = "ChainingModeECB" if iv is None else "ChainingModeCBC"
        buf = ctypes.create_unicode_buffer(mode)
        if _bcrypt.BCryptSetProperty(alg, "ChainingMode", buf, ctypes.sizeof(buf), 0):
            raise OSError("BCryptSetProperty failed")
        hkey = ctypes.c_void_p()
        kb = ctypes.create_string_buffer(key, len(key))
        if _bcrypt.BCryptGenerateSymmetricKey(alg, ctypes.byref(hkey), None, 0, kb, len(key), 0):
            raise OSError("BCryptGenerateSymmetricKey failed")
        try:
            inb = ctypes.create_string_buffer(bytes(data), len(data))
            outb = ctypes.create_string_buffer(len(data))
            got = ctypes.c_ulong()
            ivb = None if iv is None else ctypes.create_string_buffer(bytes(iv), 16)
            if _bcrypt.BCryptDecrypt(hkey, inb, len(data), None, ivb, 0 if iv is None else 16, outb, len(data),
                                     ctypes.byref(got), 0):
                raise OSError("BCryptDecrypt failed")
            return outb.raw[:got.value]
        finally:
            _bcrypt.BCryptDestroyKey(hkey)
    finally:
        _bcrypt.BCryptCloseAlgorithmProvider(alg, 0)


class Archive:
    def __init__(self, game_dir, name="Data5"):
        self.bdt = os.path.join(game_dir, name + ".bdt")
        hdr = _rsa_decrypt_file(os.path.join(game_dir, name + ".bhd"), RSA_KEYS[name])
        if hdr[:4] != b"BHD5":
            raise ValueError("%s.bhd did not decrypt to BHD5" % name)
        bucket_count, bucket_off = struct.unpack_from("<ii", hdr, 16)
        self.entries = {}
        for b in range(bucket_count):
            cnt, off = struct.unpack_from("<ii", hdr, bucket_off + 8 * b)
            for k in range(cnt):
                h, padded, foff, sha_off, aes_off, size = struct.unpack_from("<IIqqqq", hdr, off + 40 * k)
                aes = None
                if aes_off:
                    key = hdr[aes_off:aes_off + 16]
                    (rc,) = struct.unpack_from("<i", hdr, aes_off + 16)
                    ranges = [struct.unpack_from("<qq", hdr, aes_off + 20 + 16 * r) for r in range(rc)]
                    aes = (key, ranges)
                self.entries[h] = (foff, padded, size, aes)

    def has(self, path):
        return name_hash(path) in self.entries

    def read(self, path):
        foff, padded, size, aes = self.entries[name_hash(path)]
        with open(self.bdt, "rb") as f:
            f.seek(foff)
            data = bytearray(f.read(padded))
        if aes:
            key, ranges = aes
            for start, end in ranges:
                if start == -1 or end == -1 or end <= start:
                    continue
                data[start:end] = _aes_ecb_decrypt(key, data[start:end])
        return bytes(data[:size or padded])


def dcx_decompress(data):
    if data[:4] != b"DCX\0":
        return data
    # DCX header (big-endian): ... "DCS\0" uncompressed, compressed;
    # "DCP\0" "DFLT" ...; "DCA\0" headerSize; then the zlib stream.
    i = data.index(b"DCP\0")
    fmt = data[i + 4:i + 8]
    if fmt != b"DFLT":
        raise NotImplementedError("DCX compression %r" % fmt)
    j = data.index(b"DCA\0", i)
    (dca_size,) = struct.unpack_from(">i", data, j + 4)
    (csize,) = struct.unpack_from(">i", data, data.index(b"DCS\0") + 8)
    return zlib.decompress(data[j + dca_size:j + dca_size + csize])


def _utf16z(b, off):
    end = off
    while True:
        if end + 2 > len(b):
            raise ValueError("unterminated UTF-16 string at 0x%x" % off)
        if b[end:end + 2] == b"\0\0":
            return b[off:end].decode("utf-16-le")
        end += 2


def read_bnd4(data):
    """The files in a BND4 container: [(id, name, bytes)], in file order.
    Only the layout DS3's message archives use is accepted (see the module
    docstring); anything else raises NotImplementedError."""
    if data[:4] != b"BND4":
        raise ValueError("not a BND4 container")
    (count,) = struct.unpack_from("<i", data, 0x0C)
    (header_size,) = struct.unpack_from("<q", data, 0x10)
    (file_header_size,) = struct.unpack_from("<q", data, 0x20)
    big_endian, unicode, fmt = data[0x09], data[0x30], data[0x31]
    if big_endian or header_size != 0x40 or file_header_size != 0x24 or fmt != 0x74 or unicode != 1:
        raise NotImplementedError("BND4 layout not supported (big-endian %d, header 0x%x, file header 0x%x, format 0x%x, unicode %d)"
                                  % (big_endian, header_size, file_header_size, fmt, unicode))
    if count < 0 or 0x40 + count * 0x24 > len(data):
        raise ValueError("BND4 file count %d doesn't fit the data" % count)
    files = []
    for i in range(count):
        _flags, minus1, csize, usize, off, fid, name_off = struct.unpack_from("<B3xiqqIiI", data, 0x40 + i * 0x24)
        if minus1 != -1:
            raise ValueError("BND4 file header %d: expected -1, got %d" % (i, minus1))
        if csize != usize:
            raise NotImplementedError("BND4 file %d is compressed inside the container" % i)
        if csize < 0 or off + csize > len(data):
            raise ValueError("BND4 file %d runs past the end of the data" % i)
        files.append((fid, _utf16z(data, name_off), data[off:off + csize]))
    return files


def read_fmg(data):
    """A version-2 FMG text file as {id: text}. Ids with no text (offset 0)
    are left out; empty strings are kept as ''."""
    if len(data) < 0x28 or data[0] != 0 or data[1] != 0 or data[2] != 2:
        raise NotImplementedError("only little-endian version-2 FMG files are supported")
    (size,) = struct.unpack_from("<i", data, 0x04)
    group_count, string_count = struct.unpack_from("<ii", data, 0x0C)
    (offsets_off,) = struct.unpack_from("<q", data, 0x18)
    if size != len(data):
        raise ValueError("FMG says %d bytes, has %d" % (size, len(data)))
    if offsets_off != 0x28 + 16 * group_count or offsets_off + 8 * string_count > len(data):
        raise ValueError("FMG string offsets at 0x%x don't follow %d groups" % (offsets_off, group_count))
    texts = {}
    for g in range(group_count):
        index, first, last = struct.unpack_from("<iii", data, 0x28 + 16 * g)
        if last < first or index < 0 or index + (last - first) >= string_count:
            raise ValueError("FMG group %d (ids %d..%d, index %d) is out of range" % (g, first, last, index))
        for k in range(last - first + 1):
            (off,) = struct.unpack_from("<q", data, offsets_off + 8 * (index + k))
            if off == 0:
                continue
            if first + k in texts:
                raise ValueError("FMG id %d appears twice" % (first + k))
            if off < 0 or off >= len(data):
                raise ValueError("FMG id %d: string offset 0x%x outside the file" % (first + k, off))
            texts[first + k] = _utf16z(data, off)
    return texts


# ---- the regulation file (game parameters) -----------------------------------
#
# Data0.bdt is not an archive like the others: it is the regulation file, a
# DCX-compressed BND4 of .param tables, AES-256-CBC encrypted. The first 16
# bytes are the IV. Key from BinderTool's DecryptionKeys.cs
# (RegulationFileKeyDs3, MIT licence), checked to decrypt the game's own
# Data0.bdt to a DCX container.
REGULATION_FILE = "Data0.bdt"
REGULATION_KEY = b"ds3#jn/8_7(rsY9pg55GFN7VFL#+3n/)"


def read_regulation(game_dir):
    """The regulation's files: [(id, name, bytes)], as read_bnd4."""
    with open(os.path.join(game_dir, REGULATION_FILE), "rb") as f:
        raw = f.read()
    if len(raw) < 32 or (len(raw) - 16) % 16:
        raise ValueError("%s is not an encrypted regulation file (%d bytes)" % (REGULATION_FILE, len(raw)))
    plain = _aes_cbc_decrypt(REGULATION_KEY, raw[:16], raw[16:])
    if plain[:4] != b"DCX\0":
        raise ValueError("%s did not decrypt to a DCX container" % REGULATION_FILE)
    return read_bnd4(dcx_decompress(plain))


def regulation_file(files, name):
    """The one file in the regulation whose name ends in \\<name>."""
    hits = [body for _, n, body in files if n.replace("/", "\\").split("\\")[-1] == name]
    if len(hits) != 1:
        raise ValueError("%d files named %s in the regulation" % (len(hits), name))
    return hits[0]


def read_param(data, param_type):
    """A .param table as (row size, [(row id, row bytes)]) in file order.

    Layout, from the game's own files: a 0x40-byte header (+0x00 offset of
    the param type name u32, +0x0A row count u16, +0x30 data start i64),
    then one 24-byte entry per row (id i32, pad, data offset i64, name
    offset i64), then the rows, all the same size, ending where the type
    name starts. Checked strictly; param_type (e.g. "BONFIRE_WARP_PARAM_ST")
    must match the name in the file."""
    if len(data) < 0x40 or data[0x2C] != 0:
        raise NotImplementedError("only little-endian param files are supported")
    (type_off,) = struct.unpack_from("<I", data, 0x00)
    (count,) = struct.unpack_from("<H", data, 0x0A)
    (data_start,) = struct.unpack_from("<q", data, 0x30)
    if data_start != 0x40 + 24 * count or not data_start <= type_off < len(data):
        raise ValueError("param header doesn't add up (%d rows, data at 0x%x, type name at 0x%x)" % (count, data_start, type_off))
    name = data[type_off:].split(b"\0", 1)[0].decode("ascii", "replace")
    if name != param_type:
        raise ValueError("param type is %r, expected %r" % (name, param_type))
    entries = [struct.unpack_from("<iiqq", data, 0x40 + 24 * i) for i in range(count)]
    size = (type_off - data_start) // count if count else 0
    if count and data_start + size * count != type_off:
        raise ValueError("param rows don't fill the data area evenly")
    rows = []
    for i, (row_id, _pad, off, _name) in enumerate(entries):
        if off != data_start + size * i:
            raise ValueError("param row %d (id %d) is not where expected" % (i, row_id))
        rows.append((row_id, data[off:off + size]))
    return size, rows
