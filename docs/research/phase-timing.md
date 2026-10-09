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
  import bridge has only saved registers at that point. Compare `entry_qpc`
  to the watcher's dxgi load QPC: a large earlier gap is outside this helper.
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
