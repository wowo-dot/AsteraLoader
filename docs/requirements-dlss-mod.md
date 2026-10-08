# What the DLSS mod needs from CMFM

CMFM (Configuration Menu For Mods) is the user's planned framework. This file
lists what the DLSS / Frame Generation mod needs from it as a loader and
settings host, so both projects can be designed against the same contract.
Written 2026-10-08 by the DLSS project (`D:\mh world modding\dlssupdate`) after
the loader review ([loaders.md](research/loaders.md)) and its shipped-layout runs
game-50 to game-52. This is the canonical copy; the DLSS repository points here.

Levels: **MUST** means without it the DLSS mod cannot be a CMFM plugin.
**SHOULD** means we can work around it, but badly. **MAY** is nice to have.

Facts this builds on (all confirmed, exe sha c2ebbbd2…):
- The packed exe's only static import is KERNEL32. winmm, dinput8, dxgi,
  d3d12 and the rest are delay imports.
- Steam's GameOverlayRenderer64.dll statically imports WINMM and CFGMGR32.
  That is the only reason SharpPluginLoader's winmm.dll loads at process start
  without MO2.
- Under MO2, early static-import resolution finds System32 copies, not
  virtual-filesystem ones. SPL then needs MO2 force-load. Our winmm.dll still
  loads in time because we only need to beat device creation (game-52).
- Stracker's plugins load after Streamline and NGX (game-50 load order). That
  is too late for frame generation.

## 1. Load point

- **MUST** call the plugin's early entry before the game loads dxgi.dll and
  calls CreateDXGIFactory2 / D3D12CreateDevice (game call site RVA 0x23a4e2e).
  The mod hooks both to attach Streamline and NGX to the game's own device and
  swap chain.
- **MUST** run that entry on a normal thread outside the loader lock, so the
  plugin can call LoadLibrary and create threads.
- **MUST** work in all three launch setups, with no force-load and no Steam
  overlay dependency:
  - Steam launch, manual install;
  - MO2 launch;
  - Steam overlay disabled.
- **SHOULD** expose named phases a plugin can subscribe to:
  - `pre_main`: post-unpack, before CRT init; SPL's cookie trick reaches this;
  - `win_main`;
  - `device_created`;
  - `swapchain_created`;
  - `first_present`;
  - `game_ready`: title screen or hub.

  The mod needs `pre_main` or anything before dxgi. The rest replace hooks it
  installs today.
- **SHOULD** choose CMFM's own entry slot so that it coexists with SPL
  (winmm.dll) and Stracker's Loader (dinput8.dll). See open question 1.

## 2. Plugin contract

- **MUST** use a C ABI only: no C++ classes, STL types or exceptions across the
  boundary. Stracker's `LOG` puts a `std::stringstream` across it and ties
  every plugin to MSVC's release STL.
- **MUST** use a versioned API table, e.g. `struct cmfm_api { uint32 size;
  uint32 version; ... }`, with additive changes only within a major version.
  CMFM refuses a plugin built for a newer major version, with a readable reason.
- **MUST** have the plugin export `cmfm_plugin_info` (name, version, API
  version, supported exe hashes, wanted phases) and `cmfm_plugin_init(const
  cmfm_api*)`.
- **MUST** load in a deterministic, declared order (priority plus before/after
  names), not the alphabetical order of a folder listing (Stracker plugins
  rely on names like `!CRCBypass.dll`).
- **MUST** give each plugin its own folder (`<plugins>\mhw_dlss\`) and pass
  its path in at init. Under MO2, GetModuleFileNameW reports the virtual game
  path while canonical() gives the real mod folder. That mismatch disabled
  v0.13.8 (game-51). CMFM should hand plugins one consistent path. Having
  CMFM supply both the virtual path and the real path would remove the
  guesswork.
- **MUST NOT** change the process DLL search path or preload DLLs named
  `sl.*`, `nvngx_*` or `nvapi*`. The mod loads the NVIDIA runtime by full path
  and verifies SHA-256 pins (Streamline 2.12.0, DLSS, DLSS-G).
- **MUST** keep plugins loaded until process exit: no FreeLibrary at shutdown,
  no native hot-reload unless the plugin opts in. The mod deliberately retains
  Streamline, NGX and its hook trampolines for the process lifetime, because
  the game renders into Streamline's buffers.
- **SHOULD** let a plugin report its state (`active`, `disabled: <reason>`,
  `degraded: <reason>`). Every feature fails closed, and today the user only
  sees that in a log.
- **SHOULD** isolate load failures: a plugin that fails to load or init is
  logged and skipped, never fatal.

## 3. Graphics coordination (most important)

The mod owns presentation when frame generation is on. Streamline replaces
the game's swap chain with its proxy, paces Present, and needs tagged inputs
every frame.

- **MUST** let exactly one plugin be the presentation owner. That plugin
  substitutes or upgrades the factory, swap chain and queue objects the game
  receives. CMFM and other plugins then see the game-facing (proxy) objects.
- **MUST** have CMFM's menu or overlay draw into the game-facing back buffer
  before the owner's Present, on the game's queue, with correct resource
  barriers. Drawn there, DLSS-G treats it as UI: it is in the final colour but
  not in our HUD-less colour, so generated frames do not smear it.
- **MUST** release all back-buffer references before ResizeBuffers or swap
  chain recreation, and re-acquire after. Use pre-resize and post-resize
  events.
- **MUST** use one hook per D3D12/DXGI entry point with ordered callbacks
  (Present, ResizeBuffers, ExecuteCommandLists, Signal). Plugins must not each
  inline-patch vtables; SPL does today. The Discord stream crash went through
  stacked hooks (Discord's and ours).
- **SHOULD** expose the device, game queue, swap chain (proxy and native),
  back-buffer index, frame counter and present timing.
- **SHOULD** send menu open and close events, so the mod can pause frame
  generation or switch Reflex while the menu captures input.
- **MUST NOT** call NVAPI or Reflex sleep or markers, or run its own frame
  limiter that bypasses Reflex. The mod owns Reflex (PCL markers) and frame
  pacing. A CMFM limiter must defer to the presentation owner.

## 4. Settings menu (milestone 8)

- **MUST** take a typed settings schema from each plugin (enum, bool, int or
  float range, read-only status line), with get, set and validate callbacks.
  Each setting is marked `live` or `restart required`.
- **MUST** let the plugin keep its own config file as the source of truth.
  For us that is `mhw_dlss.ini`, which stays editable by hand. CMFM reads and
  writes through the plugin, so a broken CMFM store can never wipe it. SPL
  overwrites an unparsable loader-config.json with defaults; this rule avoids
  that.
- **MUST** handle input: keyboard, mouse and controller, a configurable
  hotkey, and blocking game input while the menu is open (the game uses
  DirectInput8 and XInput).
- **MUST** let a plugin show a warning popup or notification (text, severity,
  "don't show again"). Examples:
  - "Frame generation needs an RTX 40/50 GPU";
  - "3x needs RTX 50";
  - "the game's own DLSS/TAA option conflicts";
  - "disabled: reason".
- **SHOULD** support AA-control lockout: grey out or annotate the game's own
  Anti-Aliasing and DLSS options while the mod owns AA, or at least warn when
  they are changed. This item has been parked waiting for CMFM since
  2026-09-26.
- **SHOULD** offer a status page per plugin, e.g.:
  - mode;
  - render size → output size;
  - frame generation multiplier;
  - Reflex state;
  - the last failure reason.
- **MAY** let plugins add a debug page, e.g. live counters like `fg_status`
  and `dlss_summary`.

Settings the DLSS mod will register:

| Setting | Values |
|---|---|
| frame_generation | off, 2x, 3x, 4x, 6x (3x and up need RTX 50) |
| upscaling | DLAA, Quality, Balanced, Performance, Ultra Performance (beta work) |
| reflex | on, boost |
| hud_less | on/off (diagnostic) |
| status overlay | on/off |

## 5. Logging, crashes, diagnostics

- **MUST** provide a C logging call (level and message) into per-plugin files
  in one known folder, with size limits. Paths under %USERPROFILE% are redacted
  (beta backlog: log path privacy).
- **MUST** use one coordinated crash handler. It records:
  - the faulting module, offset and owning plugin;
  - the thread, and whether it is a game, CMFM or plugin thread.

  It chains to existing handlers and never swallows exceptions. Minidumps are
  opt-in, written to a known folder. Today the mod's crash.jsonl misses
  non-host faults, and we rely on WER LocalDumps.
- **SHOULD** offer a one-click "collect logs" zip for bug reports: CMFM logs,
  plugin logs, the config files and the exe hash, with no game files.
- **SHOULD** expose game identity: exe SHA-256, PE timestamp, the game version
  string (the same one Stracker's `GameVersion` reports), and DX11 or DX12
  mode. The mod is DX12-only and fails closed on anything else.

## 6. Compatibility

- **MUST** make Stracker's Loader unnecessary (user's decision, 2026-10-08:
  fewer loaders and proxy DLLs in the game folder). With CMFM installed,
  existing Stracker plugins work unchanged and neither `dinput8.dll` nor
  Stracker's `loader.dll` is needed.
  - CMFM ships a shim named `loader.dll` in its own folder. It exports the four
    symbols the 19 installed plugins use (LOG ctor/dtor, MinLogLevel,
    GameVersion) and an `Initialize` that does nothing on a second call.
  - CMFM loads the shim by full path before any plugin. A plugin's `loader.dll`
    import then binds to it: Windows uses an already-loaded module of the same
    name instead of searching (documented DLL search order). No file goes in
    the game root.
  - The shim:
    - loads `nativePC\plugins\*.dll` in alphabetical order;
    - honours loader-config.json keys and writes loader.log;
    - reports the identical GameVersion string;
    - is built with MSVC release STL.
  - Timing defaults to Stracker's moment: when the game first loads DINPUT8.
    Detect it with a DLL-load notification and defer the loads out of the
    loader lock. Early loading stays an option: SPL already preloads
    dinput8.dll before CRT init by default, so common plugins tolerate it.
  - Leftover Stracker files must be detected and reported:
    - both files present: Stracker loads its own loader.dll by relative path,
      so two modules named loader.dll exist. This should be harmless: both
      export the same symbols, and loading a plugin twice only adds a
      reference. Test it anyway;
    - dinput8.dll alone: Stracker shows a "loader.dll not found" error box.

    CMFM should tell the user to remove both.
  - Stracker's code is MIT; fork it with credit. Test against the 19 installed
    plugins, LuaEngine first.
- **MUST** coexist with SPL: don't take or rewrite its winmm.dll, and don't
  rewrite loader-config.json.
- **MUST** coexist with ReShade (dxgi.dll) and with overlays: Steam, Discord,
  RTSS, NVIDIA app.
- **MUST** be a normal MO2 mod with no force-load and no files outside its own
  folders. No exe patching. No disabling of integrity or anti-cheat. Online
  play stays possible.
- **SHOULD** detect a second copy of a plugin, e.g. the DLSS mod's standalone
  winmm.dll and its CMFM plugin together. The mod's single-instance mutex
  keeps only the first one active; CMFM should tell the user.
- **MAY** support Proton/Linux. DLSS frame generation is not a target there.

## 7. Performance, security, distribution

- **MUST** cost close to nothing per frame with the menu closed: no managed
  runtime, no per-frame allocation, no per-draw hooks. The goal is a few
  microseconds on the present thread, measured. MHW is CPU-bound (see
  cpuopt), and every microsecond on the render thread shows up.
- **MUST** be open source, with reproducible builds and published release
  hashes. No network access, auto-update or telemetry.
- **MUST** load DLLs only from its plugins folder. No arbitrary paths from
  config.
- **SHOULD** let a plugin pin its own dependencies by hash. The mod already
  does this for the NVIDIA runtime.

## What the DLSS mod commits to in return

- Ships as a CMFM plugin (`plugins\mhw_dlss\`) and keeps its standalone
  winmm.dll mode for users without CMFM. The same DLL works both ways; the
  mutex keeps one copy active.
- Registers its settings, status and warnings through CMFM. Keeps
  mhw_dlss.ini as the readable source of truth.
- Logs through CMFM's API.
- Keeps the presentation-owner role as narrow as possible: only what frame
  generation and Reflex need.

## Open questions for the user

Questions 2 to 5 were answered on 2026-10-08 (ImGui for v1 with native-style
integration later, per-plugin files, Stracker replacement in v1, MIT). See
[decisions.md](decisions.md). Question 1 waits for measurement.

1. **CMFM's entry slot.**
   - winmm.dll (collides with us and SPL unless CMFM chain-loads them);
   - a different delay import (XINPUT9_1_0, IPHLPAPI, MFPlat, MFReadWrite and
     WININET are the non-KnownDLL candidates). We'd measure their load order
     in one run;
   - CMFM *being* the shared winmm.dll host for both us and SPL.

   Pre-CRT loading without MO2 force-load is only reliable through Steam's
   overlay imports (winmm, cfgmgr32).
2. **Menu technology and look:** ImGui, or something native-styled to match
   MHW's UI?
3. **Settings storage:** per-plugin files (preferred above), or a central CMFM
   store with import and export?
4. **Stracker replacement:** decided that CMFM replaces it. Open: in CMFM v1
   (recommended: it is small and makes CMFM useful on day one), or later?
5. **Licence** for CMFM (repository: `D:\mh world modding\CMFM`).
