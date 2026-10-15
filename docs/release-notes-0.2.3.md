# Astera Loader 0.2.3

Draft, not yet tested in game. A fix for Proton on Linux, where the game
started without Astera Loader. Plugin API 1.8 as in 0.2.0; plugins built
for any earlier version keep working.

Two downloads:

- `AsteraLoader-0.2.3.zip`: the loader. Install it as an MO2 mod over
  0.2.2, or as a new one (see the README).
- `AsteraLoader-plugin-kit-0.2.3.zip`: for plugin authors. The C header,
  the plugin guide and a small example plugin that builds with CMake.

## Fixed

- **Proton (Linux).** The launcher loaded `AsteraLoader.dll` into the
  game but did not find it there, so the game started without Astera
  Loader ("Astera Loader did not confirm startup"). The launcher compared
  two names for the DLL that Windows writes the same way and Wine does
  not. It now compares names from the same source and, failing that,
  recognises the DLL by its contents. Windows is unaffected.
- **Proton: the game closed at once.** With the DLL found, the game ended
  as Astera Loader set up its first hook. Astera Loader used Windows
  functions for listing the game's threads that Proton does not have. It
  now uses them only where they exist, and otherwise the slower listing
  that works everywhere. Windows keeps the fast one.
- **Proton: the game closed after the plugins loaded.** Astera Loader
  notices the game reaching its main function through a hardware
  breakpoint, and under Proton it did not recognise its own breakpoint
  firing. It now does, and the core log has a line for each such hit.
- The launcher log says how it found the DLL in the game
  (`core_found_by=name` or `image`) and, if it did not, what it looked at.

## Proton launch options

Steam on Linux runs launch options as a Linux command, so the Windows
line does not work there. This one starts Astera Loader in the game's
place:

```
bash -c 'exec "${@/MonsterHunterWorld.exe/AsteraLoader.exe}"' -- %command%
```

## Tested

To do: Windows (under Mod Organizer 2, started directly, Steam launch
options) and Proton.

Outside the game, the test suite covers the DLL found by name, found by
its contents when no name matches, and not loaded at all.

## Known issues

As in 0.2.2. Under Proton, Astera Loader's Direct3D 12 and input
services have not been tested yet.

## Licence

MIT. Third-party notices (MinHook, Stracker's Loader, SharpPluginLoader) are
in the `licences` folder.
