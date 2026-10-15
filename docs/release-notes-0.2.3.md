# Astera Loader 0.2.3

Astera Loader now runs under Proton on Linux. Nothing changes on Windows.
Plugin API 1.8 as in 0.2.0; plugins built for any earlier version keep
working.

Two downloads:

- `AsteraLoader-0.2.3.zip`: the loader. Install it as an MO2 mod over
  0.2.2, or as a new one (see the README).
- `AsteraLoader-plugin-kit-0.2.3.zip`: for plugin authors. The C header,
  the plugin guide and a small example plugin that builds with CMake.

## Before you install: Windows Defender

As with earlier releases, Windows Defender may remove `AsteraLoader.exe` as
`Behavior:Win32/DefenseEvasion.A!ml`, because the launcher starts the game
paused and loads its DLL into it. An exclusion for the Astera Loader mod
folder covers the new files too; the README shows how.

VirusTotal: to fill in.

## Linux (Proton)

Install it as for a manual install, into the game folder (see the
README). Steam on Linux runs launch options as a Linux command, so the
Windows line does not work there. These launch options start Astera Loader
in the game's place:

```
bash -c 'exec "${@/MonsterHunterWorld.exe/AsteraLoader.exe}"' -- %command%
```

Other mods may need more on that line; see their instructions.

## Fixed

Three problems under Proton, one after the other:

- **The game started without Astera Loader** ("Astera Loader did not
  confirm startup"). The launcher loaded its DLL into the game but did not
  find it there. It compared two names for the DLL that Windows writes the
  same way and Wine does not. It now compares names from the same source
  and, failing that, recognises the DLL by its contents.
- **The game closed right away.** Astera Loader listed the game's threads
  with Windows functions that Proton does not have. It now uses them only
  where they exist, and otherwise a slower listing that works everywhere.
  Windows keeps the fast one.
- **The game closed after the plugins loaded.** Astera Loader notices the
  game reaching its main function through a hardware breakpoint, and under
  Proton it did not recognise its own breakpoint firing. It now does.

For bug reports, the logs show more:
- The launcher log says how it found the DLL in the game
  (`core_found_by=`).
- The core log has a line for each of Astera Loader's start-up breakpoints
  (`phase_win_main_hit`, `phase_device_return_hit`).

## Tested

With MHW 421810:

- **Linux, Proton** (one tester): Astera Loader started with the game, all
  its phases ran through to Exit Game, and the Direct3D 12 services
  installed. CMFM's menu worked, and the DLSS mod loaded.
- **Windows, under Mod Organizer 2,** installed from this archive alongside
  CMFM 0.1.0, the DLSS mod 0.28.5, a CMFM page for Fexty's Mod and 10
  Stracker plugins: every phase through Exit Game, all plugins active,
  exit code 0.
- **Windows, started from a shortcut,** not through Steam or Mod Organizer
  2: the same, exit code 0.

Outside the game, the test suite covers:
- the DLL found by name, found by its contents, and not loaded;
- the thread listing without its fast functions;
- the start-up breakpoints, with and without debug registers in the
  exception, as Windows and Proton report them.

## Known issues

As in 0.2.2, and under Proton:

- In the one test so far, the game crashed while exiting, in its own code
  with no plugin involved. Whether Proton, the game or a mod causes it is
  not known yet.
- Only one Linux setup has been tried.

## Licence

MIT. Third-party notices (MinHook, Stracker's Loader, SharpPluginLoader) are
in the `licences` folder.
