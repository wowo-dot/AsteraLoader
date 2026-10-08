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

Before the next run, have the detector log every `VirtualProtect` call it sees
while armed (caller module and section RVA, target RVA, size, new and old
protection, result; capped), and timestamp the bootstrap lines. Then repeat
this run unchanged.
