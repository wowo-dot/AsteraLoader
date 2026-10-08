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
