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

## Live co-op

Two players running WASD in a Seamless Co-op session can see each other live: your partner's health, area and
position on your map, the bosses they've beaten and the items they've found, all filtered by *your* spoiler setting.

Nothing is hosted for it and nothing costs money. One of you (the host) runs the link inside WASD, and
[Tailscale](https://tailscale.com), free for personal use, connects the other to it. No port forwarding and no router
changes.

**Once, on both PCs:** install Tailscale and sign in. The host shares their PC with the partner from Tailscale's admin
console (the PC's **⋯** menu → **Share**), and the partner accepts the link.

**The host:** on the live page open **Co-op ▾**, click **Host the relay here**, then **Forward it with Tailscale**.
Copy the `relay_url=…` line it shows and send it to your partner.

**The partner:** open **Co-op ▾**, paste that line into the address box and press **Save**.

That's it. If you both use the same Seamless Co-op session password, as you do to play together, WASD pairs you by
itself. Otherwise the host can create a session and send the six-letter code. Technical details are under "Co-op
relay" in [docs/TECHNICAL.md](docs/TECHNICAL.md).

## Safety

WASD is an external, read-only companion application.

It does not inject code into Dark Souls III, modify game memory, or operate during normal vanilla online play.

### What leaves your PC

Nothing, unless you use live co-op. When you do:

- Your character and progress go to your partner's WASD: name, level, attributes, health, FP and stamina, souls,
  NG cycle, area and position, play time and deaths, the bosses, key items and pickups you've found, and the summaries
  of your recorded sessions (time and deaths per area, levels, boss fights) that the results page compares. It passes
  through the host's WASD over Tailscale and isn't kept there. The same data is in a co-op code you copy by hand.
- To pair you by itself, WASD reads the session password from Seamless Co-op's settings file
  (`SeamlessCoop\ds3sc_settings.ini`). The password never leaves your PC: only a one-way hash of it goes to the host's
  relay, to find your partner. Turn this off with `auto_pair=0` in `settings.ini`.
- The host's relay only accepts connections from its own PC, which is where Tailscale delivers the partner's. Nothing
  on your network or the internet can reach it.
- WASD only changes Tailscale's settings when the host clicks **Forward it with Tailscale** or **Stop forwarding with
  Tailscale**.

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
