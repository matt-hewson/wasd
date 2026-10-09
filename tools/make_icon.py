"""Draw the placeholder app icon: a bonfire, a coiled
sword standing in a flame on an ash mound, on a dark rounded tile.

Standard library only: each size is drawn from simple shapes with 4x4
supersampling, saved as PNG and packed into assets/app.ico (PNG entries,
which Windows Vista and later read at every size). Rerun to regenerate:

    python -I tools/make_icon.py [--preview out.png]
"""
import math
import os
import struct
import sys
import zlib

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
SS = 4  # supersamples per axis

BG = (27, 23, 18)          # charcoal
BG_GLOW = (74, 40, 18)     # warm glow behind the fire
ASH = (60, 52, 44)
FLAME_OUT = (232, 89, 12)  # deep orange
FLAME_IN = (255, 212, 59)  # yellow core
BLADE = (206, 212, 218)
HILT = (120, 104, 86)


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def in_rounded_square(x, y, half=0.47, r=0.17):
    dx, dy = max(abs(x - 0.5) - (half - r), 0), max(abs(y - 0.5) - (half - r), 0)
    return dx * dx + dy * dy <= r * r


def flame_width(y, tip, cy, r):
    """Half-width of a teardrop: a circle of radius r at cy, tapering to a tip."""
    if y < tip:
        return -1
    if y <= cy:
        return r * ((y - tip) / (cy - tip)) ** 0.85
    d = y - cy
    return math.sqrt(r * r - d * d) if d <= r else -1


def in_flame(x, y, scale=1.0):
    tip, cy, r = 0.20 + (1 - scale) * 0.30, 0.64, 0.21 * scale
    w = flame_width(y, tip, cy, r)
    # A slight flicker: the upper half leans with height.
    lean = 0.025 * math.sin((y - tip) * 9) * (1 - min(1, (y - tip) / (cy - tip)))
    return w >= 0 and abs(x - 0.5 - lean) <= w


def colour_at(x, y):
    """RGBA of the icon at (x, y) in 0..1."""
    if not in_rounded_square(x, y):
        return None
    d = math.hypot(x - 0.5, y - 0.62)
    c = mix(BG_GLOW, BG, min(1.0, d / 0.45))
    if ((x - 0.5) / 0.30) ** 2 + ((y - 0.83) / 0.075) ** 2 <= 1:
        c = ASH
    if in_flame(x, y):
        c = FLAME_OUT
        if in_flame(x, y, 0.62):
            c = mix(FLAME_IN, FLAME_OUT, min(1.0, max(0.0, (0.60 - y) / 0.35)))
    # The coiled sword, point down into the fire: grip, guard, blade.
    if 0.484 <= x <= 0.516 and 0.13 <= y <= 0.29:
        c = HILT
    if 0.40 <= x <= 0.60 and 0.29 <= y <= 0.325:
        c = HILT
    if 0.472 <= x <= 0.528 and 0.325 <= y <= 0.79:
        c = BLADE if not in_flame(x, y, 0.62) else mix(BLADE, FLAME_IN, 0.45)
    return c


def render(size):
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            acc, hits = [0.0, 0.0, 0.0], 0
            for sy in range(SS):
                for sx in range(SS):
                    c = colour_at((px + (sx + 0.5) / SS) / size, (py + (sy + 0.5) / SS) / size)
                    if c:
                        hits += 1
                        for i in range(3):
                            acc[i] += c[i]
            if hits:
                row += bytes(round(acc[i] / hits) for i in range(3)) + bytes([round(255 * hits / (SS * SS))])
            else:
                row += b"\0\0\0\0"
        rows.append(bytes(row))
    return rows


def png(size, rows):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + r for r in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def ico(images):
    """images: [(size, png bytes)] -> .ico bytes with PNG entries."""
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for size, data in images:
        dim = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    return out + b"".join(data for _, data in images)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    images = [(s, png(s, render(s))) for s in SIZES]
    os.makedirs(os.path.join(root, "assets"), exist_ok=True)
    dest = os.path.join(root, "assets", "app.ico")
    with open(dest, "wb") as f:
        f.write(ico(images))
    print("Wrote %s (%s px)" % (dest, ", ".join(map(str, SIZES))))
    if len(sys.argv) > 2 and sys.argv[1] == "--preview":
        with open(sys.argv[2], "wb") as f:
            f.write(images[-1][1])
        print("Preview: %s" % sys.argv[2])


if __name__ == "__main__":
    main()
