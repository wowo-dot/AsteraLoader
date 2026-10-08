# Launcher startup implementation handoff (2026-10-08)

> **Update 2026-10-08:** run launcher-mo2-1 tested this build in the real game
> under MO2 (no force-load, overlay on): CMFM confirmed before resume, loaded
> before usvfs and SteamStub, and the virtual folder still reached the game.
> No exe TLS callbacks exist. See [research/entry-slot.md](research/entry-slot.md).

The missing launcher and core startup proof are implemented. The helpers Claude
left uncommitted are retained, with path fixes and log-write error tracking.
There is no plugin loading, Stracker shim, graphics hook or menu yet.

## Implemented behavior

- `src/launcher/main.cpp` finds only `MonsterHunterWorld.exe`, following the
  argument / launcher folder / current folder order. It forwards arguments,
  uses the game's folder as its working directory, waits, and returns its exit
  code. Paths are determined at runtime.
- It opens `cmfm/cmfm.dll` beside the launcher, retains that file handle, and
  derives the real full path with `GetFinalPathNameByHandleW`. Under MO2 this
  avoids depending on child usvfs interception to locate CMFM.
- It creates its own child with `CREATE_SUSPENDED`. Only the handles returned
  by this call are used for process access. There is no attach-by-PID path.
- A helper thread calls the existing ntdll `RtlGetVersion` entry, allowing the
  Windows loader to initialize while the primary thread stays suspended. The
  launcher then invokes the child's existing `LoadLibraryW` with CMFM's real
  path. Windows function addresses are rebased from matching mapped NT file
  paths and checked image headers/code pages; local addresses are not assumed
  valid in the child. Forwarded kernel32 exports are resolved to their actual
  owning image.
- The only remote allocations are temporary read/write parameter buffers.
  There is no copied executable stub, entry-point patch, game hook or change
  to SteamStub, integrity checks, assets, privileges or network behavior.
- The core's `DllMain` saves its module handle and attach timestamps. Its
  `cmfm_bootstrap` export runs on a separate loading thread after `LoadLibraryW`
  finishes, outside the loader lock. It validates a fixed-size/version request,
  logs hashes, timing and selected loaded modules, and confirms startup.
- The launcher checks the core's mapped physical path and bootstrap response
  before resuming the primary thread. It does not treat a 32-bit thread exit
  code as a 64-bit module handle.
- Completed loads without CMFM confirmation resume the game and show a warning.
  Preparation failures or a stuck loading thread terminate only the suspended
  child, before showing the error. Each loading thread has a 15-second limit.
  A parameter still used by a timed-out thread is left allocated until child
  termination, rather than freed underneath that thread.

Logs are `cmfm/logs/launcher.<launcher-pid>.log` and
`cmfm/logs/core.<game-pid>.log` in the real CMFM installation. They include
CMFM's version and source commit, executable/DLL SHA-256, process creation
time, attach timing, QPC values, bootstrap status and exit code. User-profile
paths are redacted, including extended paths returned from file handles.
MO2 profile and force-load must be recorded separately in each run manifest;
the launcher cannot reliably infer them. No CMFM plugins are loaded in this
proof, which the core log states explicitly.

## Verification outside the game

The Release build uses MSVC, C++20, `/W4 /WX`, and a static CRT. The core's
import table contains only `KERNEL32.dll`. CTest covers:

1. Argument quoting (including 4,096 generated combinations), launch discovery,
   drive roots, UNC/extended paths, username redaction, UTF-8 and known SHA-256
   vectors and block boundaries.
2. Core startup request size/version, null request and duplicate initialization.
3. The production launcher with a locally built stand-in named
   `MonsterHunterWorld.exe`: CMFM confirmed before resume, loaded before the
   stand-in's main, arguments preserved, correct working folder and exit code 37.
4. Missing/invalid DLL and missing explicit executable: no child main runs.
5. Core bootstrap declining: game continues and returns its exit code.
6. DLL attach rejection: game continues without the core.
7. DLL attach hanging: the suspended child is terminated without running main.

Failure tests compile the same launcher source with dialogs disabled in a
test-only target. The normal production executable keeps error/warning dialogs.
Fault DLLs and the stand-in are test artifacts and are excluded from packaging.

Build with an installed MSVC x64 toolchain:

```powershell
cmake -S . -B local/build -A x64
cmake --build local/build --config Release --parallel
ctest --test-dir local/build -C Release --output-on-failure
cmake --install local/build --config Release --prefix local/stage/0.1.0-dev
```

The distribution has `cmfm_launcher.exe`, `cmfm/cmfm.dll` and `LICENSE`.
`BUILD_TESTING=OFF` omits all test targets. Binaries, test logs and packages stay
under ignored `local/`.

## Still unverified / next work for Claude

No real game or CMFM launcher against the real game has been started by this
agent. No game-directory files, MO2 executable entries or mod enablement were
changed. Run an actual test only after preparing its installed build and run
folder; the user launches through MO2, as required by `AGENTS.md`.

The stand-in proves ordering before main. It does **not** prove ordering before
SteamStub or all possible executable TLS callbacks: Windows loader initialization
may run executable TLS callbacks before the helper thread's entry. The MHW-specific
ordering must be measured before claiming the full load-point requirement.
Do not patch or bypass SteamStub to resolve any ordering issue.

Next: measure a user-launched MO2 run for early CMFM arrival and child usvfs
inheritance. Verify virtual mod files still reach the game. Test overlay on/off
and SPL force-load combinations, then user-run manual/Steam-options launches.
Only after that add post-unpack phase dispatch, the public C plugin API, and
Stracker compatibility. The current core installs no hooks and has zero
per-frame work.

## API references used

- [CreateProcessW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)
  documents suspended child creation, mutable command lines and working folders.
- [CreateRemoteThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createremotethread)
  documents startup/DLL-initialization serialization and loading-thread limits.
- [DLL best practices](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices)
  explain why normal initialization is kept outside `DllMain`.
- [RtlGetVersion](https://learn.microsoft.com/en-us/windows/win32/devnotes/rtlgetversion)
  documents the one-argument ntdll query used for Windows loader initialization.
- [GetMappedFileNameW](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getmappedfilenamew)
  identifies mapped images without relying on initialized module lists.
