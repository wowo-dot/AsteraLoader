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
