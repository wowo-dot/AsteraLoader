# CMFM plugins in the real game (2026-10-09)

Measured facts from game runs with CMFM plugins installed.

## Run plugin-mo2-1 (confirmed)

The `hello` example plugin (`examples/hello`, 5fdee0a) as its own MO2 mod,
`cmfm/plugins/hello/hello.dll`. MO2 profile `cmfm`, "CMFM Launcher", no
force-load, Steam overlay on, core d589db2 (v0.1.0-dev.3), launcher 6591057.

- Discovery listed `<game>\cmfm\plugins` through MO2's virtual folder and
  found `hello` in a separate mod. The core reported
  `plugin name=hello version=0.1.0 status=active` before and after dispatch.
  No warning popup; the game exited normally (code 0).
- hello's init and its PRE_MAIN call ran on the PRE_MAIN thread (the game's
  startup thread), 861.5 ms after process start.
- `hello.dll` loaded at 860.7 ms, before the DLSS mod's winmm.dll (878.9 ms),
  dxgi.dll (964.1 ms) and device creation (1322.6 ms). PRE_MAIN is early
  enough for a plugin that hooks CreateDXGIFactory2 / D3D12CreateDevice.
- Paths seen by the plugin:
  - `init->folder`: `<game>\cmfm\plugins\hello` (virtual);
  - `init->folder_real`: `D:\MO2\MHW\mods\CMFM hello plugin (test)\cmfm\plugins\hello`;
  - `GetModuleFileNameW` on its own DLL: the **virtual** path, although the
    PEB module list records the real path CMFM loaded it from. usvfs
    rewrites the answer (the effect that broke the DLSS mod in game-51).
    Plugins should use the two init paths, not derive their own.
- Game paused 85.4 ms at launch; the launcher creates the game 1.5 ms after
  it starts.

Run folder: `local/runs/plugin-mo2-1` (CMFM logs, hello.log, module report).

## Run stracker-mo2-1 (confirmed)

Stracker's Loader compatibility (`stracker_compat` with its `loader.dll`
shim, 6e0dc18). MO2 profile `cmfm` with the `Stracker's Loader` mod
**disabled** and a test mod holding byte-identical copies of its
loader-config.json (logfile and logcmd on, logLevel ERROR), MonsterLoader.dll
and QuestLoader.dll. "CMFM Launcher", no force-load, overlay on.

- `stracker_compat` reported active: 2 Stracker plugins found, 2 loaded,
  0 failed; game version 421810; config found. No CMFM warning, no Stracker
  console, no loader.log (nothing at ERROR). The game exited normally.
- **The game version is readable at PRE_MAIN**, before the game's CRT
  start: Stracker's pattern matched once and its pointer held "421810".
- Load order: hello 880.3 ms, stracker_compat 881.3, CMFM's loader.dll 882.8
  (from the CMFM mod), MonsterLoader 895.2, QuestLoader 1044.3. DINPUT8 came
  from System32 at 5011 ms; no Stracker dinput8.dll or loader.dll loaded.
- PRE_MAIN took 626 ms. The shim's own work (config, version scan) is about
  12 ms; the rest is the plugins' DllMain signature scans (MonsterLoader about
  149 ms, QuestLoader about 462 ms). Under Stracker's Loader that work ran
  after DINPUT8 at about 4.4 s. It moved earlier, so the DLSS winmm.dll, dxgi
  and device creation came about 640 ms later than in plugin-mo2-1.
- The module list records the Stracker plugins by their real mod path (the
  shim loads them by full virtual path); Stracker's relative-path loads were
  recorded by the virtual path. `GetModuleFileNameW` gives the virtual path
  in both cases under usvfs.

Run folder: `local/runs/stracker-mo2-1`. Not yet tested: leftover Stracker
files, the full 19-plugin set (LuaEngine first), SPL force-load, overlay off,
manual and Steam launch options.

## Runs stracker-mo2-2 and stracker-mo2-3: leftover Stracker files (confirmed)

The same setup with the `Stracker's Loader` mod enabled again, so its
dinput8.dll and loader.dll are in the virtual game folder.

- `stracker_compat` reports **degraded**: "Stracker's Loader files are still
  in the game folder (dinput8.dll, loader.dll); CMFM replaces them, so disable
  or remove them". CMFM shows its warning popup. The plugins still load once
  at PRE_MAIN through CMFM's shim; the game runs and exits normally.
- Stracker's own dinput8.dll (4963 ms) and loader.dll (4966 ms, a second
  module named loader.dll) still run. Its requests for the same plugins get
  the modules already loaded: **no plugin was mapped twice** (stracker-mo2-3,
  watcher). stracker-mo2-2 had no watcher.
- Correction: after stracker-mo2-2 the shim was changed (3ce2d5d) to load
  plugins by Stracker's relative path, on the theory that a relative load is
  recorded by the virtual path and a full-path load is not. stracker-mo2-3
  disproved it: the plugins are recorded by their real mod path either way,
  and usvfs resolves Stracker's later requests to those same files. The
  relative call stays because it is Stracker's own; the entry-slot-1
  observation (Stracker's loads recorded by the virtual path) remains
  unexplained.

Run folders: `local/runs/stracker-mo2-2`, `local/runs/stracker-mo2-3`.

## Run spl-mo2-1: SharpPluginLoader force-loaded (confirmed)

SPL 2.0.0 from its official GitHub release, as an MO2 mod. Its `winmm.dll`
is force-loaded by MO2 for the "CMFM Launcher" entry, in the user's profile
`cmfm-spl`, which is `cmfm` plus SPL, minus the DLSS winmm mod (both ship a
`winmm.dll`). Steam overlay on, no C# plugins, two launches.

- CMFM is unchanged by SPL. The startup check came from the cache (93 and
  84 µs), and PRE_MAIN ran. hello and stracker_compat were active, the
  Stracker plugins loaded 2 of 2, and both launches exited with code 0.
- Load order:
  - `cmfm.dll` loads first;
  - SPL's `winmm.dll` follows 10 ms later, from its mod folder;
  - CMFM's PRE_MAIN plugins come next;
  - SPL's .NET host (hostfxr, coreclr, the Bootstrapper) starts right after
    PRE_MAIN returns, at its own post-unpack point.
- SPL found `__scrt_common_main_seh` at 0x1427428D4. That is the common-main
  function CMFM's startup check verifies, so CMFM restoring its startup hook
  before handing over leaves SPL's hook points intact.
- SPL's D3D12 setup succeeded, and its F9 menu opened (user). Its log has no
  errors.
- SPL rewrites `loader-config.json` in place. Stracker's keys survive, and
  SPL adds an `SPL` object (PreloadDLLs `dinput8.dll`, `dxgi.dll`; neither
  exists here). stracker_compat still reads the file.
- SPL writes into MO2's overwrite folder: `FASMX64.dll` and `imgui.ini` at
  the game root, and its caches, log and `cimgui.dll` under
  `nativePC\plugins\CSharp\Loader`.
- SPL's own startup cost is that the graphics device came up 0.35-0.9 s
  later than without it. The first launch also built SPL's caches.

Run folder: `local/runs/spl-mo2-1`.

Not yet tested:
- SPL with Stracker's Loader still enabled (SPL would preload its
  `dinput8.dll` right after PRE_MAIN);
- SPL with C# plugins;
- overlay off;
- manual and Steam launch options;
- the full 19-plugin set.

## Run stracker-all-1: every Stracker plugin the user uses (confirmed)

Profile `cmfm-stracker`: `cmfm` plus 15 plugin mods with 22 plugin DLLs.
These are the user's everyday set from `Default`, including G8F Actor
Routing 0.4, plus the handoff's test targets: LuaEngine, Better Input
Detection, MoreAutoShoutout_v and Tic Rate Fix. Stracker's Loader and SPL
are off, `!CRCBypass.dll` is excluded, and there is no force-load. Two
launches, the second into the save.

- Both launches: 22 found, 22 loaded, 0 failed, exit code 0. Each plugin
  was mapped once, from its own mod folder, at PRE_MAIN. Stracker's
  `dinput8.dll` never loads; the game loads System32's at 8.8 s.
- LuaEngine works. It initialised in both launches, and in the second it
  logged a scene switch when the save loaded.
- PRE_MAIN takes 3.9-4.4 s. That is the plugins' own DllMain work, run in
  order under the loader lock. The slowest are:
  - MatchMakingUnlocker, about 0.85 s;
  - BetterDailyCatSkills, 0.6 s;
  - QuestLoader and NoHomeChange, 0.5 s each;
  - G8F_AnimationRouting, 0.5 s, falling to 7 ms on the next launch.

  The graphics device came up at 5.6 s, against about 1.7 s with two
  plugins. Stracker's Loader does the same work later, when the game loads
  `dinput8.dll`. A baseline run should show whether the time to the title
  screen differs.
- Messages that are not CMFM's:
  - NameColorForce reports that its config file is missing. The mod ships
    none.
  - LuaEngine's `Engine.lua` checks for Stracker's `loader.dll` and
    `dinput8.dll` in the game folder and reports "file not found". The
    check is informational, and these files are absent under CMFM on
    purpose. The `?` characters come from LuaEngine itself; its own log
    shows them too.
- The "Stracker's Loader" console window appears because the config has
  `"logcmd": true`. The shim reproduces Stracker here: the console opens
  with the first line that passes `logLevel`. Earlier runs logged nothing
  at ERROR, so they had no console.

Run folder: `local/runs/stracker-all-1`.

Not yet run:
- the same plugins under Stracker's Loader, as a baseline for start-up time;
- the mods that are off in every profile.

## Run stracker-baseline-1: the same plugins under Stracker's Loader (confirmed)

Profile `stracker-baseline`: `cmfm-stracker` with Stracker's Loader in place
of CMFM, started from the plain "Monster Hunter: World" entry. Two launches.

- Both launches crashed the same way, before the Capcom logo, about 19 s
  after start. Windows recorded 0xC0000005 in `nvwgf2umx.dll`, NVIDIA's
  D3D12 driver, at offset 0x797c17 both times.
  - The crash came shortly after LuaEngine's scripts started. LuaEngine
    wrote its own crash record of its environment check.
  - Nothing names the culprit. The likely mix is the plugins the user does
    not normally run (LuaEngine above all), loaded after the graphics
    device exists, possibly with the DLSS frame-generation mod.
  - The user does not use LuaEngine, so this is not bisected.
  - Under CMFM the same plugins did not crash in two launches (stracker-all-1).
- Start-up time: under Stracker's Loader the plugins load from 4.4-4.9 s,
  after the graphics device, and take 6.5-7 s, against 4.2-4.4 s at CMFM's
  PRE_MAIN.
- The game's input, audio and last-module markers came 3.0-3.4 s later than
  under CMFM. The baseline crashed before the title screen, so the time to
  title itself was not compared; up to these markers, CMFM is ahead even
  though its window appears later.

Run folder: `local/runs/stracker-baseline-1`.

## Runs stracker-baseline-2 and stracker-all-2: like for like without LuaEngine (confirmed)

The user uninstalled LuaEngine, leaving 19 plugin DLLs in both profiles.
There was one launch each into the save, and both exited with code 0.

- Without LuaEngine, Stracker's Loader no longer crashes, so the
  stracker-baseline-1 crash needed LuaEngine.
- Through CMFM, all 19 plugins loaded, none failed, and PRE_MAIN took 3.8 s.
- Start-up markers in ms after process creation, CMFM first, then Stracker's
  Loader:

  | Marker | CMFM | Stracker's Loader | Difference |
  |---|---|---|---|
  | XINPUT9_1_0 | 8020 | 11088 | +3.07 s |
  | XAudio2_9 | 9162 | 12177 | +3.02 s |
  | WININET | 9217 | 12243 | +3.03 s |
  | d3d11on12 (last module) | 9548 | 12565 | +3.02 s |

  With the same plugins, the game reaches these points about 3 s sooner
  under CMFM. Most of the gain is the plugins' own loading: 3.6 s at
  PRE_MAIN, against 6.2 s after the device under Stracker's Loader. That is
  one launch each.

Run folders: `local/runs/stracker-baseline-2`, `local/runs/stracker-all-2`.
