# Astera Loader 0.2.2

Starting `AsteraLoader.exe` by itself now works, and plugins' input works
with the DLSS mod's frame generation. Plugin API 1.8 as in 0.2.0; plugins
built for any earlier version keep working. 0.2.1 went to a closed beta
only; this release contains its fix.

Two downloads:

- `AsteraLoader-0.2.2.zip`: the loader. Install it as an MO2 mod over
  0.2.0, or as a new one (see the README).
- `AsteraLoader-plugin-kit-0.2.2.zip`: for plugin authors. The C header,
  the plugin guide and a small example plugin that builds with CMake.

## Before you install: Windows Defender

As with earlier releases, Windows Defender may remove `AsteraLoader.exe` as
`Behavior:Win32/DefenseEvasion.A!ml`, because the launcher starts the game
paused and loads its DLL into it. An exclusion for the Astera Loader mod
folder covers the new files too; the README shows how.

## Fixed

- **Starting `AsteraLoader.exe` directly.** For a manual install, starting
  the launcher by double-click or a shortcut closed the game at once, and
  Steam started it again without Astera Loader. Steam and Mod Organizer 2
  give the game Steam's app id for Monster Hunter: World; the launcher now
  gives it the same id when it was started any other way. Steam must be
  running (the launcher says so if it is not), and Steam still checks that
  you own the game. Steam's Play button needs the launch options, as
  before, to start Astera Loader.
- **Input with frame generation** (from 0.2.1). With the DLSS mod's frame
  generation on (RTX 40 and 50 series), the game gets a swap chain from the
  DLSS mod that has no window of its own. Astera Loader looked for the game
  window only there, so plugins' hotkeys, key and pad state, input capture
  and window messages did nothing; in CMFM the mouse wheel did not scroll.
  Astera Loader now uses the window the game created its swap chain for.
- A launcher placed in a folder of its own, away from
  `MonsterHunterWorld.exe`, says so more plainly.
- For plugin authors: `al_d3d12_swapchain_t.window` is the game's window,
  also behind such a swap chain. The core log's `d3d12_state` line says
  where it came from (`swapchain_window=chain`, `creation` or `none`), and
  the launcher log says where the game's Steam app id came from
  (`steam_app_id=... source=inherited` or `launcher`).

## Tested

With MHW 421810 under Mod Organizer 2: CMFM and the DLSS mod 0.28.4 with
frame generation at 2x, which hands the game that swap chain; Astera Loader
found the game window, its input layer received the window's messages, and
the game exited with code 0. Outside the game, the test suite covers the
window behind such a swap chain, an ordinary swap chain, and neither, each
through a resize; and the launcher started directly, with and without an
app id from Steam or MO2.

## Known issues

As in 0.2.0:

- SharpPluginLoader 2.0.0 together with a large set of Stracker plugins
  fails to set up some of its own hooks at start-up, without Astera Loader
  too.
- Some Stracker plugins (seen: auto_food_skill, more_auto_shoutouts,
  FextyMod) crash while the game exits, on their own threads. The game was
  closing anyway; Astera Loader names them in its crash report and shows no
  notice.
- Leftover Stracker's Loader files: Astera Loader warns, but Stracker's
  Loader starts as well. Remove it.
- Input capture covers the keyboard, the mouse and XInput pads.
- DirectX 11 mode: plugins load, but the Direct3D 12 services are
  unavailable.

## Licence

MIT. Third-party notices (MinHook, Stracker's Loader, SharpPluginLoader) are
in the `licences` folder.
