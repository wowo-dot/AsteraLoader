# Astera Loader 0.2.1

Went to the closed beta only (2026-10-14); 0.2.2 replaces it and contains
this fix.

A fix for plugins' keyboard, mouse and pad input while the DLSS mod's
frame generation is on. Plugin API 1.8 as in 0.2.0; plugins built for any
earlier version keep working.

Two downloads:

- `AsteraLoader-0.2.1.zip`: the loader. Install it as an MO2 mod over
  0.2.0, or as a new one (see the README).
- `AsteraLoader-plugin-kit-0.2.1.zip`: for plugin authors. The C header,
  the plugin guide and a small example plugin that builds with CMake.

## Before you install: Windows Defender

As with earlier releases, Windows Defender may remove `AsteraLoader.exe` as
`Behavior:Win32/DefenseEvasion.A!ml`, because the launcher starts the game
paused and loads its DLL into it. An exclusion for the Astera Loader mod
folder covers the new files too; the README shows how.

## Fixed

- **Input with frame generation.** With the DLSS mod's frame generation on
  (RTX 40 and 50 series), the game gets a swap chain from the DLSS mod that
  has no window of its own. Astera Loader looked for the game window only
  there, so plugins' hotkeys, key and pad state, input capture and window
  messages did nothing; in CMFM the mouse wheel did not scroll. Astera
  Loader now uses the window the game created its swap chain for.
- For plugin authors: `al_d3d12_swapchain_t.window` is that window too, also
  behind such a swap chain. The core log's `d3d12_state` line says where it
  came from (`swapchain_window=chain`, `creation` or `none`).

## Tested

With MHW 421810 under Mod Organizer 2: CMFM and the DLSS mod 0.28.4 with
frame generation at 2x, which hands the game that swap chain; Astera Loader
found the game window, its input layer received the window's messages, and
the game exited with code 0. Outside the game, the test suite covers the
window behind such a swap chain, an ordinary swap chain, and neither, each
through a resize.

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
