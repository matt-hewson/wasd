WASD: World Awareness & State Display
=====================================

A read-only companion for Dark Souls III played through Seamless Co-op:
an overlay over the game, a live page for a second monitor, and a results
page of your sessions. It reads the game's memory and never changes it,
and it only attaches when Seamless Co-op is loaded.

Starting it
  Start menu -> WASD. Start Dark Souls III through Seamless Co-op; WASD
  attaches by itself. The first time, it reads item positions from your
  DS3 install (about 10 seconds). Pin the WASD window's taskbar button to
  start it from the taskbar.

In game
  F9   cycle the spoiler tier (Off / Vague / Category / Full)
  F10  hide the overlay and pause all memory reads
  F11  show or hide the performance figures

Your data
  Settings, sessions, reports, map data and logs are in
  %LOCALAPPDATA%\WASD. Updates and uninstalling keep them unless you
  choose otherwise.

Uninstalling
  Settings -> Apps -> Installed apps -> "WASD: World Awareness & State
  Display" -> Uninstall. It asks whether to delete your data too.

The command-line tool, bin\wasd-cli.exe, has the developer commands
(`wasd-cli.exe paths` shows where everything lives).

WASD is free software under the GNU GPL, version 3 or later (LICENSE.txt).
Credits for the game facts it uses, and the bundled Python's licence: see
data\THIRD_PARTY_NOTICES.md.
