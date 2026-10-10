# Astera Loader (AL) and CMFM (Monster Hunter: World)

## Objective

Astera Loader ("AL") is an open-source native loader and framework for the
Windows PC version of Monster Hunter: World (DirectX 12). CMFM ("Configuration
Menu For Mods") is its in-game settings menu, shipped as a first-party plugin.
Until 2026-10-09 the whole project was called CMFM, and older docs, handoffs
and run records use that name for the loader (`decisions.md` item 11).

- **Astera Loader:** our own launcher and native loader (the SKSE route).
  - `AsteraLoader.exe` starts the game suspended, loads `AsteraLoader.dll` into it,
    then resumes it. AL puts no proxy DLL in the game folder.
  - `AsteraLoader.dll` is in before Steam's DRM stub and before any game code, in
    every launch setup, and dispatches load phases to plugins.
  - Exposes a versioned C plugin API (`al_` prefix, `include/al/al_plugin.h`)
    with load phases.
  - Coordinates Direct3D 12 / DXGI hooks between mods.
  - Makes Stracker's Loader unnecessary: existing Stracker plugins run
    unchanged with no `dinput8.dll` or `loader.dll` in the game folder.
- **CMFM:** the in-game settings menu, a plugin of the loader. Plugins
  register typed settings, status pages and warning popups.

The user's guiding principle: fewer loaders, proxy DLLs and bandaids over the
exe. AL replaces Stracker's Loader. It coexists with SharpPluginLoader (SPL)
and does not compete with it: AL never takes `winmm.dll`, `dinput8.dll`,
`dxgi.dll` or any other proxy name.

v1 is launcher-only. A proxy fallback for Steam's plain Play button is added
only if Steam launch options (`"<path>\AsteraLoader.exe" %command%`) turn out
not to work.

The first plugin client is the sibling DLSS / Frame Generation mod. Its
requirements are in `docs/requirements-dlss-mod.md`.

Start with `docs/handoff-20261008.md`, then `docs/decisions.md` (which
supersedes the handoff where they differ) and `docs/research/entry-slot.md`.

## Canonical Local Paths

- Repository: `D:\mh world modding\AsteraLoader`
- Sibling DLSS mod (read-only reference from here): `D:\mh world modding\dlssupdate`
- Sibling CPU project (read-only reference): `D:\mh world modding\cpuopt`
- Game directory: `D:\SteamLibrary\steamapps\common\Monster Hunter World`
- Game executable: `D:\SteamLibrary\steamapps\common\Monster Hunter World\MonsterHunterWorld.exe`
- Mod Organizer 2 (portable): `C:\Modding\MO2\ModOrganizer.exe`, data directory `D:\MO2\MHW`.
  Profiles: `Default`, `dlss`, `barebonesdlss`. Stracker's Loader is the MO2 mod
  `Stracker's Loader`.
- IDA 9.4: `C:\Program Files\IDA Professional 9.4\ida.exe` (the Hex-Rays IDA MCP
  needs 9.4; 9.3 stays installed). Private database:
  `local\ida\input-20261010\decrypted.i64`.
- Decoded game image (read in place):
  `D:\mh world modding\ai\glavenus\MonsterHunterWorld_decrypted.exe`, SHA-256
  `d78611731e4a5f214e306d1d577d4782b59adaf1c75de54d2983caa45074884c`, decoded from
  the tested exe `c2ebbbd2…90ea`. The exe on disk has SteamStub-encrypted code,
  so offline signature checks run on this image.
- Claude Code reversing tools: `C:\Users\akifs\.claude\re-tools\` (Python 3.12:
  `py -3.12 -B`); x64dbg: `D:\reverse engine jank\x64dbg\release\x96dbg.exe`.
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
  anti-cheat, licensing or platform protections. Astera Loader itself must not
  change gameplay or network behaviour. What plugins do is the user's choice.
- The launcher is the only AL component that writes into the game process,
  and only to load `AsteraLoader.dll`:
  - it only creates `MonsterHunterWorld.exe` itself, suspended. It never
    attaches to a running game or touches any other process;
  - it loads `AsteraLoader.dll` by full path from AL's own folder, and changes
    nothing else in the process;
  - it leaves Steam's DRM stub (SteamStub) and its checks to run untouched.
    `AsteraLoader.dll` and plugins install hooks only after the game's code is
    unpacked;
  - it needs no administrator rights, and has no network access or
    auto-update.
- Never touch `D:\mh world modding\ai\glavenus` IDA databases (another
  project). Work on private copies under `local/` only.
- Do not commit or redistribute Capcom binaries, decoded images, game assets,
  NVIDIA binaries, other mods' binaries, crash dumps or IDA databases.
- Never send project data, logs or binaries to external services.
  - The one exception (decisions.md item 15): AL's own release binaries,
    built from this repository, go to VirusTotal. Claude or the user
    uploads them. Nothing else is uploaded: no NVIDIA DLLs, other mods'
    binaries, game files, logs, dumps or dev builds.
- Do not reverse engineer NVIDIA binaries. Third-party mod DLLs (Stracker's,
  SPL, plugins) are studied from their published source where it exists.
  Only inspect a binary's import and export tables when no source exists.

## Runtime Testing Protocol

- The user launches the game, always through MO2: the AL launcher entry
  for AL runs, the plain "Monster Hunter: World" entry for baselines. Never
  launch MHW or the launcher through Steam, a script or computer-use. Steam
  launch-option tests are run by the user.
- Before every launch, prepare a run folder (`local/runs/<name>`) and the
  installed build. After the game closes, seal the folder with a short result
  file.
- Keep the installed test build in place between runs; announce any
  intentional stop or removal.
- Run logs identify:
  - the exe SHA-256 and the AL DLL SHA-256 and version;
  - the MO2 profile, with or without force-load;
  - every loaded plugin with its version.
- Measurement tools reading the game process (`tools/modwatch.py` and the
  like) are strictly read-only (x64dbg and Frida: see Reverse-Engineering
  Tools):
  - access `PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ`;
  - no writes, no thread suspension, no remote threads or allocations.

  The PEB module list read this way shows load order, load times and the
  parent module of each load. The launcher is not a measurement tool; see
  Safety and Scope.
- When screenshotting the desktop, crop to the game window.

## Reverse-Engineering Tools

Claude Code has an IDA MCP (Hex-Rays), an x64dbg MCP, a Frida MCP and
`sigscan.py`; the user-level `reversing` skill describes them. This file's
rules come first.

- **IDA:** open only the private database under `local\ida\` (copy a new one
  there when needed), never anything in `ai\glavenus`. Ask through the MCP's
  `execute_python` instead of writing one-off headless scripts. Names, types
  and comments are saved to the private copy only; exports such as decompiled
  functions go to `local\`.
- **Signatures** (`py -3.12 -B C:\Users\akifs\.claude\re-tools\sigscan.py`, on
  the decoded image):
  - `audit <image> src probes` before staging a build that adds or changes a
    pattern. A pattern the code searches for must match exactly once. A
    pattern checked only at a pinned address may match elsewhere: on
    2026-10-10 `kExitPattern` (`frame/sites.cpp`) matched 3 times, which is
    fine there; the other three matched once.
  - `make <image> <VA>` gives the shortest unique pattern for a new site and
    `scan <image> "<pattern>"` checks one (`--rel N` resolves a rel32). Record
    the pattern, RVA and exe hash in the design doc as usual.
- **x64dbg and Frida on the running game** are research tools, not AL
  components, and the only exception to the read-only rule above:
  - the user launches through MO2 as always and says OK in the session before
    each attach; never attach during startup (SteamStub, AL's load phases);
  - prefer x64dbg hardware breakpoints and non-stopping logging breakpoints:
    they change no code bytes, and the game re-checks its code with CRC;
  - Frida hooks only functions AL or a plugin already hooks. Software
    breakpoints, memory writes, other hooks and calls into game code each
    need their own OK;
  - offline is preferred: a paused game drops its online session;
  - detach when done (x64dbg `detach`, never `terminate_session`) and note
    what was attached, when and why in the run folder.
- Point the tools at the game's code only, never at NVIDIA DLLs or other
  mods' binaries (Safety and Scope).

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
    ends the suspended process it created and says why. If `AsteraLoader.dll` does
    not confirm it loaded, the game keeps running without AL, and the
    launcher says so.
- Under MO2 the launcher derives `AsteraLoader.dll`'s real path from a file handle
  and injects that, so loading AL does not depend on usvfs timing in the
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
  - MO2 through the AL launcher entry, no force-load;
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
