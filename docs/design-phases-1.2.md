# Design: the remaining load phases (API 1.2, part 1)

Status 2026-10-09: implemented and verified in original stand-ins (34 Release
CTest cases pass), and confirmed in MHW in `phases-mo2-1` and
`phases-timing-1`/`-2`. See `research/plugins-in-game.md` and
`research/phase-timing.md`. Actual SPL coexistence for the new phases remains
unverified. Roadmap item 8. The implementation
revises the initial call/return-site patch plan to preserve SPL's discovery and
the standalone DLSS mod's complete game-function hash.

## What ships now and what waits

| Phase | Ships | Fires | Payload (`data`) |
|---|---|---|---|
| PRE_MAIN | 1.0 | game code unpacked, before its CRT starts | NULL |
| WIN_MAIN | 1.2 | the CRT has finished; the game's WinMain is about to run | `al_phase_win_main_t` |
| DEVICE_CREATED | 1.2 | the game's own `D3D12CreateDevice` call returned a device, before the game uses it | `al_phase_device_t` |
| SWAPCHAIN_CREATED | Stage 3 | (reserved) | |
| FIRST_PRESENT | Stage 3 | (reserved) | |
| GAME_READY | later | (reserved; no game-side signal known) | |
| SHUTDOWN | 1.2 | the game's WinMain returned, before CRT teardown; objects local to WinMain may already be destroyed | `al_phase_shutdown_t` |

The swap-chain and present phases wait for the graphics hub (Stage 3). The
sibling DLSS mod inline-hooks every DXGI swap-chain entry point and Present,
compares the first 32 bytes of each hooked function, and turns itself off
when another hook appears there. It also routes calls only for the native
objects it pinned. Any AL hook on those functions today would disable it.
The hub, where AL owns one hook per entry point and the DLSS mod becomes an
AL plugin with callbacks, is the place for them. Source: the DLSS mod's
`src/reflex/presentation_probe.cpp` and `presentation.cpp`, read 2026-10-09.

## Guarantees

- Each phase fires at most once, in the order of the table, on the thread
  that reached it: the game's main thread for WIN_MAIN and SHUTDOWN, the
  game's render-setup thread for DEVICE_CREATED.
- A phase that cannot be detected, or whose site fails its checks, is
  skipped with a reason in the core log and a warning through the existing
  visible-warning callback. An absent D3D12 import is an expected skip.
- Plugins see what the game sees: the device pointer the game receives. A
  plugin that keeps a COM object beyond the call must AddRef it.
- Payload structs start with `size`; fields are only added at the end.
- A plugin's fault in `on_phase` disables that plugin, as for PRE_MAIN.
- Under DirectX 11 (`al.game` reports `graphics`), DEVICE_CREATED does not
  fire.

## How each phase is detected

All sites are in the game exe, found and verified at PRE_MAIN after the
plugins' PRE_MAIN calls, while the game's own code has not started.

### WIN_MAIN and SHUTDOWN: an execution observer at WinMain

- The CRT wrapper AL's startup check verified jumps to
  `__scrt_common_main_seh`. Inside it, the call to WinMain is the only
  `call rel32` that follows a `lea rcx, [image base]` within a few bytes
  (WinMain's first argument is the image base). SPL's published pattern for
  the same site (`Core::WinMainCall`, MIT) confirms the shape. The call
  target must lie in the exe's code.
- The search stays within the common-main function's PE unwind extent,
  accepts the two observed argument-move orderings, verifies the LEA resolves
  to the image base and all three call targets are executable code, and
  refuses ambiguity. It does not scan the whole text section again.
- A helper worker briefly suspends only the thread requesting the observer,
  reads its debug context, reserves one free local execution-breakpoint slot,
  sets it and balances its suspension. Neither the CRT call nor WinMain's
  bytes change. This matters because SPL resolves the CRT call target later,
  during cookie initialization: an early call redirect would make SPL hook
  AL's bridge instead of WinMain.
- On the owned WinMain breakpoint, the exception handler restores only the
  reserved debug slot and transfers the existing WinMain arguments/stack to
  AL's interceptor. It performs no plugin work inside the exception handler.
  The interceptor dispatches WIN_MAIN, calls the original entry (including
  SPL's hook if present), then dispatches SHUTDOWN with WinMain's return value.
  It preserves Win32 last error and returns the original value to the CRT.
- If the game exits by calling ExitProcess or TerminateProcess itself
  instead of returning from WinMain, SHUTDOWN does not fire. The first game
  run checks which way MHW exits.

### DEVICE_CREATED: the import plus an execution observer at the return site

- The game delay-loads `d3d12.dll` and imports `D3D12CreateDevice` by
  ordinal 101 (dumpbin, 2026-10-09). DXGI is not imported at all.
- AL exchanges that import slot with a forwarding stub, preserving the
  previous value. It verifies the game's call is `call [rip+disp]` through the
  slot, or `call rel32` to an intact `jmp [rip+disp]` import thunk. The stub
  saves the D3D12 arguments, reserves an execution breakpoint at the original
  return address, then tail-jumps through the original import with the game's
  return address and stack intact. The DLSS export hook sees its normal caller.
- The initial proposal to patch five return-site bytes would fail DLSS's
  `gate(0x23a4e2e)`: it hashes the complete 11,101-byte game function, after
  device creation. The execution observer keeps those bytes unchanged.
- At the owned return breakpoint, the handler restores the debug slot and
  transfers execution to a retained RX return bridge. Outside the handler,
  that bridge saves volatile registers, XMM0..5 and flags, checks HRESULT and
  the output pointer, dispatches DEVICE_CREATED once for a real device,
  restores the saved state and jumps to the untouched return site. The API's
  last error is preserved across the observation and plugin callbacks.
- The first call goes through the exe's delay-load thunk, which writes the
  resolved function into the slot. If that call produced no device (a
  capability probe with a NULL out-pointer), AL puts its stub back for the
  next call. After DEVICE_CREATED, the slot holds the resolved function,
  exactly as without AL.
- Failed calls and capability probes do not dispatch; observation is capped
  at sixteen attempts. A foreign import update is left intact and reported.
  No second inline patch is installed on a D3D12/DXGI export.

### Observer ownership and lifetime

The helper uses the documented suspended-thread
[GetThreadContext](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadcontext)
and [SetThreadContext](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadcontext)
protocol; it only operates on threads that called AL inside this process.
Occupied slots, global-enable, general-detect or single-step state are refused when they
cannot be preserved safely. The handler claims only its recorded thread,
address and debug-hit bit, and restores a slot only while it still matches
AL's reservation. Other exceptions and conflicting state are left to their
owners. It is removed after device delivery, or at normal shutdown; the worker
stops at shutdown and attempts to clear an outstanding owned observation.

Generated bridges are constructed RW and published RX, then retained for
in-flight returns. The core and host were already pinned by startup. Callback
dispatches are serialized and per-plugin phase timings are protected by the
plugin lock; the host still contains plugin faults as in PRE_MAIN.

## Cost

Nothing per frame. Observers run during startup only; the WinMain interceptor
stays on the stack until it returns. The worker waits on an event without
polling. AL logs each phase's dispatch time and each plugin's time per phase.
Measured device arming takes 43-87 us in the two timing runs; entry to
forwarding, including diagnostics, is below 0.32 ms. The earlier larger
startup gap did not reproduce; see `research/phase-timing.md` for the limits.

## Integration

`crt_entry` in the user's startup runtime calls one function after the
PRE_MAIN dispatch: `phases::arm(host, log, crt_wrapper)`, passing the CRT
wrapper address it already verified, plus the existing visible-warning callback.
Everything else lives in
`src/core/phases/`.

## Testing

- Unit tests: normal imports, owned-slot conflicts, bounded/ambiguous WinMain
  patterns, image-base/branch checks, and debug-slot reservation/restoration
  with occupied, global, general-detect and foreign-changed state.
- End to end: the fake game started by the real launcher delay-loads
  `d3d12.dll`, creates a WARP device through its import, and returns from
  WinMain. The probe sees PRE_MAIN, WIN_MAIN, DEVICE_CREATED and SHUTDOWN once
  in order, with valid payloads and the exact device the game receives. A
  capability probe and a second successful creation do not duplicate delivery.
  A test export hook checks the original caller and unchanged complete game
  function before and after WARP creation, emulating DLSS's checks. A test
  static initializer resolves the unmodified CRT call and installs an inline
  WinMain hook after AL arms, emulating SPL; AL's callback runs first.
  Test callbacks deliberately alter last error, which the game still receives
  unchanged. These are compatibility stand-ins, not MHW/SPL/DLSS confirmation.
  A second integration case creates devices on a separate setup thread and
  verifies DEVICE_CREATED uses that thread while the other phases use main.

The next MHW run should log `phase_win_main_enter`,
`phase_device_created_enter`, the device payload, and, if MHW returns normally
from WinMain, `phase_shutdown_enter`. Swapchain, first-present and game-ready
remain reserved.
