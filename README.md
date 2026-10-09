# WASD

**World Awareness & State Display**

WASD is a free, community-driven live game companion.

The first supported game is **Dark Souls III**, with support designed around Seamless Co-op on PC.

## What it does

WASD can provide:

- live character and equipment information
- item and progression tracking
- boss information and resistances
- spoiler-controlled hints
- NPC quest and missable guidance
- live route mapping
- build and upgrade guidance
- session statistics and post-session analysis

## Spoiler control

Guidance can be adjusted depending on how much information you want:

- Off
- Vague
- Category
- Full

## Safety

WASD is an external, read-only companion application.

It does not inject code into Dark Souls III, modify game memory, or operate during normal vanilla online play.

## Status

WASD is currently in development.

The first public release will support Dark Souls III with Seamless Co-op.

## Installing

When a release is out, download `WASD-<version>-setup.exe` from the GitHub Releases page and run it. It installs for
your Windows user only (no administrator prompt) into `%LOCALAPPDATA%\Programs\WASD`; your settings and session
history live in `%LOCALAPPDATA%\WASD` and survive upgrades. Uninstall from Settings → Apps.

**Needs:** Windows 10 or 11 (x64), Dark Souls III on Steam with Seamless Co-op. On first start WASD reads the item,
map and name data it needs from your own game install; nothing from the game is shipped with it.

## Building from source

With Visual Studio 2022 or later (C++ desktop workload), Python 3 and Node.js:

```powershell
.\build.ps1      # builds build\WASD.exe (the app) and build\wasd-cli.exe (the command-line tool)
.\test.ps1       # runs every test suite
```

How each value is read from the game, how every number was verified, and the full list of commands are in
[docs/TECHNICAL.md](docs/TECHNICAL.md).

## Website

https://wasd-9cz.pages.dev/

## Roadmap

See [ROADMAP.md](./ROADMAP.md).

## Feedback

Bug reports and feature requests can be submitted through GitHub Issues.

## Licence

WASD is free software under the **GNU General Public License, version 3 or later** ([LICENSE](LICENSE)). Facts about
the game documented by other projects (memory layouts, file formats, flag ids, formulas) are credited, with their
licences, in [data/THIRD_PARTY_NOTICES.md](data/THIRD_PARTY_NOTICES.md).

## Disclaimer

WASD is unofficial software and is not affiliated with or endorsed by FromSoftware, Bandai Namco Entertainment, Valve, or the developers of Seamless Co-op.

Dark Souls and related names are trademarks of their respective owners.
