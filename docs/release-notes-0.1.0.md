# Astera Loader 0.1.0

The first public release of Astera Loader, a plugin loader for Monster
Hunter: World on PC (DirectX 12). It replaces Stracker's Loader: your
Stracker plugins keep working, with no `dinput8.dll` or `loader.dll` in the
game folder, and it works alongside SharpPluginLoader.

Two downloads:

- `AsteraLoader-0.1.0.zip`: the loader. Install it as an MO2 mod (see the
  README).
- `AsteraLoader-plugin-kit-0.1.0.zip`: for plugin authors. The C header,
  the plugin guide and a small example plugin that builds with CMake.

## Before you install: Windows Defender

Windows Defender may remove `AsteraLoader.exe` as
`Behavior:Win32/DefenseEvasion.A!ml`, because the launcher starts the game
paused and loads its DLL into it, as launcher-style loaders do. Add an
exclusion for the Astera Loader mod folder; the README shows how. The source
is open, so you can check what it does.

VirusTotal for this release's files (2026-10-11):

| File | SHA-256 | Result |
|---|---|---|
| `AsteraLoader.exe` | `4d24f70a21dba904f6c9224048825023a2fffeec161e4e7eb918ec5600548942` | 1/75 (Trapmine, machine learning) |
| `AsteraLoader\AsteraLoader.dll` | `947934b4ee6f8b063b1aca19a9f9294a377ae61407f036d6edad1f41b315fd83` | 0/75 |
| `stracker_compat.dll` | `cee418446a796a80f4071afedc2e5ab2f8c00ee2e0118a9e7054d2f00d7a78b4` | 1/75 (Cynet, machine learning) |
| `loader.dll` | `f2f415a2d2c25c87c531a990df0c13e38b10e143be5972c6e77f76072e64a092` | 0/75 |

Archive `AsteraLoader-0.1.0.zip`: SHA-256
`62e172ed33eb3b57c5fe1f4e2e8d040d19fbb3e70f7f89cbfa90f7540fa7285a`. Plugin
kit `AsteraLoader-plugin-kit-0.1.0.zip`: SHA-256
`200189390ed08f9e11a0472eeba3a04b4f5f1e146a58d0c6d4cfea96c6eafb1c`.

## What it does

- **A launcher**, `AsteraLoader.exe`, that starts the game with Astera
  Loader. Nothing goes into the game folder, and the game's files and
  protections are left alone. No administrator rights, no network access.
- **Stracker plugins**, from `nativePC\plugins` as before, through
  Astera Loader's own stand-in for Stracker's `loader.dll`.
- **Astera Loader plugins** in `AsteraLoader\plugins\<name>\`, with a C API
  (version 1.7):
  - load phases from before the game's code starts to its shutdown,
    including GAME_READY (the title screen) and EXITING (the game is
    quitting: stop your threads);
  - shared Direct3D 12 and game-function hooks, so several mods can hook
    the same function in a set order;
  - a per-frame tick and a task queue on the game thread;
  - hotkeys, key and pad state, and input capture for in-game menus;
  - game identity, a cached pattern scanner, per-plugin logs and data
    folders.
- **Visible failures:** a plugin that cannot start, or a game version a
  plugin does not support, gets a warning at start-up instead of a crash.
  If the game crashes, a report names the plugin, or the other mod's DLL,
  whose code faulted.

## Tested

With MHW 421810 (the final version) under Mod Organizer 2, with the Steam
overlay on and off: a set of 19 Stracker plugins, a Direct3D 12 plugin in
development (a DLSS / Frame Generation mod), input capture (keyboard, mouse,
XInput pad), GAME_READY and a clean Exit Game, over about 8 minutes of play
with loading screens, menus, alt-tab and window changes. SharpPluginLoader
2.0.0 force-loaded beside it, without the Stracker plugins: both start and
exit cleanly. With Stracker's Loader left installed, Astera Loader warns at
start-up and the game runs.

A manual install into the game folder, started with Steam's launch options
(`"<game folder>\AsteraLoader.exe" %command%`): starts and exits cleanly.

## Known issues

- SharpPluginLoader 2.0.0 together with a large set of Stracker plugins (19
  in our test) fails to set up some of its own hooks at start-up, and the
  game then crashes in SharpPluginLoader. This happens without Astera Loader
  too, with Stracker's own loader and the same plugins; SharpPluginLoader on
  its own starts normally. Its log
  (`nativePC\plugins\CSharp\Loader\SharpPluginLoader.log`) shows
  "IP relative memory operand is too far away".
- Some Stracker plugins (seen: auto_food_skill, more_auto_shoutouts,
  FextyMod) crash while the game exits, on their own threads. The game was
  closing anyway; Astera Loader names them in its crash report and shows no
  notice.
- If Stracker's Loader is still installed (`dinput8.dll` and `loader.dll` in
  the game folder, or its MO2 mod enabled), Astera Loader loads the Stracker
  plugins and shows a warning, but Stracker's Loader starts as well. Remove
  it.
- Input capture covers the keyboard, the mouse and XInput pads; DirectInput
  pads are not captured. Steam Input has not been tested.
- DirectX 11 mode: plugins load, but the Direct3D 12 services are
  unavailable.

## Licence

MIT. Third-party notices (MinHook, Stracker's Loader, SharpPluginLoader) are
in the `licences` folder.
