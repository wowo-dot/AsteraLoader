# Astera Loader 0.2.0

Plugin API 1.8: plugins can change bytes in the game's code and data
through Astera Loader, checked and with one owner per byte. Everything in
0.1.0 stays as it was; plugins built for 0.1.0 (API 1.0 to 1.7) keep
working.

Two downloads:

- `AsteraLoader-0.2.0.zip`: the loader. Install it as an MO2 mod over
  0.1.0, or as a new one (see the README).
- `AsteraLoader-plugin-kit-0.2.0.zip`: for plugin authors. The C header,
  the plugin guide and a small example plugin that builds with CMake.

## Before you install: Windows Defender

As with 0.1.0, Windows Defender may remove `AsteraLoader.exe` as
`Behavior:Win32/DefenseEvasion.A!ml`, because the launcher starts the game
paused and loads its DLL into it. An exclusion for the Astera Loader mod
folder covers the new files too; the README shows how.

VirusTotal for this release's files (2026-10-12):

| File | SHA-256 | Result |
|---|---|---|
| `AsteraLoader.exe` | `325462bac6a8267d70d7c23ae223d62a725e6c7f21e6a6f162e0320fe528a358` | 2/75 (Bkav, Trapmine) |
| `AsteraLoader\AsteraLoader.dll` | `480e296a4d8152e20c28c0d83bec742d050f47ffb1f0894c52b4ebab4e97392e` | 0/75 |
| `stracker_compat.dll` | `75c14dca9677d030da250c7c41af8b6bb4b8a7912203d1c66a65913a1882944f` | 1/75 (Cynet) |
| `loader.dll` | `7a0ce70730c79a5ffe800e3c2c7db8f382d15cdfcedad295ae5f3515826272c2` | 0/75 |

Archive `AsteraLoader-0.2.0.zip`: SHA-256
`8b51f054c433b442404526af6aa3277e8c69dec167cd45a0084d350db70bbc29`. Plugin
kit `AsteraLoader-plugin-kit-0.2.0.zip`: SHA-256
`c78a6c5b53a24569ce403f148e6af550822cb00dead0202a94fe64e49738d4e5`.

## What is new

- **"al.patch" (API 1.8)**, for changes that are not a hook at a
  function's start: NOPs, a flipped jump, a constant, a pointer in a
  table, a call sent to the plugin. Astera Loader:
  - checks the bytes before it changes them, and finds them by address or
    by a pattern that must match exactly once;
  - changes only the game exe's code and data, never its headers, import
    table or other data directories, Steam's DRM stub or any other DLL;
  - keeps one owner per byte, shared with "al.hooks": a change over
    another plugin's change or hook is refused, and the plugin's log
    names the holder;
  - writes small aligned values in one atomic step, and everything else
    with the game's other threads paused, never while one is inside the
    changed code;
  - puts data changes back at shutdown, and lists the active changes and
    their owners in its logs and crash reports.
- **Crash reports** name the plugin whose change the crash happened in,
  and the next-launch notice suggests trying the game without it.
- CMFM, the in-game settings menu, uses al.patch for its "Mods" entry in
  the Start menu.

## Tested

With MHW 421810 under Mod Organizer 2, Steam overlay on, this release's
package installed over the previous build: CMFM changing its two Start
menu values through al.patch, the DLSS / Frame Generation mod in
development, and 17 Stracker plugins; every game hook in place, the game
exiting with code 0. Launch setups and stability were tested for 0.1.0;
the launcher is unchanged since. Outside the game, the test suite covers every refusal, the
ownership rules, writes while another thread runs the changed code, and a
full launch through the launcher.

## Known issues

As in 0.1.0:

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
