# CMFM: Configuration Menu For Mods (Monster Hunter: World)

## Objective

CMFM is an open-source native framework for the Windows PC version of Monster
Hunter: World (DirectX 12). It has two parts under one name:

- **CMFM Core:** our own launcher and native loader (the SKSE route).
  - `cmfm_launcher.exe` starts the game suspended, loads `cmfm.dll` into it,
    then resumes it. CMFM puts no proxy DLL in the game folder.
  - `cmfm.dll` is in before Steam's DRM stub and before any game code, in
    every launch setup, and dispatches load phases to plugins.
  - Exposes a versioned C plugin API with load phases.
  - Coordinates Direct3D 12 / DXGI hooks between mods.
  - Makes Stracker's Loader unnecessary: existing Stracker plugins run
    unchanged with no `dinput8.dll` or `loader.dll` in the game folder.
- **CMFM Menu:** the in-game settings menu. Plugins register typed settings,
  status pages and warning popups.

The user's guiding principle: fewer loaders, proxy DLLs and bandaids over the
exe. CMFM replaces Stracker's Loader. It coexists with SharpPluginLoader (SPL)
and does not compete with it: CMFM never takes `winmm.dll`, `dinput8.dll`,
`dxgi.dll` or any other proxy name.

v1 is launcher-only. A proxy fallback for Steam's plain Play button is added
only if Steam launch options (`"<path>\cmfm_launcher.exe" %command%`) turn out
not to work.

The first plugin client is the sibling DLSS / Frame Generation mod. Its
requirements are in `docs/requirements-dlss-mod.md`.

Start with `docs/handoff-20261008.md`, then `docs/decisions.md` (which
supersedes the handoff where they differ) and `docs/research/entry-slot.md`.

## Canonical Local Paths

- Repository: `D:\mh world modding\CMFM`
- Sibling DLSS mod (read-only reference from here): `D:\mh world modding\dlssupdate`
- Sibling CPU project (read-only reference): `D:\mh world modding\cpuopt`
- Game directory: `D:\SteamLibrary\steamapps\common\Monster Hunter World`
- Game executable: `D:\SteamLibrary\steamapps\common\Monster Hunter World\MonsterHunterWorld.exe`
- Mod Organizer 2 (portable): `C:\Modding\MO2\ModOrganizer.exe`, data directory `D:\MO2\MHW`.
  Profiles: `Default`, `dlss`, `barebonesdlss`. Stracker's Loader is the MO2 mod
  `Stracker's Loader`.
- IDA: `C:\Program Files\IDA Professional 9.3\ida.exe`
- Python 3.10: `C:\Users\akifs\AppData\Local\Programs\Python\Python310\python.exe` (run with `-B`)
- CMake/CTest (Visual Studio 18 2026, x64):
  `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`

These paths are machine-local. Do not encode them into distributable binaries
or assume other contributors use them.

## Safety and Scope

- Treat the game directory as read-only. Install test builds as MO2 mods,
  never into the game directory.
- Never patch `MonsterHunterWorld.exe` or game assets on disk. In-memory hooks
  only at signature-verified sites, with clean removal.
- Do not disable, bypass or weaken integrity checks, CRC/anti-tamper code, DRM,
  anti-cheat, licensing or platform protections. CMFM Core itself must not
  change gameplay or network behaviour. What plugins do is the user's choice.
  (The CRC-bypass plugin `!CRCBypass.dll` from "Performance Booster and Plugin
  Extender", and the crit-chance plugins GRcrit and tcscrit!, were unused and
  are removed from the user's MO2 setup (2026-10-09). They are not a CMFM
  concern.)
- The launcher is the only CMFM component that writes into the game process,
  and only to load `cmfm.dll`:
  - it only creates `MonsterHunterWorld.exe` itself, suspended. It never
    attaches to a running game or touches any other process;
  - it loads `cmfm.dll` by full path from CMFM's own folder, and changes
    nothing else in the process;
  - it leaves Steam's DRM stub (SteamStub) and its checks to run untouched.
    `cmfm.dll` and plugins install hooks only after the game's code is
    unpacked;
  - it needs no administrator rights, and has no network access or
    auto-update.
- Never touch `D:\mh world modding\ai\glavenus` IDA databases (another
  project). Work on private copies under `local/` only.
- Do not commit or redistribute Capcom binaries, decoded images, game assets,
  NVIDIA binaries, other mods' binaries, crash dumps or IDA databases.
- Never send project data, logs or binaries to external services.
- Do not reverse engineer NVIDIA binaries. Third-party mod DLLs (Stracker's,
  SPL, plugins) are studied from their published source where it exists.
  Only inspect a binary's import and export tables when no source exists.

## Runtime Testing Protocol

- The user launches the game, always through MO2: the CMFM launcher entry
  for CMFM runs, the plain "Monster Hunter: World" entry for baselines. Never
  launch MHW or the launcher through Steam, a script or computer-use. Steam
  launch-option tests are run by the user.
- Before every launch, prepare a run folder (`local/runs/<name>`) and the
  installed build. After the game closes, seal the folder with a short result
  file.
- Keep the installed test build in place between runs; announce any
  intentional stop or removal.
- Run logs identify:
  - the exe SHA-256 and the CMFM DLL SHA-256 and version;
  - the MO2 profile, with or without force-load;
  - every loaded plugin with its version.
- Measurement tools reading the game process (`tools/modwatch.py` and the
  like) are strictly read-only:
  - access `PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ`;
  - no writes, no thread suspension, no remote threads or allocations.

  The PEB module list read this way shows load order, load times and the
  parent module of each load. The launcher is not a measurement tool; see
  Safety and Scope.
- When screenshotting the desktop, crop to the game window.

## Engineering Rules

- C++20 and CMake; 64-bit Windows only.
- The plugin boundary is a C ABI: plain structs, function pointers, explicit
  sizes and versions. No STL types, C++ classes or exceptions cross it.
  - The Stracker shim is the one exception. It must reproduce Stracker's C++
    `LOG` ABI exactly (MSVC, release STL).
- Fail closed and visibly:
  - a plugin, signature or invariant that fails is logged and skipped;
  - the user can see why (status page or popup, not only a log);
  - a failure is never fatal to the game.
- Near-zero per-frame cost with the menu closed: no managed runtime, no
  per-frame allocation, no per-draw hooks. Measure the cost on the present
  thread.
- One hook per D3D12/DXGI entry point, with ordered callbacks. Never stack
  independent inline patches on the same function.
- Paths under MO2: GetModuleFileNameW returns the virtual game-folder path,
  while canonical() and GetFinalPathNameByHandle resolve to the real mod
  folder. Never compare paths from the two sources (this broke the DLSS mod in
  game-51). Compare file identity, or keep to one derivation.
- The launcher:
  - finds the game from, in order: a `MonsterHunterWorld.exe` path as its
    first argument (Steam's `%command%`), then next to itself, then the
    current directory. No hard-coded paths;
  - passes the remaining arguments through, starts the game in the game
    folder, waits for it and returns its exit code;
  - fails visibly but never fatally. If it fails before resuming the game, it
    ends the suspended process it created and says why. If `cmfm.dll` does
    not confirm it loaded, the game keeps running without CMFM, and the
    launcher says so.
- Under MO2 the launcher derives `cmfm.dll`'s real path from a file handle
  and injects that, so loading CMFM does not depend on usvfs timing in the
  child.
- Keep third-party dependencies explicit and pinned, and record their licences.
  Forking Stracker's Loader (MIT) needs its copyright notice; its log code
  carries a BSD-2 notice.

## Verification

- Build with warnings as errors in project code.
- Unit-test pure logic (config parsing, ordering, ABI tables, path handling)
  outside the game.
- Test Stracker compatibility against the user's installed plugins (list in
  the handoff). Test with and without leftover Stracker files.
- Test every launch setup, each with the Steam overlay on and off:
  - MO2 through the CMFM launcher entry, no force-load;
  - the same with SPL force-loaded;
  - a manual install with the launcher started directly;
  - Steam launch options with `%command%`.
- Under MO2, confirm that the virtual folder still reaches the game started
  by the launcher: mod files and Stracker plugins load from their mod
  folders.
- Check stability across loading screens, cutscenes, menus, alt-tab, resize
  and full-screen toggles, with and without the DLSS mod (frame generation on).

## Git Hygiene

- Git is the source of truth: focused commits, and an annotated tag for each
  version the user has tested in game.
- Commit locally; do not push without the user saying so.
- Keep build trees, logs, runs, dumps, game files and third-party binaries
  untracked (`local/`).
- Do not rewrite or discard user changes without explicit approval.

## Working With the User

- The user grants broad autonomy: decide, act and report; they object when
  they disagree. Present plans briefly and proceed on "go ahead".
- Report measured facts and say plainly what is unverified.
- Use plain language in user-facing summaries.
- The user plays online; that is fine for testing.
