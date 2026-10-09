# Licence and credits

## WASD's licence

WASD is free software, released under the **GNU General Public License,
version 3 or (at your option) any later version** (GPL-3.0-or-later).
The full text is `LICENSE` at the top of the repository (installed next
to the program). You may use, change and share WASD; copies and changed
versions you distribute must stay under the GPL, with their source.

Dark Souls III and its data belong to FromSoftware and Bandai Namco.
WASD only reads the game's memory and the files of your own install; it
ships none of the game's data (see "Game data" below).

## Credits: facts about the game

These projects documented facts about Dark Souls III that WASD relies on:
memory layouts, byte patterns, file formats, flag ids, formulas. **Facts
only: no code, data tables or text from them are copied**, except
BinderTool's archive keys (below). Every fact was checked against the
game, and the code using it was written for this project.

| Source | Licence | Facts used | Where |
|---|---|---|---|
| [darksoulsiii-practice-tool](https://github.com/veeenu/darksoulsiii-practice-tool) (veeenu) | AGPL-3.0 | Pointer chains; CharacterStats layout; param table layout; per-param field offsets | `src/main.cpp` |
| [DS3RuntimeScripting](https://github.com/AmySouls/DS3RuntimeScripting) (AmySouls) | MIT | Equipped-slot offsets and slot order; attribute and inventory structure layouts; item id prefixes | `src/main.cpp` |
| [Paramdex](https://github.com/soulsmods/Paramdex) (soulsmods) | none stated | Param field layouts (weapons, armour, item lots, NPCs, materials) | `src/main.cpp` |
| Souls Modding wiki | none stated | `CalcCorrectGraph` (stat scaling curves) | `src/main.cpp` |
| [Dark Souls III CT](https://github.com/The-Grand-Archives/Dark-Souls-III-CT-TGA) (The Grand Archives) | none stated | Map section, play region, last bonfire, death count, play time, NG cycle and character name offsets | `src/main.cpp` |
| [SoulSplitter](https://github.com/FrankvdStam/SoulSplitter) (FrankvdStam) | GPL-3.0 | How event flags are stored and read; boss-defeated flag ids | `src/main.cpp`, `data/bosses.tsv` |
| [ds3-attack-rating-calculator](https://github.com/Derling/ds3-attack-rating-calculator) (Derling) | none stated | The shape of the attack-rating calculation | `src/main.cpp` |
| [Fextralife DS3 wiki](https://darksouls3.wiki.fextralife.com/) | site terms | Formulas and breakpoints: soul level cost, equip load, rings, attunement, stamina, roll tiers, poise; expected values in tests | `src/main.cpp`, `tests/cpp/test_formulas.cpp` |
| [BinderTool](https://github.com/Atvaark/BinderTool) (Atvaark) | MIT | The archive (BHD5 / BDT) layout, name hash and DCX container; **the archives' public RSA keys and the regulation file's AES key, copied as constants** | `tools/ds3_archive.py` |
| [SoulsFormats](https://github.com/soulsmods/SoulsFormatsNEXT) (soulsmods) | GPL-3.0 | The MSB3 map layout (param lists, parts, Treasure events), the BND4 and FMG text layouts, the EMEVD event-script header | `tools/` |
| [DarkScript3](https://github.com/AinTunez/DarkScript3) (AinTunez and contributors) | none stated | What DS3's event-script instructions do and how their arguments are laid out (its `ds3-common.emedf.json`), used to read the NPC questlines behind `data/missables.tsv` | research for `data/missables.tsv` |

WASD's byte patterns (how it finds `WorldChrMan`, `GameDataMan`,
`GameMan`, the event flags, `FieldArea`, the param tables and the `XA`
offset in the running game) are its own: found in the game's code by
`tools/find_patterns.py`, not taken from any of
the projects above.

The radare2 disassembler was used to read the game's code during
research; nothing from it is part of WASD.

### BinderTool's notice

The archive keys in `tools/ds3_archive.py` come from BinderTool, under
this licence:

```
The MIT License (MIT)

Copyright (c) 2015 Atvaark

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Game data

The tables WASD reads from your own install (`data/generated/`:
`treasures.tsv`, `enemies.tsv`, `item_names.tsv`, `bonfire_names.tsv`,
`place_names.tsv`, `regions.tsv`, `boss_names.tsv`, holding the game's
own English names) are the game's data. They are generated on your PC,
not part of the repository and not distributed; an installed copy writes
them to `%LOCALAPPDATA%\WASD\generated\`.

The guidance tables in `data/` (`bosses.tsv`, `key_items.tsv`,
`route.tsv`, `missables.tsv`, `region_areas.tsv`) are WASD's own: written
for this project or derived from the game's files, with flag ids from the
game (credited above where a source documented them).

## Bundled Python (installed copies only)

An installed copy of WASD ships python.org's **embeddable Python 3.14.8
for Windows x64** in `python\`, to run the map-file reader.
It is fetched by `tools/fetch_python.ps1` from
<https://www.python.org/ftp/python/3.14.8/python-3.14.8-embed-amd64.zip>,
checked against python.org's published SHA-256, and trimmed to the files
the reader needs; nothing in it is modified.

**Licence:** Python is distributed under the **PSF License Agreement**
(with the licences of its bundled components, such as libffi and the
Microsoft Visual C++ runtime DLLs Python ships). The full text is the
package's own `LICENSE.txt`, installed beside it as
`python\LICENSE.txt`. The PSF licence allows redistribution with that
notice, and is compatible with the GPL.
