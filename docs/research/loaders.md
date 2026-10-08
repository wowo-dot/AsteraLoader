# MHW loaders, load order and MO2 behaviour (2026-10-08)

Carried over from the DLSS project's loader review
(`D:\mh world modding\dlssupdate\docs\research\mod-loaders.md`) and its game-50
to game-52 runs. "Confirmed" means read from source, PE tables or a live
process. Anything else is labelled as a hypothesis.

Executable: SHA-256 `c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea`,
PE timestamp `0x66D13CB6`. This is the final Steam build; it is SteamStub-packed.

## Game imports (confirmed, PE directories of the packed exe)

- Static: KERNEL32 only.
- Delay: USER32, SHELL32, ole32, OLEAUT32, IMM32, DINPUT8, XINPUT9_1_0, d3d11,
  d3d12, WS2_32, IPHLPAPI, steam_api64, WINMM, amd_ags_x64, MFPlat,
  MFReadWrite, WININET, SETUPAPI, GDI32, ADVAPI32.
- Not KnownDLLs, so a game-folder copy wins the search: DINPUT8, XINPUT9_1_0,
  d3d11, d3d12, IPHLPAPI, WINMM, MFPlat, MFReadWrite, WININET.
- Steam's GameOverlayRenderer64.dll statically imports two non-KnownDLLs:
  WINMM and CFGMGR32. When Steam injects the overlay at process start, a
  game-folder winmm.dll loads at process start too. Without the overlay, winmm
  loads at the game's first winmm call.

## Load order seen in game (confirmed, PEB module list, read-only)

- game-50, the DLSS mod MO2-force-loaded, Stracker enabled:
  - the DLSS DLL at position 24, then d3d12 (27), dxgi (36), WINMM (45) and
    gameoverlayrenderer64 (56);
  - then Streamline sl.interposer (91), NGX and DLSS-G (96–109);
  - Stracker's dinput8.dll only at 116, then its loader.dll (118) and the
    plugins.

  **Stracker plugins load after the graphics device and frame generation are
  set up.**
- game-51, the DLSS mod as winmm.dll in an MO2 mod, no force-load:
  - System32 WINMM.dll at 31, the MO2 winmm.dll at 43, d3d12 at 45,
    gameoverlayrenderer64 at 53;
  - so under MO2 the early winmm load resolves to System32, and the virtual
    copy loads later through the game's own delay-load.
- game-52, same layout: the DLSS mod's D3D12CreateDevice hook caught the
  game's own call at RVA 0x23a4e2e. A game-folder winmm.dll arrives before
  device creation even under MO2.
- User report: SPL under MO2 "acts weird" unless MO2 force-loads its
  winmm.dll. This fits the game-51 order: SPL must load before CRT init, and
  the virtual copy arrives too late for that.

## MO2 / usvfs path behaviour (confirmed, game-51)

The same DLL in an MO2 mod reports two different paths:
- `GetModuleFileNameW` gives the virtual path: `<game>\mhw_dlss\...`;
- `std::filesystem::weakly_canonical` (GetFinalPathNameByHandle) gives the
  real path: `D:\MO2\MHW\mods\<mod>\mhw_dlss`.

Comparing one against the other disabled the DLSS mod v0.13.8. Use file
identity (`std::filesystem::equivalent`) or one derivation consistently.

## Stracker's Loader (confirmed, source + installed binaries)

- Source: https://github.com/Strackeror/MHW-QuestLoader at 9770302
  (2024-06-06), MIT, © 2020 Rémi Lavergne. `MHWLoader/log.h` derives from
  BSD-2 code (© 2018 Philipp Krähenbühl).
- The installed `D:\MO2\MHW\mods\Stracker's Loader` matches the source:
  loader.dll was built 2024-06-06 12:48 UTC, 19 minutes before the last
  commit, and its strings match loaderdll.cpp.
- `dinput8.dll` (DllLoader/):
  - DllMain loads System32 dinput8 and forwards DirectInput8Create;
  - then `LoadLibraryA("./loader.dll")` and calls `Initialize`;
  - if loader.dll is missing, it shows a "loader.dll not found" message box.
- `loader.dll` `Initialize`:
  - `LoadConfig()`: loader-config.json in the working directory;
  - `OldWarning()`: warns about hid.dll / dtdata.dll;
  - `FindVersion()`: signature scan for the game version string;
  - `LoadAllPluginDlls()`: every `*.dll` directly in `nativePC\plugins`, in
    `directory_iterator` order (alphabetical on NTFS), via LoadLibraryA. It is
    skipped when `enablePluginLoader` is false.
- Config keys: `logfile` (writes `loader.log`), `logcmd` (console titled
  "Stracker's Loader"), `logLevel` (DEBUG, INFO, WARNING, ERROR), `outputEveryPath`,
  `enablePluginLoader`.
- **ABI the installed plugins actually import** (19 plugins scanned):

  | Symbol | Used by |
  |---|---|
  | `??0LOG@loader@@QEAA@W4LogLevel@1@@Z` (LOG ctor) | 19 |
  | `??1LOG@loader@@QEAA@XZ` (LOG dtor) | 19 |
  | `?MinLogLevel@loader@@3W4LogLevel@1@A` | 19 |
  | `?GameVersion@loader@@3PEBDEB` | 3 |

  `LOG` is `{ std::stringstream stream; LogLevel logLevel; }`. The ctor and
  dtor live in loader.dll, while `operator<<` is an inline template compiled
  into each plugin. The plugin's STL code therefore writes into a stream
  constructed by loader.dll, so the layout must match: MSVC, release STL
  (`_ITERATOR_DEBUG_LEVEL 0`). The dtor appends a newline and passes
  `stream.str()` to `_log`, which applies the level filter and the config,
  then writes a time-stamped line to loader.log and the console.
- QuestLoader and MonsterLoader are ordinary plugins shipped with Stracker
  (same repository, MIT).

## SharpPluginLoader (confirmed, source)

- Source: https://github.com/Fexty12573/SharpPluginLoader at 7be2393
  (2026-10-08, active), MIT, © 2023 Fexty. Version 0.0.8.
- Ships its native DLL as `winmm.dll` (Windows) or `ucrtbase.dll` (Linux).
  The exports forward to System32. It only activates inside
  MonsterHunterWorld.exe.
- Preloader, from DllMain:
  - resets the exe's load-config security cookie to the MSVC default;
  - inline-hooks `GetSystemTimeAsFileTime`;
  - on the post-unpack `__security_init_cookie` call (return address inside
    the exe, default cookie constant nearby), hooks `__scrt_common_main_seh`,
    WinMain and the sMhMain constructor.
- `loader-config.json` → `"SPL": { "PreloadDLLs": [...] }`:
  - loaded at that post-unpack, pre-CRT point, in list order, by
    `LoadLibraryA` on paths relative to the game folder;
  - missing files are skipped silently;
  - **default `["dinput8.dll", "dxgi.dll"]`**, so with SPL installed,
    Stracker's loader and its plugins load before CRT init.
- SPL shares loader-config.json with Stracker (adds an `SPL` object) and
  rewrites it every launch. **An unparsable file is replaced with defaults**,
  including Stracker's keys.
- Hosts .NET 8 (hostfxr); users need the .NET 8 Desktop Runtime. C# plugins
  live in `nativePC\plugins\CSharp`, with hot reload.
- Native components: `<Plugin>.Native.dll` beside a C# plugin, exporting
  `get_internal_call_count` and `collect_internal_calls`. These always sit
  behind a C# plugin; there is no standalone native plugin API.
- D3D12 overlay (ImGui and primitives):
  - a mid-hook on the sMhRender assignment gets the swap chain at
    renderer+0x1470 and the queue at renderer+0x20;
  - inline hooks: swap chain Present (vtable 8), ResizeBuffers (13), queue
    Signal (14);
  - ImGui is drawn on the back buffer inside Present via ExecuteCommandLists.

  With frame generation on, that swap chain is presumably Streamline's proxy.
  Hypothesis, untested.
- Menu key F9.

## Implications for CMFM

Superseded for the entry slot: CMFM uses its own launcher (decisions.md item
6, research/entry-slot.md). The analysis below is kept for the record.

- Entry slot:
  - winmm.dll is taken by SPL and by the DLSS mod's standalone mode;
  - dinput8.dll is Stracker's, and loads too late anyway;
  - other delay imports (XINPUT9_1_0, IPHLPAPI, MFPlat, MFReadWrite,
    WININET) are unmeasured.

  Pre-CRT loading without force-load only happens through the Steam overlay's
  static imports (winmm, cfgmgr32). Pre-device loading works through any delay
  import the game calls early enough. Measure before choosing (handoff, open
  question 1).
- The Stracker replacement needs no game-root files. Load a shim named
  `loader.dll` by full path before the plugins: Windows binds a plugin's
  `loader.dll` import to the already-loaded module of that name (documented
  DLL search order).
