# Plugin API and loader handoff (2026-10-08)

Steps 1 and 2 of the plan are done: the public C plugin header and the plugin
loader. Nothing calls the loader inside the game yet. Step 3 (the first
in-game phase) wires it in.

## What exists

- `include/cmfm/cmfm_plugin.h`: the public C header, API 1.0. Plugins export
  `cmfm_plugin_info` and `cmfm_plugin_init`. The header documents the folder
  layout, start-up order, versioning and lifetime rules.
- `tests/abi_layout.h`: every size and offset pinned, compiled as C and C++.
- `src/core/plugins/`:
  - `manifest`: reads a plugin's info, or gives the reason it cannot run;
  - `load_order`: the deterministic order;
  - `discovery`: lists `<root>\<name>\<name>.dll`;
  - `host`: `Host::load_all`, `Host::dispatch`, `Host::report`, the API
    table, per-plugin log files.
- Tests: `plugin_abi_c`, `plugin_abi_cpp`, `plugin_manifest_order` (unit) and
  `plugin_host` (11 fixture plugins covering every refusal, order, phases,
  logs and unloading).

## What step 3 needs to call

At the first in-game phase, on that phase's thread (outside the loader lock):

1. `Host::load_all` with:
   - `plugins_root`: `<exe folder>\cmfm\plugins`, built from
     `GetModuleFileNameW(nullptr)` (the game-folder path, virtual under MO2;
     MO2's usvfs is active by then, so plugin mods are visible);
   - `logs_folder`: the core's log folder (`cmfm\logs` next to `cmfm.dll`);
   - `exe_sha256`: the running exe's hash. The core's background thread only
     logs it today; it needs to store the result and let `load_all` wait for
     it. If it is empty, plugins that list exe hashes are refused (fail
     closed).
2. Log every `Host::report()` line to the core log: name, version, status,
   reason (AGENTS.md: run logs identify every loaded plugin with its version).
3. `Host::dispatch(CMFM_PHASE_PRE_MAIN, nullptr)`.

`cmfm.dll` does not link `cmfm_plugins` yet. Everything the host uses is in
KERNEL32, so the core's import table should stay KERNEL32-only; check it
when linking.

## Choices made (say if you disagree)

- Layout `cmfm\plugins\<name>\<name>.dll`; only that DLL is loaded, the rest
  of the folder belongs to the plugin. Names are lower-case `a-z 0-9 _ - .`,
  up to 64, and must match the folder.
- Plugin DLLs are loaded by their real path (resolved through a file handle)
  with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`,
  so a plugin's own DLLs are found in its folder and the process search path
  is untouched. `init` gets both the game-folder path and the real path.
- `order`: lower loads earlier, default 0. Then `load_after` / `load_before`
  override it; names not present are ignored.
- A load-order cycle refuses every plugin in it, with a reason; the others
  still load.
- A plugin built for a newer minor API version is refused, as well as other
  majors.
- Plugins are loaded first, then initialised in order. A refused plugin is
  unloaded; one whose init was called is never unloaded, even if it failed.
- Plugin logs: `cmfm\logs\plugins\<name>.log`, previous run kept as
  `<name>.previous.log`, 8 MiB cap.

## Not done yet

- Status page and popups (CMFM Menu) for the report; today it is data only.
- Isolating a crash inside a plugin's info or init call.
- Reserved names: a plugin named like a module the game or Stracker plugins
  rely on (`loader`, `dinput8`, `winmm`...) would share that DLL base name.
- Shipping the header as an SDK package.
