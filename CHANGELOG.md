# Changelog

## 0.2.0 (pre-release)

### New: live co-op

Two players running WASD in a Seamless Co-op session can now see each other live on the live page: your partner's
health, FP and stamina, their area and position on your map, the bosses they've beaten, their item count for the area
you're in, and their deaths beside yours. All of it follows *your* spoiler tier. The results page compares the live
partner while you're connected.

Nothing is hosted and nothing costs money. One player's WASD runs the link, and [Tailscale](https://tailscale.com),
free for personal use, connects the other to it. No port forwarding and no router changes.

- **Host:** **Co-op ▾** → **Host the relay here** → **Forward it with Tailscale** (WASD runs `tailscale serve` for
  you; the same button turns it off again), then send your partner the `relay_url=…` line it shows.
- **Partner:** **Co-op ▾**, paste that line into the address box, **Save**. No settings file to edit, no restart.
- **Pairing:** with the same Seamless Co-op session password, WASD pairs you by itself. Otherwise the host creates a
  session and sends a six-letter code.
- A dropped connection reconnects by itself, and your partner's last values stay on screen, dimmed, until they're
  back.

What leaves your PC, and only when you use live co-op: your character and progress, to your partner's WASD, through the
host's PC, kept nowhere. To pair you, only a one-way hash of your Seamless Co-op password leaves your PC (turn this off
with `auto_pair=0`). The host's relay accepts connections only from its own PC, which is where Tailscale delivers the
partner. See "What leaves your PC" in the [README](README.md).

### Improved

- Boss fights: attempts are counted from the game's own fight flags, with the fight's phase.
- Gear refreshes when you upgrade or pick up an upgrade material.
- Missables: a done questline step stays done at every later step.
- At the Vague tier, the boss note says where weaknesses are shown.
- `settings.ini` takes `live_port`, for a live page on another port.

### Fixed

- Co-op codes pasted into Discord could break: its formatting ate some underscores. Codes now use characters Discord
  leaves alone.
- **Copy my code** could fail without saying so. It now waits until your code is ready, won't accept your own code as
  your partner's, and **Clear** removes your partner's data.
- Switching characters mid-session recorded into the first character's history. Each character now gets its own
  session.

### Known issues

- Live co-op is new: it has been tested on one PC through Tailscale, but not yet between two real players' PCs.
- As the joining player in Seamless Co-op, your own pickups may not show as found.
- Resting at the same bonfire again isn't recorded.
- Item Discovery can be wrong after relaunching the game.
- WASD updates about 6 times a second instead of 10, and uses roughly half of one CPU core.

## 0.1.1 (pre-release)

The first public pre-release: the live page, the in-game overlay, the results page, spoiler tiers, missable warnings,
route guidance for all 23 areas, and co-op codes.
