# PRE_MAIN in the real game (2026-10-09)

Results for the PRE_MAIN phase described in `../handoff-pre-main-20261008.md`.
Measured facts come from runs `pre-main-1` through `pre-main-3` and read-only reads of the exe
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

## Run pre-main-2 (confirmed)

Same MO2 setup, core 89fcfaf, retained launcher 6591057. PID 18668 exited with
code 0. The watcher captured 159 modules with no torn or suspect polls; overlay
and the MO2 DLSS/Stracker files loaded normally. PRE_MAIN was again skipped
after the deadline. This run used the diagnostic build installed in MO2.

The trace identifies a rejected transition, so the next change can be specific:

| Record | Caller | Target | Size | Protection | Filters |
|---|---|---|---|---|---|
| 129 | `0x180004ca6`, private allocation | game `.text`, RVA `0x1000` | `0x2e12000` | execute/read → read/write | covers text, caller outside `.bind` |
| 130 | `0x180004d3e`, unmapped when reported later | same | same | read/write → execute/read | covers text, executable/read, caller outside `.bind` |

Both API calls succeeded on thread 64348. Record 130 occurred 578.646 ms after
resume and passed every transition filter except `from_bind`. Four earlier
calls from `.bind` changed protections in the `0x180000000` allocation. This
is consistent with a temporary startup helper; its exact implementation was
not inspected. The actual CRT signature at record 130 is still unverified:
the old caller filter prevented that scan from running.

The observer saw 556 calls, recorded the first 256, truncated 300 and had zero
pending records. Retained calls span 6.830–668.549 ms after resume. The two
full-text transitions are within the retained portion; claims about later
calls cannot be made from this trace. Caller metadata resolved later can be
unavailable after unload, as record 130 demonstrates.

Timing now localizes the startup pause:

| Interval | pre-main-2 |
|---|---:|
| launcher start → before CreateProcess | 1.513 ms |
| before CreateProcess → resume | 89.889 ms |
| bootstrap entry → resume | 72.942 ms |
| synchronous DLL hash checkpoints | 0.997 ms |
| observer created → observer armed (covers MinHook enable) | 70.573 ms |

Most bootstrap time is in the interval that enables the observer, rather than
hashing. That is a measurement of this run, not an explanation of MinHook's
internal delay. The background EXE hash finished 221.716 ms after the
before-CreateProcess checkpoint.

The run is sealed under `local/runs/pre-main-2` with both CMFM logs, module
report and `trace-analysis.json`. The tested diagnostic source is tagged
`v0.1.0-dev.2`.

## Run pre-main-3 (confirmed: PRE_MAIN reached)

Same MO2 setup, core d589db2, retained launcher 6591057. PID 10916 exited with
code 0. The user reported no popup; the core has no warnings or skipped/failed
phase. The watcher captured 156 modules across 2104 polls with zero torn or
suspect snapshots. Overlay, the MO2 DLSS winmm.dll and Stracker files loaded.

The caller-independent fix worked in this setup:

- Record 130 again restored the full `.text` from read/write to execute/read,
  caller `0x180004d3e`, outside `.bind`, on thread 2752.
- The guarded scan verified one CRT wrapper: entry RVA `0x2742a48`, cookie
  initializer `0x2743690`, common-main `0x27428d4`.
- `unpacked_crt_verified_after_protection_change` was followed by
  `startup_api_observer_removed`, then one `phase_pre_main_enter` and one
  `phase_pre_main_complete; startup wrapper restored`, on thread 2752.
- Discovery used the virtual game-folder path
  `Monster Hunter World\cmfm\plugins`, with the expected EXE hash. No CMFM
  plugin was installed, so this verifies reaching the host/discovery/dispatch
  path, not a real client plugin's initialization or settings.
- The observer recorded all 133 calls before it stopped: no truncation or
  pending records. The actual CRT match is now measured for this game hash.

| Interval | pre-main-3 |
|---|---:|
| before CreateProcess → resume | 89.967 ms |
| bootstrap entry → resume | 73.849 ms |
| observer created → armed (covers MinHook enable) | 71.986 ms |
| DLL hash checkpoints | 0.967 ms |
| full-text transition return → entry hook ready | 80.207 ms |
| full-text transition return → PRE_MAIN enter | 178.730 ms |
| PRE_MAIN enter → complete (no plugins) | 0.175 ms |
| background EXE hash wait at PRE_MAIN | 1 µs |

PRE_MAIN entry occurred 841.237 ms after the before-CreateProcess checkpoint,
751.270 ms after resume. The 80/179 ms intervals include signature scanning,
hook work and startup execution between checkpoints; they are not isolated
measurements of any one function. The D3D12 device marker was later in this
run than in pre-main-2; two startup runs alone do not identify a cause.

The run is sealed under `local/runs/pre-main-3`, with both CMFM logs, module
report, `trace-analysis.json` and result. The tested build is tagged
`v0.1.0-dev.3`. The installed core and launcher stay in place.

## The exe on disk (confirmed, read-only)

- `.text` is encrypted on disk: entropy 8.00 in every 64 KiB block, and none
  of the int3 padding runs or common prologues plain MSVC code has. SteamStub
  decrypts it at run time, so the startup wrapper can only be verified in
  memory.
- The exe's import table (in `.rdata`, 188 KERNEL32 imports) includes
  `VirtualProtect`. CMFM's observer targets
  `GetProcAddress(kernel32, "VirtualProtect")`.

## Next

Step 3 is confirmed for the tested MO2/no-force-load/overlay-on setup. All 19
Release CTest cases passed for d589db2. The core still imports only KERNEL32;
private bootstrap and public plugin ABIs are unchanged.

Later validation should cover a real CMFM client plugin and the remaining
launch/overlay/SPL configurations. No fixture plugin or new mod was enabled
for these runs. Further work can also measure the hook setup/removal costs
separately before optimizing the startup delay. WinMain/device phases,
Stracker replacement and menu work remain separate next steps.

## Startup optimization (confirmed in startup-opt-1)

The source of the system-wide enumeration cost is MinHook 1.3.4's
`EnumerateThreads`: `CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)` captures
all system threads before filtering by process. The CMFM adapter first uses
the documented Windows process-snapshot API, with `GetCurrentProcess()` and
`PSS_CAPTURE_THREADS` only. It returns current-process IDs excluding the caller.
No memory clone, thread context or handle-table capture is requested.

The adapter owns snapshot/marker cleanup and returns its ID array on MinHook's
private heap. Any capture/query/walk/allocation failure discards the partial
list and uses MinHook's original Toolhelp path. Suspension, instruction-pointer
relocation, patching and resume logic remain unchanged. The 13-line vendored
adapter patch is recorded in `third_party/README.md`; notices and the original
dependency pin are retained. API references:
[capture](https://learn.microsoft.com/en-us/windows/win32/api/processsnapshot/nf-processsnapshot-psscapturesnapshot),
[flags](https://learn.microsoft.com/en-us/windows/win32/api/processsnapshot/ne-processsnapshot-pss_capture_flags).

Normal PRE_MAIN restoration now queues both owned startup patches and applies
them under one freeze. Each patch must still match CMFM's saved bytes; conflicts
and partial failures retain individual recovery. Trampolines remain allocated
for in-flight calls. The full-text transition and unique CRT signatures are
unchanged. No cached thread list or unprotected instruction patching is used.

Local measurements in `local/runs/startup-opt-standin-1`:

| Measurement | Result |
|---|---:|
| system Toolhelp enumeration, 9 samples, 16 owned workers | median 50,107 µs; range 49,050–52,481 µs |
| process-only PSS enumeration, same test | median 59 µs; range 58–125 µs |
| observer enable, synthetic helper run | 398 µs |
| CRT entry enable, same run | 229 µs |
| paired startup restoration, same run | 242 µs |

All 25 Release CTest cases passed with project warnings as errors. Tests verify
the owned workers are found and the caller excluded; eight workers keep calling
two targets through 12 paired enable/disable cycles; a pre-existing suspension
stays intact. The same hook test passes with PSS failure forced, exercising
Toolhelp fallback. Existing phase, ABI, launcher, plugin and Stracker tests pass.
The core still imports only KERNEL32.

`startup_hook_timing` now reports operation duration, successful process
snapshots and Toolhelp fallbacks. `startup_signature_scan_us` separates the
signature search from hook enable. These are local measurements, not a claim
about MHW's improvement by themselves. The following game run measures it.

### Run startup-opt-1 (2026-10-09, confirmed)

Core 5ee3591, retained launcher 6e0dc18, PID 46816, MO2 `cmfm`, `CMFM Launcher`,
no force-load, overlay present. PRE_MAIN ran once on thread 38640, both startup
patches were restored, and the game exited with code 0. No core warnings.
`hello` and `stracker_compat` 0.1.0 remained active; hello recorded one phase-0
callback and the expected virtual/real MO2 folders. The shim reported two
Stracker plugins found, two loaded, zero failed, game version 421810.

| Measurement | Previous game run | startup-opt-1 |
|---|---:|---:|
| before CreateProcess → resume | 84.647 ms | 18.423 ms |
| observer created → armed checkpoints | 67.134 ms | 0.188 ms |
| observer enable, isolated operation | not isolated | 0.173 ms |
| CRT entry enable, isolated operation | not isolated | 0.282 ms |
| both startup patches restored, isolated operation | not isolated | 0.172 ms |
| PRE_MAIN enter → complete (client work included) | 623.631 ms | 620.250 ms |

The previous column is `stracker-mo2-3`. The before-CreateProcess-to-resume
interval fell by 78.2%; it includes process creation/initialization as well as
the paused bootstrap, so it is not a pure MinHook measurement. Bootstrap entry
to resume was 2.112 ms. All three hook operations used one process snapshot
each and zero Toolhelp fallbacks; their combined measured cost was 0.627 ms.
Signature search was 26.456 ms and the EXE-hash wait at PRE_MAIN was zero.

The prepared manifest expected Stracker's original loader enabled, but the
actual run had it disabled. The collected profile has `-Stracker's Loader`;
module evidence shows the CMFM shim and later System32 DINPUT8, without the
old Stracker proxy/loader pair. Both compatibility clients are active. The
manifest preserves the planned mod list and records the observed setup.
This difference occurs after the startup hook operations, but overall loading
times are not a controlled A/B comparison and no general FPS claim follows.

The watcher saw 160 distinct modules with zero torn/suspect snapshots. All
133 observer records were retained, with no pending records or truncation.
The run is sealed under `local/runs/startup-opt-1` with core/launcher/client
logs, the module report and `timing-analysis.json`. Tested source 5ee3591 is
tagged `v0.1.0-dev.6`. The optimized installed build stays in place.

The thread-snapshot overhead is now below a millisecond. Remaining measured
startup costs include the 26 ms signature search and about 620 ms inside the
client/plugin phase; the latter was already present in the baseline and was
not changed by this optimization.

## Diagnostic build used in pre-main-2

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
