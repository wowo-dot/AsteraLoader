# Device-call timing diagnostics (2026-10-09)

The measured dxgi-to-first-device gap grew from 262-398 ms to 1060-1202 ms
with the API 1.2 phase observers (`plugins-in-game.md`). The old log recorded
observer success only after arming, so it did not locate that extra time.
This diagnostic build measures the existing path; it makes no speedup claim.

The context worker is already created in `phases::arm`, after PRE_MAIN. It
arms the WinMain breakpoint before the first device call, and waits on an
event between requests. No worker creation was moved or added to device entry.

For the first three calls through the armed import:

- `phase_device_call_enter thread_id=... call=... entry_qpc=...` captures
  entry to the C helper, immediately after preserving last error and before
  validation, logging, allocation or the worker handshake. The generated
  import bridge has only saved registers at that point. Convert `entry_qpc`
  to milliseconds after process creation using the core's attach QPC and
  process-relative attach time, then compare the watcher's dxgi `load_ms`.
  The watcher records PE load times, not QPC; a large earlier gap is outside
  this helper.
- `startup_hook_timing operation=device_return_arm elapsed_us=...` records
  the observer handshake with `begin_qpc`, `end_qpc`, thread/call and success.
  Fields measure event reset/signalling, worker wake, thread open/suspend,
  context get/reservation/set, resume and caller wake. Timing is gathered
  without formatting or file writes on the worker, before it signals ready.
  `worker_wake_us` starts before SetEvent; `caller_wake_us` starts just before
  the ready SetEvent, so both include event signalling and scheduling.
  Fields are elapsed wall-time and may overlap: the worker can wake and suspend
  the caller before its signalling interval ends. Do not add the fields to
  reconstruct total time; use `elapsed_us` and the raw endpoints for that.
- `phase_device_call_forward` measures entry to the final forwarding-log
  start, return-bridge construction, the handshake, and earlier diagnostic
  formatting/file writes. `diagnostic_log_us` includes the entry record,
  observer timing record and legacy armed record; these writes are excluded
  from `observer_us`. No log duration is counted as context API work.
- `phase_device_call_return` records the forwarding-log duration and raw
  `forward_ready_qpc`, saved after that log returned. Its forward-to-return
  interval includes import resolution, DirectX/vendor work, the breakpoint
  exception and return-bridge entry, not just the DirectX call itself.

WinMain's handshake uses the same timing record with `operation=win_main_arm`.
Both records include the one-time CreateThread API duration and create-to-worker
entry delay. `worker_precreated=1` makes the existing placement explicit.
All raw QPCs use the core log's `qpc_frequency`.

Phase selection, breakpoint/slot ownership, forwarding and the plugin ABI are
unchanged. Diagnostics stop after three intercepted calls; there is no per-frame
polling. Log failures remain optional and last error is restored before the
original API and after the return observation.

The two WARP stand-ins now make two capability probes, create a real device
and create another device after observer removal. They check exactly three
entry records and verify each timeline surrounds actual API entry, as captured
independently in the test export hook. The usual caller/code checks, payloads,
once-only callbacks, phase order and last-error checks remain. No hard timing
thresholds are used. The separate setup-thread case also passes.

In one initial stand-in sample, device handshake times were 58, 48 and 40 us;
entry-to-forwarding-log times were 166, 73 and 80 us. These are implementation
measurements, not an explanation of MHW's delay. The next MHW run on the same
profile should distinguish an expensive arming step from time before CMFM entry.

## Runs phases-timing-1 and phases-timing-2 (confirmed)

Core 0e07f55, retained launcher/clients 266f909, MO2 `cmfm-stracker`, no
force-load, overlay module observed. PID 31532 was the title-screen run;
PID 47864 was the user's requested save-load run before normal exit. The
installed core hash was
`bddcaf58d75d0b603293f7303ff94c475f5f337a993aaf817138c3f220ea3655`.

| Measurement | phases-timing-1 | phases-timing-2 |
|---|---:|---:|
| dxgi load to D3D12Core load, as in the earlier comparison | 298.817 ms | 392.036 ms |
| dxgi load to first CMFM device-entry timestamp | 297.874 ms | 391.386 ms |
| first entry to D3D12Core load | 0.943 ms | 0.650 ms |
| entry to forwarding-ready, calls 1/2/3, including logs | 0.314 / 0.201 / 0.228 ms | 0.296 / 0.189 / 0.236 ms |
| observer handshake, calls 1/2/3 | 64 / 43 / 65 us | 87 / 51 / 46 us |
| WinMain observer handshake | 233 us | 232 us |
| worker CreateThread API duration | 39 us | 42 us |
| create to worker entry, once during PRE_MAIN | 200 us | 195 us |

The earlier 1060-1202 ms gap did not reproduce. Both new gaps are within the
262-398 ms range of the three pre-phase runs. Almost the whole measured gap
is before CMFM's first device-entry helper; the forwarding path costs less
than 0.32 ms per call in these runs. The six context reads took 13-25 us,
context writes 10-29 us, and worker wakes 6-13 us. Return-bridge construction
took 9-11 us. Neither worker startup nor the measured arming/forwarding work
explains a 0.7-0.8 s increase here.

After forwarding, the third call took about 2.39/2.14 s to reach CMFM's return
bridge. This interval includes DirectX/vendor/mod initialization and exception
delivery; it is not CMFM arming time. DEVICE_CREATED dispatch itself took
26/27 us. No graphics-export hook, thread priority or other behavior was
changed to produce these results; the build adds diagnostics. Added logging
can affect scheduling, and the runs are not a controlled explanation of the
earlier slowdown. A recurrence needs another timestamped sample before a
performance fix is justified.

Both runs delivered PRE_MAIN, WIN_MAIN, DEVICE_CREATED and SHUTDOWN once to
hello 0.3.0, with the expected payloads and WinMain return 0. The temporary
observer/import cleanup was recorded, all 19 legacy plugins loaded with zero
failures, and the games exited with code 0. There were no core warnings.
The watchers recorded 176/174 modules, zero torn polls and zero suspect
layouts. Each watcher reported an access-denied open for another PID after
the measured game's exit; those unrecorded processes are excluded. These
observations do not establish a complete play-session stability pass.

Both run folders are sealed with core/client/launcher logs, module evidence,
`timing-analysis.json` and `result.json`. Tested source 0e07f55 is tagged
`v0.1.0-dev.9`; the diagnostic build remains installed.
