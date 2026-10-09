"""Extract treasure (item pickup) positions from DS3's map files.

Reads map/mapstudio/*.msb.dcx straight out of the user's own Data5 / DLC1 /
DLC2 archives
(read-only, see ds3_archive.py) and writes data/generated/treasures.tsv:

    map  lot  lot2  x  y  z  region  part

`region` is the play region the pickup stands in (-1 if unknown), the same
id the game reports for the player; data/region_areas.tsv names its area.

Also writes data/generated/enemies.tsv (map, entity, npcParam, thinkParam,
x, y, z, part): every placed enemy, used to find each boss's NpcParam row
(boss resistances).

And data/generated/item_names.tsv (category, id, name): the game's own
English item names, read from msg/engus/item_dlc2.msgbnd.dcx in Data1; and
bonfire_names.tsv (id, name): each bonfire in the regulation's
BonfireWarpParam, keyed by the id the game reports as the last bonfire,
named from the menu text; place_names.tsv (id, name): the area-banner
names; regions.tsv (region, label): each play
region labelled with its area and nearest bonfire (step 4); and
boss_names.tsv (flag, name): each boss in data/bosses.tsv named as on its
health bar, from the event scripts (step 5.2). Skipped when the folder
has no Data1 (all) or no Data0.bdt (bonfires, regions, bosses).
--region-areas FILE also writes the committed region -> area table
(data/region_areas.tsv); see "play regions" below for its rules.

`lot` is the ItemLotParam row id; the guide joins it to the live
ItemLotParam for item names and pickup flags. The output is game data, so
it is generated locally and not committed (data/generated/ is ignored).

MSB3 layout as documented by SoulsFormats (soulsmods/SoulsFormatsNEXT,
GPL-3.0; layout facts only, reimplemented here):
  - "MSB " header (0x10 bytes), then params in order MODEL, EVENT, POINT,
    ROUTE, LAYER, PARTS, ...; each param: version i32, offsetCount i32,
    nameOffset i64, (offsetCount-1) entry offsets i64, nextParamOffset i64.
  - Event entry: nameOffset i64, eventId i32, type u32 (4 = Treasure),
    id i32, 0, baseDataOffset i64, typeDataOffset i64 (relative to entry).
    Treasure type data: 0, 0, partIndex i32, 0, itemLot1 i32, itemLot2 i32.
  - Part entry: nameOffset i64, type u32, id i32, modelIndex i32, 0,
    sibOffset i64, position f32x3 (+0x20), rotation, scale ...
    Part indices count across the whole PARTS param in file order.

Usage: python -I tools/extract_treasures.py ["<DS3 Game folder>"] [--out <folder>]

--out: where to write the files (default data/generated/ in this repo).
WASD's installed copy runs this with its bundled Python and
--out %LOCALAPPDATA%/WASD/generated.

Exit codes: 0 done, 2 game folder not found, 3 no map archives in it,
4 an archive couldn't be read.

Test-only: WASD_RSA_KEYS_FILE names a JSON file {"Data5": "<base64 key>", ...}
that replaces the archive keys, so tests can use synthetic archives signed
with a key of their own. A real install never sets it.
"""
import argparse
import collections
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ds3_archive  # noqa: E402

# Steam's default install location, for running this by hand; the app always passes the folder it found.
DEFAULT_GAME_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\DARK SOULS III\Game"

# Map sections in the base game and DLCs (m40_00 Cemetery/Firelink etc.).
MAPS = ["m30_00_00_00", "m30_01_00_00", "m31_00_00_00", "m32_00_00_00", "m33_00_00_00",
        "m34_01_00_00", "m35_00_00_00", "m37_00_00_00", "m38_00_00_00", "m39_00_00_00",
        "m40_00_00_00", "m41_00_00_00", "m45_00_00_00", "m46_00_00_00", "m47_00_00_00",
        "m50_00_00_00", "m51_00_00_00", "m51_01_00_00"]


def _utf16z(b, off):
    end = off
    while b[end:end + 2] != b"\0\0":
        end += 2
    return b[off:end].decode("utf-16-le")


def read_params(msb):
    if msb[:4] != b"MSB ":
        raise ValueError("not an MSB")
    params = {}
    pos = 0x10
    while pos:
        _ver, count = struct.unpack_from("<ii", msb, pos)
        (name_off,) = struct.unpack_from("<q", msb, pos + 8)
        offsets = struct.unpack_from("<%dq" % (count - 1), msb, pos + 16)
        (nxt,) = struct.unpack_from("<q", msb, pos + 16 + 8 * (count - 1))
        params[_utf16z(msb, name_off)] = list(offsets)
        pos = nxt
    return params


# Play region of a part (per-area items). Objects (type 1,
# CollisionPartIndex at type data +0x08) and enemies (type 2, +0x1C) name
# the collision part (type 5) they stand on -- an index into the whole
# PARTS list -- and a collision carries PlayRegionID at type data +0x38,
# the same id the game reports for the player (region_areas.tsv).
#
# m40 holds both the Cemetery of Ash / Firelink Shrine and Untended Graves
# / Dark Firelink on the same geometry; its collisions only carry the
# Untended ids (4000xx). A part's MapStudioLayer (+0x48) tells the worlds
# apart: bit 0 clear = Untended only (checked against the wiki's Untended
# Graves list: 11 of 11 named matches; all 5 pickups taken in normal
# Firelink have it set). Normal-world parts get the matching 4001xx id
# (400001 Untended Graves <-> 400101 Cemetery of Ash, 400002 Dark Firelink
# <-> 400102 Firelink Shrine, 400000 <-> 400100 the Gundyr arena).
def _read_parts(msb, params):
    parts = []
    for off in params["PARTS_PARAM_ST"]:
        name_off, ptype = struct.unpack_from("<qI", msb, off)
        x, y, z = struct.unpack_from("<3f", msb, off + 0x20)
        (layer,) = struct.unpack_from("<I", msb, off + 0x48)
        (type_off,) = struct.unpack_from("<q", msb, off + 0xB8)
        t = off + type_off
        (entity_off,) = struct.unpack_from("<q", msb, off + 0xB0)
        (entity,) = struct.unpack_from("<i", msb, off + entity_off)
        coll = region = place = None
        if ptype == 1:
            (coll,) = struct.unpack_from("<i", msb, t + 0x08)
        elif ptype == 2:
            (coll,) = struct.unpack_from("<i", msb, t + 0x1C)
        elif ptype == 5:
            (region,) = struct.unpack_from("<i", msb, t + 0x38)
            # The place name shown on this floor: an id in the place-name text,
            # stored negated on most floors; -1 (or 0) = none.
            (name_id,) = struct.unpack_from("<h", msb, t + 0x24)
            place = abs(name_id) if name_id not in (-1, 0) else None
        parts.append({"name": _utf16z(msb, off + name_off), "type": ptype, "pos": (x, y, z),
                      "layer": layer, "coll": coll, "region": region, "place": place, "entity": entity})
    return parts


def _part_regions(map_name, parts):
    """Play region per part index (-1 = unknown); unlinked parts take the
    region of the nearest linked object/enemy in the same world."""
    untended_map = map_name.startswith("m40")
    regions = []
    for p in parts:
        r = -1
        c = p["coll"]
        if c is not None and 0 <= c < len(parts) and parts[c]["type"] == 5 and parts[c]["region"]:
            r = parts[c]["region"]
            if untended_map and p["layer"] & 1 and 400000 <= r < 400100:
                r += 100
        regions.append(r)
    linked = [(p["pos"], regions[i], p["layer"] & 1) for i, p in enumerate(parts)
              if regions[i] > 0 and p["type"] in (1, 2)]
    for i, p in enumerate(parts):
        # Any placed part (also dummy objects/enemies, types 9/10, which have
        # no collision link) -- but not collisions or map pieces themselves.
        if regions[i] > 0 or p["type"] in (0, 5) or not linked:
            continue
        world = p["layer"] & 1
        cands = [l for l in linked if not untended_map or l[2] == world] or linked
        best = min(cands, key=lambda l: sum((a - b) ** 2 for a, b in zip(l[0], p["pos"])))
        regions[i] = best[1]
    return regions


def treasures(msb, map_name=""):
    params = read_params(msb)
    parts = _read_parts(msb, params)
    regions = _part_regions(map_name, parts)
    out = []
    for off in params["EVENT_PARAM_ST"]:
        name_off, _eid, etype = struct.unpack_from("<qiI", msb, off)
        if etype != 4:
            continue
        (type_off,) = struct.unpack_from("<q", msb, off + 0x20)
        t = off + type_off
        part_idx, = struct.unpack_from("<i", msb, t + 8)
        lot1, lot2 = struct.unpack_from("<ii", msb, t + 0x10)
        if lot1 <= 0 and lot2 <= 0:
            continue
        if 0 <= part_idx < len(parts):
            pname, (x, y, z), region = parts[part_idx]["name"], parts[part_idx]["pos"], regions[part_idx]
        else:
            pname, x, y, z, region = "", float("nan"), float("nan"), float("nan"), -1
        out.append((lot1, lot2, x, y, z, region, pname))
    return out


def enemies(msb):
    """Enemy parts (type 2): entity id, NpcParam id, think id, position, name.
    Part layout (SoulsFormats): entityDataOffset i64 at +0xB0, typeDataOffset
    i64 at +0xB8; entity data starts with EntityID i32; Enemy type data:
    0, 0, ThinkParamID i32, NPCParamID i32."""
    out = []
    for off in read_params(msb)["PARTS_PARAM_ST"]:
        name_off, ptype = struct.unpack_from("<qI", msb, off)
        if ptype != 2:
            continue
        ent_off, typ_off = struct.unpack_from("<qq", msb, off + 0xB0)
        (eid,) = struct.unpack_from("<i", msb, off + ent_off)
        think, npc = struct.unpack_from("<ii", msb, off + typ_off + 8)
        x, y, z = struct.unpack_from("<3f", msb, off + 0x20)
        out.append((eid, npc, think, x, y, z, _utf16z(msb, off + name_off)))
    return out


def extract(game_dir, out_dir, archives=None, log=print):
    """Reads every map in MAPS and writes treasures.tsv and enemies.tsv into
    out_dir. archives: [(name, archive)] with has(path) / read(path); by
    default the game's DLC2, DLC1 and Data5 (the DLCs override Data5: m47
    is in both, and the DLC1 copy is used). Returns (treasures, enemies)."""
    if archives is None:
        archives = [(n, ds3_archive.Archive(game_dir, n)) for n in ("DLC2", "DLC1", "Data5")
                    if os.path.exists(os.path.join(game_dir, n + ".bhd"))]
    os.makedirs(out_dir, exist_ok=True)
    rows, enemy_rows = [], []
    for m in MAPS:
        path = "/map/mapstudio/%s.msb.dcx" % m
        src = next(((n, ar) for n, ar in archives if ar.has(path)), None)
        if not src:
            log("  %s: not found (skipped)" % m)
            continue
        msb = ds3_archive.dcx_decompress(src[1].read(path))
        ts = treasures(msb, m)
        es = enemies(msb)
        log("  %s: %d treasures, %d enemies (%s)" % (m, len(ts), len(es), src[0]))
        rows += [(m,) + t for t in ts]
        enemy_rows += [(m,) + e for e in es]
    # Each file is written to <name>.tmp and renamed into place, so a run that
    # is stopped part way (WASD closed during the first read) never leaves a
    # cut-off file that looks complete.
    dest = os.path.join(out_dir, "treasures.tsv")
    with open(dest + ".tmp", "w", encoding="utf-8", newline="\n") as f:
        f.write("# Generated locally from the user's own DS3 install by tools/extract_treasures.py; not for redistribution.\n")
        f.write("# map\tlot\tlot2\tx\ty\tz\tregion\tpart\n")
        for m, l1, l2, x, y, z, r, p in rows:
            f.write("%s\t%d\t%d\t%.2f\t%.2f\t%.2f\t%d\t%s\n" % (m, l1, l2, x, y, z, r, p))
    os.replace(dest + ".tmp", dest)
    log("Wrote %d treasures to %s" % (len(rows), dest))
    dest = os.path.join(out_dir, "enemies.tsv")
    with open(dest + ".tmp", "w", encoding="utf-8", newline="\n") as f:
        f.write("# Generated locally from the user's own DS3 install by tools/extract_treasures.py; not for redistribution.\n")
        f.write("# map\tentity\tnpcParam\tthinkParam\tx\ty\tz\tpart\n")
        for m, eid, npc, think, x, y, z, p in enemy_rows:
            f.write("%s\t%d\t%d\t%d\t%.2f\t%.2f\t%.2f\t%s\n" % (m, eid, npc, think, x, y, z, p))
    os.replace(dest + ".tmp", dest)
    log("Wrote %d enemies to %s" % (len(enemy_rows), dest))
    return len(rows), len(enemy_rows)


# ---- names, from the game's own text --------------------

MSG_ARCHIVE = "Data1"  # holds msg/engus/*.msgbnd.dcx
ITEM_MSGBND = "/msg/engus/item_dlc2.msgbnd.dcx"  # base game + both DLCs
# category (as data/item_names.tsv and the C++ loader name them) -> the
# FMG files' ids in ITEM_MSGBND: base game, DLC1, DLC2. Checked against the
# files' own names (武器名 = weapon names etc.) in read_item_names.
ITEM_NAME_FMGS = [
    ("Weapon", (11, 211, 251), "武器名"),
    ("Protector", (12, 212, 252), "防具名"),
    ("Accessory", (13, 213, 253), "アクセサリ名"),
    ("Goods", (10, 210, 250), "アイテム名"),
    ("Magic", (14, 214, 254), "魔法名"),
]


def read_fmgs(bnd, ids, stem):
    """Merge the FMG files with these BND ids (later ones win), checking
    each file's name is stem + '' / '_dlc1' / '_dlc2'."""
    files = {fid: (name, body) for fid, name, body in bnd}
    merged = {}
    for fid, suffix in zip(ids, ("", "_dlc1", "_dlc2")):
        if fid not in files:
            raise ValueError("message file %d (%s%s.fmg) not found" % (fid, stem, suffix))
        name, body = files[fid]
        if name.replace("/", "\\").split("\\")[-1] != stem + suffix + ".fmg":
            raise ValueError("message file %d is %r, expected %s%s.fmg" % (fid, name, stem, suffix))
        merged.update(ds3_archive.read_fmg(body))
    return merged


def clean_text(text):
    """A name as one TSV field: tabs and line breaks become spaces, and the
    ends are trimmed. '' means no name."""
    return text.replace("\t", " ").replace("\r", " ").replace("\n", " ").strip()


def read_item_names(archive):
    """[(category, id, name)] from the game's item name texts, sorted."""
    bnd = ds3_archive.read_bnd4(ds3_archive.dcx_decompress(archive.read(ITEM_MSGBND)))
    rows = []
    for category, ids, stem in ITEM_NAME_FMGS:
        for i, text in read_fmgs(bnd, ids, stem).items():
            name = clean_text(text)
            if name:
                rows.append((category, i, name))
    order = {c: k for k, (c, _, _) in enumerate(ITEM_NAME_FMGS)}
    rows.sort(key=lambda r: (order[r[0]], r[1]))
    return rows


MENU_MSGBND = "/msg/engus/menu_dlc2.msgbnd.dcx"
BONFIRE_TEXT_FMGS = ((200, 232, 272), "FDP_メニューテキスト")  # menu text: base, DLC1, DLC2
# BonfireWarpParam (64-byte rows), found from the game's own data:
# +0x00 is the bonfire-lit flag and +0x04 the
# warp id, whose value + 1000 is the "last bonfire" id the game reports
# (both live-verified, README "Event flags"); +0x08 is the only field that
# holds a menu-text id in every row: the bonfire's name (Iudex Gundyr's
# bonfire 4002952 -> 211205 "Iudex Gundyr", as seen in game).
BONFIRE_WARP_ID = 0x04
BONFIRE_TEXT_ID = 0x08


def read_bonfire_names(regulation, archive):
    """[(bonfire id, name)] sorted: one per bonfire in BonfireWarpParam,
    keyed by the id the game reports as the last bonfire rested at."""
    _size, rows = ds3_archive.read_param(ds3_archive.regulation_file(regulation, "BonfireWarpParam.param"),
                                         "BONFIRE_WARP_PARAM_ST")
    ids, stem = BONFIRE_TEXT_FMGS
    menu = read_fmgs(ds3_archive.read_bnd4(ds3_archive.dcx_decompress(archive.read(MENU_MSGBND))), ids, stem)
    names = {}
    for row_id, b in rows:
        (warp,) = struct.unpack_from("<i", b, BONFIRE_WARP_ID)
        (text_id,) = struct.unpack_from("<i", b, BONFIRE_TEXT_ID)
        if text_id not in menu:
            raise ValueError("BonfireWarpParam row %d: text %d not in the menu text" % (row_id, text_id))
        name = clean_text(menu[text_id])
        if not name or warp <= 0:
            raise ValueError("BonfireWarpParam row %d: no name or warp id (%d, %r)" % (row_id, warp, name))
        bonfire = warp + 1000
        if names.get(bonfire, name) != name:
            raise ValueError("bonfire %d has two names: %r and %r" % (bonfire, names[bonfire], name))
        names[bonfire] = name
    return sorted(names.items())


PLACE_NAME_FMGS = ((19, 216, 256), "地名")  # place names (area banners): base, DLC1, DLC2


def read_place_names(archive):
    """[(place id, name)] sorted, from the game's place-name text. Blank
    entries (a single space, unused slots) are left out."""
    ids, stem = PLACE_NAME_FMGS
    texts = read_fmgs(ds3_archive.read_bnd4(ds3_archive.dcx_decompress(archive.read(ITEM_MSGBND))), ids, stem)
    return sorted((i, clean_text(t)) for i, t in texts.items() if clean_text(t))


def write_tsv(dest, header, rows):
    """Write to <dest>.tmp, then rename into place (see extract)."""
    with open(dest + ".tmp", "w", encoding="utf-8", newline="\n") as f:
        f.write("# Generated locally from the user's own DS3 install by tools/extract_treasures.py; not for redistribution.\n")
        f.write(header)
        for r in rows:
            f.write("\t".join(str(x) for x in r) + "\n")
    os.replace(dest + ".tmp", dest)


def extract_names(game_dir, out_dir, archive=None, regulation=None, log=print):
    """Writes item_names.tsv, place_names.tsv and bonfire_names.tsv into
    out_dir from the game's text (and, for bonfires, its regulation file). Returns
    (item names, bonfire names); None for a table whose source the game
    folder doesn't have (no Data1 archive, no Data0.bdt)."""
    if archive is None:
        if not os.path.exists(os.path.join(game_dir, MSG_ARCHIVE + ".bhd")):
            log("  names: no %s.bhd (skipped)" % MSG_ARCHIVE)
            return None, None
        archive = ds3_archive.Archive(game_dir, MSG_ARCHIVE)
    os.makedirs(out_dir, exist_ok=True)
    rows = read_item_names(archive)
    dest = os.path.join(out_dir, "item_names.tsv")
    write_tsv(dest, "# The game's English item names (msg/engus/item_dlc2.msgbnd.dcx).\n# category\tid\tname\n", rows)
    log("Wrote %d item names to %s" % (len(rows), dest))
    places = read_place_names(archive)
    dest = os.path.join(out_dir, "place_names.tsv")
    write_tsv(dest, "# The game's place names, as on the area banners (msg/engus/item_dlc2.msgbnd.dcx).\n"
                    "# 1000-2008 are unused leftovers; DS3's own run 3000-5400.\n# id\tname\n", places)
    log("Wrote %d place names to %s" % (len(places), dest))
    if regulation is None:
        if not os.path.exists(os.path.join(game_dir, ds3_archive.REGULATION_FILE)):
            log("  bonfire names: no %s (skipped)" % ds3_archive.REGULATION_FILE)
            return len(rows), None
        regulation = ds3_archive.read_regulation(game_dir)
    bonfires = read_bonfire_names(regulation, archive)
    dest = os.path.join(out_dir, "bonfire_names.tsv")
    write_tsv(dest, "# The game's bonfire names: BonfireWarpParam (warp id + 1000 = the last-bonfire id the game\n"
                    "# reports) -> its menu text (msg/engus/menu_dlc2.msgbnd.dcx).\n# id\tname\n", bonfires)
    log("Wrote %d bonfire names to %s" % (len(bonfires), dest))
    return len(rows), len(bonfires)


# ---- play regions: labels and areas ---------------------
#
# The game's play regions are PlayRegionParam's rows; a region counts when
# some map floor carries it (the game reports the region of the floor you
# stand on). Each region's area is a place name, decided by the first rule
# that gives one:
#   0. AREA_OVERRIDES: calls made by hand where the game data is thin or
#      points across a border (user's calls, 2026-10-09, with reasons);
#   1. m40's Untended Graves world: its floors carry 4000xx (the normal
#      world is 4001xx, see _part_regions) -> Untended Graves;
#   2. the region's own floors: the place most of them name;
#   3. its PlayRegionParam group (+0x00): the area most of the group's
#      floor-named regions have;
#   4. its map's own place: mAA_BB -> place AA*100 + BB*10 (the numbering
#      the place-name text follows: m30_01 -> 3010 Lothric Castle).
# A tie decides nothing and falls through. The label is the area plus the
# bonfire nearest the middle of the region's placed objects and enemies
# (same map, same m40 world), e.g. "Undead Settlement (near Cliff Underside)".

# The normal maps and the two DLC2 PvP arenas (no pickups, but they're
# places the player can be).
REGION_MAPS = MAPS + ["m53_00_00_00", "m54_00_00_00"]
UNTENDED_GRAVES = 4010
PVP_ARENAS = (4600, 4700, 5300, 5400)  # places, but not areas: no level, bosses or route
AREA_OVERRIDES = {
    300006: (3000, "the Dancer of the Boreal Valley's arena, where the Dancer is fought (one floor names Lothric Castle)"),
    370010: (3700, "Pontiff Sulyvahn's side of Irithyll (no named floors; its region group is Anor Londo's)"),
    390001: (3900, "part of Irithyll Dungeon (no named floors; its group's named floors are mostly Profaned Capital)"),
    390002: (3900, "part of Irithyll Dungeon (12 floors unnamed; its 2 Profaned Capital floors are the way out)"),
}


def region_evidence(msb, map_name):
    """What a map tells about its play regions: [(region, place or None)] per
    floor, [(region, position)] per placed object and enemy, and
    [(warp id, position, layer)] per part (bonfires are found among them)."""
    parts = _read_parts(msb, read_params(msb))
    regions = _part_regions(map_name, parts)
    floors, placed, entities = [], [], []
    for i, p in enumerate(parts):
        if p["type"] == 5 and p["region"]:
            r = p["region"]
            floors.append((r, p["place"]))
            # m40's floors carry the Untended Graves ids (4000xx); a floor in
            # the normal world too (layer bit 0) also carries its 4001xx twin,
            # which is what the game reports there (README "Current area").
            if map_name.startswith("m40") and p["layer"] & 1 and 400000 <= r < 400100:
                floors.append((r + 100, p["place"]))
        if p["type"] in (1, 2) and regions[i] > 0:
            placed.append((regions[i], p["pos"]))
        if p["type"] == 1 and p["entity"] > 0:
            entities.append((p["entity"], p["pos"], p["layer"]))
    return floors, placed, entities


def _majority(counter):
    if not counter:
        return None
    (top, n), *rest = counter.most_common()
    return None if rest and rest[0][1] == n else top


def decide_areas(region_ids, groups, floors, places):
    """{region: (place id, rule)} for the regions in region_ids (those with
    a floor). groups: {region: PlayRegionParam group}; floors: {region:
    Counter(place id)}; places: the place-name ids with text."""
    out = {}
    for r in region_ids:
        if r in AREA_OVERRIDES:
            out[r] = (AREA_OVERRIDES[r][0], "by hand: " + AREA_OVERRIDES[r][1])
        elif 400000 <= r < 400100:
            out[r] = (UNTENDED_GRAVES, "m40's Untended Graves world")
        elif _majority(floors.get(r, collections.Counter())) is not None:
            out[r] = (_majority(floors[r]), "its floors")
    by_floors = {r: p for r, (p, rule) in out.items() if rule == "its floors"}
    for r in region_ids:
        if r in out or not groups.get(r):
            continue
        group = collections.Counter(p for o, p in by_floors.items() if groups.get(o) == groups[r])
        if _majority(group) is not None:
            out[r] = (_majority(group), "its region group")
    for r in region_ids:
        if r not in out:
            p = (r // 10000) * 100 + ((r // 1000) % 10) * 10
            if p in places:
                out[r] = (p, "its map's place")
    return out


def nearest_bonfire(points, bonfires):
    """The name of the bonfire nearest the middle of points; None without either."""
    if not points or not bonfires:
        return None
    mid = tuple(sum(p[i] for p in points) / len(points) for i in range(3))
    return min(bonfires, key=lambda b: sum((a - c) ** 2 for a, c in zip(b[1], mid)))[0]


def read_regions(archives, regulation, place_names, bonfire_names):
    """([(region, label)], [(region, area name)]) from the map files, the
    regulation and the names; both sorted. Areas leave out the PvP arenas."""
    _size, rows = ds3_archive.read_param(ds3_archive.regulation_file(regulation, "PlayRegionParam.param"),
                                         "PLAY_REGION_PARAM_ST")
    groups = {rid: struct.unpack_from("<i", row, 0)[0] for rid, row in rows if rid}
    floors, placed = collections.defaultdict(collections.Counter), collections.defaultdict(list)
    carried = set()  # regions some floor carries, named or not
    bonfires = collections.defaultdict(list)  # (map, world) -> [(name, position)]
    for m in REGION_MAPS:
        path = "/map/mapstudio/%s.msb.dcx" % m
        src = next((ar for _, ar in archives if ar.has(path)), None)
        if src is None:
            continue
        fl, pl, ents = region_evidence(ds3_archive.dcx_decompress(src.read(path)), m)
        for r, p in fl:
            carried.add(r)
            if p is not None:
                floors[r][p] += 1
        for r, pos in pl:
            placed[(r, m)].append(pos)
        for warp, pos, layer in ents:
            name = bonfire_names.get(warp + 1000)
            if name and pos[1] > -900:  # a few bonfires are parked far below the map
                bonfires[(m, layer & 1 if m.startswith("m40") else 0)].append((name, pos))
    region_ids = sorted(r for r in groups if r in carried)
    areas = decide_areas(region_ids, groups, floors, place_names)
    labels, area_rows = [], []
    for r in region_ids:
        if r not in areas:
            continue
        place, _rule = areas[r]
        name = place_names[place]
        near = None
        for (pr, m), pts in placed.items():
            if pr == r:
                world = 0 if not m.startswith("m40") else (1 if r >= 400100 else 0)
                near = nearest_bonfire(pts, bonfires.get((m, world), []))
        labels.append((r, "%s (near %s)" % (name, near) if near and near != name else name))
        if place not in PVP_ARENAS:
            area_rows.append((r, name))
    return labels, area_rows


AREAS_HEADER = """# Play region id -> area. Written by tools/extract_treasures.py --region-areas from the game's own
# files; don't edit by hand, change the rules or AREA_OVERRIDES there and rewrite.
# Regions: PlayRegionParam's rows that a map floor carries. Area: a place name from the game's text, by
# the first rule that gives one: a call made by hand (AREA_OVERRIDES, with reasons); m40's Untended Graves
# world (4000xx); the place most of the region's floors name; the area most of its PlayRegionParam
# group has; its map's own place (mAA_BB -> AA*100 + BB*10). PvP arenas have no area.
# region_id\tarea
"""


def extract_regions(game_dir, out_dir, archives=None, text_archive=None, regulation=None, areas_out=None,
                    log=print):
    """Writes regions.tsv (labels) into out_dir, and the area table to
    areas_out if given. Returns (labels, areas) as read_regions, or None when the game
    folder lacks Data1 or Data0.bdt."""
    if text_archive is None:
        if not os.path.exists(os.path.join(game_dir, MSG_ARCHIVE + ".bhd")):
            log("  regions: no %s.bhd (skipped)" % MSG_ARCHIVE)
            return None
        text_archive = ds3_archive.Archive(game_dir, MSG_ARCHIVE)
    if regulation is None:
        if not os.path.exists(os.path.join(game_dir, ds3_archive.REGULATION_FILE)):
            log("  regions: no %s (skipped)" % ds3_archive.REGULATION_FILE)
            return None
        regulation = ds3_archive.read_regulation(game_dir)
    if archives is None:
        archives = [(n, ds3_archive.Archive(game_dir, n)) for n in ("DLC2", "DLC1", "Data5")
                    if os.path.exists(os.path.join(game_dir, n + ".bhd"))]
    labels, areas = read_regions(archives, regulation, dict(read_place_names(text_archive)),
                                 dict(read_bonfire_names(regulation, text_archive)))
    os.makedirs(out_dir, exist_ok=True)
    dest = os.path.join(out_dir, "regions.tsv")
    write_tsv(dest, "# The game's play regions, each labelled with its area (a place name from the game's text)\n"
                    "# and the nearest bonfire.\n# region_id\tlabel\n", labels)
    log("Wrote %d play regions to %s" % (len(labels), dest))
    if areas_out:
        with open(areas_out + ".tmp", "w", encoding="utf-8", newline="\n") as f:
            f.write(AREAS_HEADER)
            for r, a in areas:
                f.write("%d\t%s\n" % (r, a))
        os.replace(areas_out + ".tmp", areas_out)
        log("Wrote %d region areas to %s" % (len(areas), areas_out))
    return labels, areas


# ---- bosses: names and areas ---------------------------
#
# A boss is its defeat flag (data/bosses.tsv). Its enemies are the map parts
# with entity flag - 10,000,000 + 0..9 in one map: the defeat entity and the
# ones just after it, which carry the rest of the fight (Abyss Watchers'
# bars sit on +1, the Dancer's on +9). Its name is what the game puts on its
# health bar: the event scripts' instruction 2003:11 (state u8, 3 pad,
# entity i32, slot i16, 2 pad, name id i32: an id in the NPC-name text),
# found from Iudex Gundyr's (4000800 -> 905110) and Champion Gundyr's bars.
# A fight with more than one name takes the user's call (2026-10-09), kept
# as text ids so no name is written here: the fight's own name for fights
# that change name, both names for two bosses fought together. Its area is
# where its enemies stand (the region areas).
NPC_NAME_FMGS = ((18, 215, 255), "NPC名")  # NPC names: base, DLC1, DLC2
HEALTH_BAR = (2003, 11)
BOSS_NAME_CALLS = {
    13200850: (905010,),          # King of the Storm -> Nameless King: the fight's own name
    14500800: (906020,),          # Sister Friede -> Father Ariandel and Friede -> Blackflame Friede
    15000800: (905022,),          # Demon from Below and Demon in Pain -> Demon Prince
    15100800: (45000,),           # Halflight (a stray Friede bar sits on the same entity in m51)
    13410830: (905250, 905251),   # Lorian and Lothric, fought together: both names
    14500860: (30000, 906030),    # the Gravetender and the Greatwolf, fought together: both names
}


def read_emevd_instructions(data, wanted):
    """[args bytes] of every instruction (bank, id) == wanted in a DS3 event
    script (EMEVD, 64-bit little-endian, version 0xCD): header "EVD\\0",
    then i64 pairs from +0x10: events (count, offset), instructions (count,
    offset), ..., arguments (length, offset) at +0x70; each instruction is
    0x20 bytes: bank i32, id i32, args length i64, args offset i64 (into
    the arguments block), layer offset i64."""
    if data[:4] != b"EVD\0" or data[4] != 0 or data[5] != 0xFF:
        raise NotImplementedError("only 64-bit little-endian EMEVD files are supported")
    (version,) = struct.unpack_from("<i", data, 8)
    if version != 0xCD:
        raise NotImplementedError("EMEVD version 0x%x (DS3's is 0xCD)" % version)
    count, offset = struct.unpack_from("<qq", data, 0x20)
    args_len, args_off = struct.unpack_from("<qq", data, 0x70)
    if offset + 0x20 * count > len(data) or args_off + args_len > len(data):
        raise ValueError("EMEVD tables run past the end of the file")
    out = []
    for k in range(count):
        bank, iid, alen, aoff, _layer = struct.unpack_from("<iiqqq", data, offset + 0x20 * k)
        if (bank, iid) == wanted:
            if aoff < 0 or aoff + alen > args_len:
                raise ValueError("EMEVD instruction %d's arguments run past the arguments block" % k)
            out.append(data[args_off + aoff:args_off + aoff + alen])
    return out


def read_health_bars(text_archive, maps):
    """{(map, entity): [name id, ...]} for every boss health bar the maps'
    event scripts show (event/<map>.emevd.dcx, in Data1)."""
    bars = collections.defaultdict(list)
    for m in maps:
        path = "/event/%s.emevd.dcx" % m
        if not text_archive.has(path):
            continue
        for args in read_emevd_instructions(ds3_archive.dcx_decompress(text_archive.read(path)), HEALTH_BAR):
            if len(args) != 16:
                raise ValueError("%s: a health-bar instruction with %d bytes of arguments" % (path, len(args)))
            state, entity, _slot, name_id = struct.unpack_from("<B3xihxxi", args)
            if state == 1 and entity > 0 and name_id > 0 and name_id not in bars[(m, entity)]:
                bars[(m, entity)].append(name_id)
    return bars


def boss_flags(path=None):
    """The defeat flags in data/bosses.tsv (beside tools/ in a checkout and in an install)."""
    path = path or os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "bosses.tsv")
    with open(path, encoding="utf-8") as f:
        return [int(l.split("\t", 1)[0]) for l in f if l.strip() and not l.startswith("#")]


def read_bosses(archives, text_archive, flags, region_area):
    """[(flag, name, area)] in flags' order. region_area: {region: area}.
    Raises ValueError for a boss whose enemies or name can't be found, or
    whose bars show several names without a call in BOSS_NAME_CALLS."""
    npc = read_fmgs(ds3_archive.read_bnd4(ds3_archive.dcx_decompress(text_archive.read(ITEM_MSGBND))), *NPC_NAME_FMGS)
    enemies = {}  # entity -> (map, region)
    for m in REGION_MAPS:
        path = "/map/mapstudio/%s.msb.dcx" % m
        src = next((ar for _, ar in archives if ar.has(path)), None)
        if src is None:
            continue
        msb = ds3_archive.dcx_decompress(src.read(path))
        parts = _read_parts(msb, read_params(msb))
        regions = _part_regions(m, parts)
        for i, p in enumerate(parts):
            if p["type"] == 2 and p["entity"] > 0:
                enemies.setdefault(p["entity"], (m, regions[i]))
    bars = read_health_bars(text_archive, REGION_MAPS)
    out = []
    for flag in flags:
        span = [flag - 10000000 + k for k in range(10)]
        maps = [enemies[e][0] for e in span if e in enemies]
        if not maps:
            raise ValueError("boss %d: no enemy with entity %d..%d in the maps" % (flag, span[0], span[-1]))
        m = maps[0]
        areas = collections.Counter(region_area[enemies[e][1]] for e in span
                                    if e in enemies and enemies[e][0] == m and enemies[e][1] in region_area)
        ids = []
        for e in span:
            ids += [i for i in bars.get((m, e), []) if i not in ids]
        names = []
        for i in ids:
            if npc.get(i) and clean_text(npc[i]) not in names:
                names.append(clean_text(npc[i]))
        if flag in BOSS_NAME_CALLS:
            missing = [i for i in BOSS_NAME_CALLS[flag] if not clean_text(npc.get(i, ""))]
            if missing:
                raise ValueError("boss %d: called name id(s) %s not in the NPC-name text" % (flag, missing))
            name = " and ".join(clean_text(npc[i]) for i in BOSS_NAME_CALLS[flag])
        elif len(names) == 1:
            name = names[0]
        else:
            raise ValueError("boss %d: health bars show %s; add a call to BOSS_NAME_CALLS" % (flag, names or "no name"))
        if not areas:
            raise ValueError("boss %d: its enemies stand in no region with an area" % flag)
        (area, n), *rest = areas.most_common()
        if rest and rest[0][1] == n:
            raise ValueError("boss %d: its enemies stand in two areas equally (%s)" % (flag, dict(areas)))
        out.append((flag, name, area))
    return out


def extract_bosses(game_dir, out_dir, region_area, archives=None, text_archive=None, flags=None, log=print):
    """Writes boss_names.tsv (flag, name) into out_dir. region_area: {region:
    area} (from extract_regions). Returns read_bosses' rows, or None when
    the game folder has no Data1."""
    if text_archive is None:
        if not os.path.exists(os.path.join(game_dir, MSG_ARCHIVE + ".bhd")):
            log("  bosses: no %s.bhd (skipped)" % MSG_ARCHIVE)
            return None
        text_archive = ds3_archive.Archive(game_dir, MSG_ARCHIVE)
    if archives is None:
        archives = [(n, ds3_archive.Archive(game_dir, n)) for n in ("DLC2", "DLC1", "Data5")
                    if os.path.exists(os.path.join(game_dir, n + ".bhd"))]
    rows = read_bosses(archives, text_archive, boss_flags() if flags is None else flags, region_area)
    os.makedirs(out_dir, exist_ok=True)
    dest = os.path.join(out_dir, "boss_names.tsv")
    write_tsv(dest, "# Each boss's name as the game shows it on its health bar (event scripts + NPC-name text).\n"
                    "# flag\tname\n", [(f, n) for f, n, _ in rows])
    log("Wrote %d boss names to %s" % (len(rows), dest))
    return rows


# ---- key items -----------------------------------------
#
# The game's own key-item category: EquipParamGoods' byte +0x3A, found from
# the game's data (1 = key items: keys, tomes, coals, ashes, Cinders...; 0
# consumables, 2 upgrade materials, 5 spells). data/key_items.tsv lists
# exactly these plus KEY_ITEMS_BY_HAND; the real-data suite checks it.
GOODS_CATEGORY = 0x3A
KEY_ITEM_CATEGORY = 1
KEY_ITEMS_BY_HAND = {
    2118: "Loretta's Bone: filed as an ordinary item, but a step of Greirat's questline (user's call 2026-10-09)",
}


def read_key_item_ids(regulation):
    """The goods ids data/key_items.tsv should hold, sorted."""
    _size, rows = ds3_archive.read_param(ds3_archive.regulation_file(regulation, "EquipParamGoods.param"),
                                         "EQUIP_PARAM_GOODS_ST")
    return sorted({r for r, b in rows if b[GOODS_CATEGORY] == KEY_ITEM_CATEGORY} | set(KEY_ITEMS_BY_HAND))


def main(argv=None):
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description="Extract item pickup and enemy positions from DS3's map files.")
    ap.add_argument("game_dir", nargs="?", default=DEFAULT_GAME_DIR, help="the DS3 'Game' folder (holds Data5.bdt)")
    ap.add_argument("--region-areas", metavar="FILE",
                    help="also write the region -> area table (data/region_areas.tsv)")
    ap.add_argument("--out", default=os.path.join(here, "data", "generated"),
                    help="where to write the generated tables (default: data/generated in this repo)")
    args = ap.parse_args(argv)
    keys_file = os.environ.get("WASD_RSA_KEYS_FILE")
    if keys_file:  # tests only, see the module docstring
        with open(keys_file, encoding="utf-8") as f:
            ds3_archive.RSA_KEYS.update(json.load(f))
    if not os.path.isdir(args.game_dir):
        print("Game folder not found: %s" % args.game_dir)
        return 2
    if not any(os.path.exists(os.path.join(args.game_dir, n + ".bhd")) for n in ("Data5", "DLC1", "DLC2")):
        print("No map archives (Data5.bhd) in %s -- is this the DS3 'Game' folder?" % args.game_dir)
        return 3
    try:
        extract(args.game_dir, args.out)
        extract_names(args.game_dir, args.out)
        regions = extract_regions(args.game_dir, args.out, areas_out=args.region_areas)
        if regions is not None:
            extract_bosses(args.game_dir, args.out, dict(regions[1]))
    except (OSError, ValueError, KeyError, NotImplementedError) as e:
        print("Couldn't read the game's archives: %s" % e)
        return 4
    return 0


if __name__ == "__main__":
    sys.exit(main())
