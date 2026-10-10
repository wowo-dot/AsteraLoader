<p align="center"><img src="assets/logo/astera-loader.svg" width="128" alt="Astera Loader logo"></p>

# Astera Loader

A plugin loader for Monster Hunter: World on PC (DirectX 12). It replaces
Stracker's Loader: your Stracker plugins keep working, with no
`dinput8.dll` or `loader.dll` in the game folder. It works alongside
SharpPluginLoader. Plugins written for Astera Loader get a C API with load
phases, shared game and Direct3D 12 hooks, a per-frame tick, hotkeys and
input capture.

Astera Loader starts the game through its own launcher, `AsteraLoader.exe`,
and puts nothing into the game folder itself.

**Status:** version 0.x, in development. Tested with MHW 421810 (the final
version) under Mod Organizer 2. Nothing else to install: no Visual C++
runtime, no .NET.

## Windows Defender

**Windows Defender may remove `AsteraLoader.exe`.** It can report it as
`Behavior:Win32/DefenseEvasion.A!ml` and quarantine it, sometimes while it
runs. Add an exclusion for it (below).

Why it happens: the launcher starts the game paused, loads
`AsteraLoader.dll` into it, then lets it run. That is how launcher-style
loaders work (Skyrim's SKSE does the same). It is also something malware
does, so Defender's behaviour detection flags it. Astera Loader is open
source: you can read exactly what the launcher does (`src/launcher`) and
build it yourself. It needs no administrator rights, has no network
access, does not auto-update, and does not touch the game's files or its
protections.

To add the exclusion: Windows Security, Virus & threat protection, Manage
settings, Exclusions, "Add or remove exclusions", "Add an exclusion":

- **Mod Organizer 2:** choose *Folder* and pick the Astera Loader mod's
  folder (in MO2's `mods` folder).
- **Manual install:** choose *File* and pick `AsteraLoader.exe` in the game
  folder.

If Defender already removed it: Virus & threat protection, Protection
history, open the entry, Actions, Restore. Or install it again after
adding the exclusion.

**Other antivirus programs** may flag Astera Loader's files too, for the
same reasons: on VirusTotal a few engines report the launcher, the core
DLL or the Stracker interface (`AsteraLoader\plugins\stracker_compat\loader.dll`,
which Kaspersky flags) with generic or machine-learning verdicts. The fix
is the same: exclude the Astera Loader folder (or the mod's folder under
MO2) in that program. If `loader.dll` is removed, your Stracker plugins do
not load; Astera Loader itself keeps running and says why.

**SmartScreen:** the files are not signed yet, so starting
`AsteraLoader.exe` after unpacking it with Explorer may show "Windows
protected your PC". Choose "More info", then "Run anyway". Installs
through MO2 usually do not see this.

## Install

### Mod Organizer 2 (recommended)

1. Install the archive as a mod ("Install a new mod from an archive"). If
   MO2 says the archive has no data folder, install it as it is: for MHW
   the mod's root is the game folder.
2. Turn off Stracker's Loader (its mod), if you have it. Keep your Stracker
   plugins' mods.
3. Add an executable in MO2: binary `<MO2 mods>\Astera Loader\AsteraLoader.exe`,
   argument `"<game folder>\MonsterHunterWorld.exe"`, start in the game
   folder.
4. Start the game with that entry from now on. The plain "Monster Hunter:
   World" entry starts the game without Astera Loader.

### Manual (not yet tested)

1. Remove Stracker's Loader: delete `dinput8.dll` and `loader.dll` from the
   game folder. Keep `nativePC\plugins`.
2. Copy `AsteraLoader.exe` and the `AsteraLoader` folder into the game
   folder.
3. Start the game with `AsteraLoader.exe`.

### Steam launch options (not yet tested)

Set the game's launch options to `"<path>\AsteraLoader.exe" %command%`.
Without them, Steam's Play button starts the game without Astera Loader.

## Plugins

- **Astera Loader plugins:** `AsteraLoader\plugins\<name>\<name>.dll`.
- **Stracker plugins:** stay in `nativePC\plugins`, as before.
- **SharpPluginLoader:** works alongside, unchanged.

If Stracker's Loader files are still in the game folder (or its MO2 mod is
on), Astera Loader's Stracker support turns itself off and says why, so
plugins are not loaded twice.

When a plugin cannot start, Astera Loader shows a warning at start-up and
keeps the game running.

## Logs and crash reports

They are in the `AsteraLoader\logs` folder next to `AsteraLoader.exe`
(under MO2, inside the mod's folder):

- `core.<pid>.log`: Astera Loader's own log, one per game launch;
- `launcher.<pid>.log`: the launcher's log;
- `plugins\<name>.log`: each plugin's log;
- `crashes\`: if the game crashes, a report naming the plugin whose code
  faulted. You are told at the next launch.

For a bug report, send the core log, the launcher log and the crash report
from that launch. Astera Loader logs no key presses or typed text, and its
own logs show your Windows user folder as `%USERPROFILE%`.

## Uninstall

Turn off the mod in MO2, or delete `AsteraLoader.exe` and the
`AsteraLoader` folder. Astera Loader changes nothing else.

## For plugin authors

- `docs/plugin-guide.md`: how to write a plugin;
- `include/al/al_plugin.h`: the C API (the reference);
- `examples/hello/hello.c`: the smallest working plugin.

## Building

Windows, Visual Studio 2026 with C++ (x64), CMake 3.25 or newer:

```
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

The tests start a stand-in game through the launcher, so Defender may need
the exclusion for the build folder too.

## Licence

MIT (`LICENSE`). Third-party notices are in `licences`: MinHook (BSD-2),
Stracker's Loader (MIT, its log code BSD-2) and SharpPluginLoader (MIT).
