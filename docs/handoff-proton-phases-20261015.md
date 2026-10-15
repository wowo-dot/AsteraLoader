# To the user: the WinMain breakpoint under Proton (2026-10-15)

**Done as Change A in 9ece667**, at the user's request: Codex isn't
available, so Claude wrote it. It differs from the spec below in two ways:
- **Which hits are accepted:** one at the armed address on the armed thread
  whose Dr6 shows no breakpoint at all. A Dr6 showing another slot or a
  single step is still declined (`slot_hit` in `debug_slot.hpp`).
- **If the worker can't clear the slot:** there's no resume-flag
  pass-through. The hit line says `cleared=failed`, and the game would end
  at the next hit as before.

The phase tests also run with a test core (`AL_TEST_HIT_UNREPORTED`) that
takes every hit as Wine reports it.

Under Proton, build eb35367 gets through start-up and PRE_MAIN. Then the
game dies at WinMain with an unhandled `EXCEPTION_SINGLE_STEP`. The phase
observers' debug-register mechanics are yours (memory note on ownership),
so this is a spec, not a change. Evidence: `local/runs/proton-tester-4`.

## What the logs show

- **Core log:**
  - All plugins load at PRE_MAIN and all game hooks become active.
  - `startup_hook_timing operation=win_main_arm ... armed=1 set_context_us=9954`,
    then `phase_win_main_armed`, then `phase_device_created_armed`. That's
    the last line.
- **Launcher log:** `game_exit_code=2147483652`, which is `0x80000004`.
- **The handler declined the first hit.** The core log is written with one
  `WriteFile` per line, and `win_main`'s first line
  (`phase_win_main_observer_removed`) is missing. So `device_exception`
  (`src/core/phases/runtime.cpp`) never redirected to the wrapper; it
  returned `EXCEPTION_CONTINUE_SEARCH`.

## Why, as far as Wine's source shows

Checked in Proton's Wine (`ValveSoftware/wine` proton_9.0,
`dlls/ntdll/unix/signal_x86_64.c`):

- **Where the exception context's debug registers come from:** a hardware
  breakpoint arrives as a trap that Wine turns into `EXCEPTION_SINGLE_STEP`.
  Wine fills `Dr0` to `Dr7` from its own per-thread copy
  (`amd64_thread_data()->dr*`), not from the CPU.
- **When Wine asks the kernel instead:** for a single step, Wine refreshes
  through `NtGetContextThread(self)`. That calls the wineserver, which reads
  the real registers with ptrace, only when the copy's `Dr7` already shows a
  breakpoint. Otherwise it keeps the copy.
- **Setting the thread's own debug registers:** `NtSetContextThread(self)`
  goes to the server only when the values differ from the copy.

**Not confirmed:**
- Whether `SetThreadContext` from another thread updates the target
  thread's copy. AL's context worker arms the slot that way. If the copy
  stays at zero, the handler sees `Dr6 = Dr7 = 0` and its checks fail, which
  matches the log.
- Whether clearing `Dr7` in an exception context reaches the CPU when
  execution continues.

## Change A (recommended): accept the hit without the context's registers

1. **`device_exception`:** keep today's gate: observer active, the
   observer's thread, `EXCEPTION_SINGLE_STEP`, and `ExceptionAddress` equal
   to `pending.caller`. Then:
   - **The context shows the slot** (today's `Dr6`/`Dr7` checks pass): as
     today. Release the slot in the context, set `Rip`, continue. Record
     `cleared=context`.
   - **It doesn't** (Wine): set `Rip` to `pending.resume` and continue,
     leaving the debug registers alone. Record `cleared=worker`, which
     means the slot is still armed in hardware.
   - **Either way,** keep what the handler saw (`ContextFlags`, `Dr6`, `Dr7`,
     the slot's address register) in plain globals for later logging.
2. **`win_main` with `cleared=worker`:** before calling `g->win_original`,
   have the context worker release the slot from outside the thread. That
   is the worker's existing `release_debug_slot` +
   `SetThreadContext(CONTEXT_DEBUG_REGISTERS)` path, which already worked on
   Proton for arming. Wait for it to finish.
   - If the release fails, the original WinMain would hit the breakpoint
     again. The handler must then let that hit through instead of declining,
     for example by setting the resume flag (`EFlags` bit 16) for exactly
     that address and thread.
   - In that case, also log it and show a start-up warning.
3. **Log from the wrapper:**
   `phase_win_main_hit context_flags=0x... dr6=0x... dr7=0x... dr_slot=0x... cleared=context|worker`,
   so the next Proton log shows exactly what Wine reports.
4. **DEVICE_CREATED:** the same for its return-address slot, which goes
   through the same handler and worker.

## Change B (fallback): no debug-register observers under Wine

- **What:** if ntdll exports `wine_get_version`, don't arm WinMain or the
  device observer. Report `win_main`, `device_created` and `shutdown` as
  unavailable ("not supported under Wine/Proton yet") and keep the game
  running.
- **What's lost there:**
  - WIN_MAIN, DEVICE_CREATED and SHUTDOWN for plugins;
  - EXITING from the exit site, because `frame::Layer::exiting` needs the
    WinMain thread id that `win_main` provides.

  GAME_READY, frames, input, hooks and PRE_MAIN plugins are unaffected.
- **When:** this unblocks the tester at once. A is better if it works.

## Testing

- **Windows:** every phase test as today. Core logs should say
  `cleared=context`, and nothing else changes.
- **Unit test:** if the accept decision moves into a pure helper (next to
  `debug_slot.hpp`), a crafted `CONTEXT` with `Dr6 = Dr7 = 0` can test it. I
  can write that test once the helper exists.
- **Proton** (the tester): expect `phase_win_main_hit ... cleared=worker`,
  then `phase_win_main_observer_removed`, DEVICE_CREATED, GAME_READY and
  exit code 0.
