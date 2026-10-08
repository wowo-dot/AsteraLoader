# PRE_MAIN in the real game (2026-10-09)

Results for the PRE_MAIN phase described in `../handoff-pre-main-20261008.md`.
Measured facts come from run `pre-main-1` and read-only reads of the exe
(c2ebbbd2…); everything else is labelled.

## Run pre-main-1 (confirmed)

MO2 profile `cmfm` (CMFM, DLSS mod as winmm.dll, Stracker's Loader), "CMFM
Launcher" entry, no force-load, Steam overlay on, no CMFM plugins. CMFM
0.1.0-dev at 6591057.

- PRE_MAIN was **not** reached. The core log has
  `phase_pre_main_observer_armed`, then after 15 s
  `startup_api_observer_removed` and `phase_pre_main_skipped: no verified
  post-unpack startup transition within 15 seconds`. There is no
  `unpacked_crt_verified_after_protection_change`. The warning popup
  appeared, as designed.
- Otherwise the game started and exited normally (code 0, returned by the
  launcher). MO2 files still reached it: the DLSS mod's winmm.dll and
  Stracker's dinput8.dll, loader.dll and plugins loaded as before.
- The log does not say which `VirtualProtect` calls the observer saw or why it
  rejected them. These cannot be told apart yet:
  - SteamStub never calls kernel32's `VirtualProtect`;
  - it calls it from outside `.bind`;
  - it calls it with a range or protection the detector does not accept.

Startup pause, compared with launcher-mo2-1:

| Interval | launcher-mo2-1 | pre-main-1 |
|---|---:|---:|
| launcher start → game created | 207.9 ms | 1.5 ms |
| created → cmfm.dll attached | 53.7 ms | 17.1 ms |
| bootstrap → resume | 229.0 ms | 70.3 ms |
| **created → resumed (game paused)** | **283.4 ms** | **88.0 ms** |

The bootstrap lines have no timestamps, so what fills its 70 ms is not
measured. The exe hash finished on its background thread 224 ms after
create.

## The exe on disk (confirmed, read-only)

- `.text` is encrypted on disk: entropy 8.00 in every 64 KiB block, and none
  of the int3 padding runs or common prologues plain MSVC code has. SteamStub
  decrypts it at run time, so the startup wrapper can only be verified in
  memory.
- The exe's import table (in `.rdata`, 188 KERNEL32 imports) includes
  `VirtualProtect`. CMFM's observer targets
  `GetProcAddress(kernel32, "VirtualProtect")`.

## Next

Repeat the same launch setup with the diagnostic build described below.
PRE_MAIN in the real game remains unverified until that run is analysed.

## Diagnostic build for the repeat run (prepared, untested in game)

The observer records the first 256 `VirtualProtect` calls while armed, including
rejected calls. Each record has entry/return QPC timestamps, thread ID, caller
and target addresses, caller module/section/RVA, target module/section/RVA,
size, requested and previous protection, API result and last error. The
`from_bind`, `covers_text` and `executable_read` fields show each existing
filter's result; `state` shows whether the detector was waiting or already busy.
Acceptance filters and the startup signatures are unchanged.

Recording uses fixed storage without allocation or log I/O in the observer.
The existing deadline worker formats and writes published records, polling
every 250 ms. The final `startup_vprotect_summary` reports total calls,
recorded calls, the cap, truncated calls and any still-pending records. Calls
after the recording window closes are excluded. Overflow leaves the detector working;
it only limits diagnostic detail. No new observers were added.

The old-protection output is read only after success, with a recoverable-fault
guard, and is otherwise logged as `unavailable`. The API's return value, output
and last error remain unchanged. Last error on a successful call may simply
be a value left by an earlier call; only the API result indicates failure.
Main-exe section metadata is saved before startup. Other module/section names
are resolved later and may be unavailable or stale after unload; raw addresses
and call timestamps remain the captured values.

Every core-log line now has `qpc` and microseconds since bootstrap entry.
Checkpoints bracket DLL hashing, EXE-hash thread creation, image metadata,
MinHook initialization, lifetime pinning, observer creation/activation and
bootstrap completion. The first checkpoint includes time spent resolving the
DLL's real path and creating the log. This should identify the unmeasured
70 ms from `pre-main-1`; diagnostic logging itself also has a cost.

Verification uses original synthetic executables only: a successful transition
dispatches PRE_MAIN once on the main thread and restores its wrapper; the
missing-transition case deliberately makes over 300 rejected calls, records
exactly 256, reports truncation and still exits normally. Failed calls retain
their output sentinel and last error. A second successful-transition test fills
the trace first and confirms PRE_MAIN still runs after the cap. A concurrent
buffer test checks capacity and unpublished slots. All 18 Release CTest cases
passed with warnings as errors;
the core still imports only KERNEL32. This is evidence about the implementation,
not confirmation that the real game's transition will match.
