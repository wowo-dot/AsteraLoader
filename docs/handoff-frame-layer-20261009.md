# Handoff: the frame layer and the exit site (API 1.5, 2026-10-09)

API 1.5 is in place on the core side: the `AL_PHASE_EXITING` phase, the
`"al.frame"` interface, the clock behind it, and unit tests (43/43). Design:
`design-frame-1.5.md`. What remains is the hook-layer part: two of the
loader's own hooks through the game-hook registry, calling into the clock
and the host. Nothing calls `begin_frame` yet, so in the game "al.frame"
stays WAITING and EXITING never fires until these land.

## Entry points (core side, done)

- `host.frame_clock()` (`src/core/frame/clock.hpp`), on the game's frame
  thread:
  - `begin_frame()` before the game's frame function, `end_frame()` after;
  - `last_frame()` when the game's exit flag is set after a frame: drains
    queued tasks, then STOPPED;
  - `set_unavailable(reason)` if a hook cannot be installed (and a visible
    warning, as for the other services);
  - `summary()`: log it with the game-hook summary at SHUTDOWN.
- `host.raise_exiting()`: dispatches EXITING once; later calls return false
  at once. Any thread.
- `dispatch(SHUTDOWN)` already closes the task queue.

## The two hooks

Both through `host.game_hooks()`, registered at arming so they install when
the hook layer is set. Their registry client is the loader itself (a
`game_hooks::Client` named "AsteraLoader", logging to the core log). Only
for exe 421810; on another exe, `set_unavailable` with the reason.

1. **Frame layer on Main:Update** (exe+0x1af2b60). Same check as
   site_probe: the pattern `48 8B 8B 88 F8 0A 00 48 8B 01 FF 50 48` at the
   target + 25. Order between -1000 and 0 (for example -500): inside the
   DLSS mod's Reflex markers (-1000), before plugin detours at 0. Around the
   original: `begin_frame()`, the original, `end_frame()`. Then, if the
   game's exit flag (byte `[*(exe+0x51C5320) + 0x2BE8]`, pointer checked for
   null) is set: `raise_exiting()` and `last_frame()`.
2. **Exit site** (exe+0x22e5c60): the routine the WinMain thread's message
   loop calls right before it sets the exit flag. Its first bytes:
   `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 81 EC 20 01 00 00`.
   It is also called once by the frame thread before its loop, so only a
   call on the WinMain thread after the first frame (`status.frames > 0`)
   means the game is quitting: then `raise_exiting()` before the original.
   It takes the main object in rcx and returns nothing.

The run results with the evidence: hooks-mo2-2, astera-mo2-1, astera-mo2-2
and dlss-astera-1 under `local/runs` (`result.json` in each).

## Tests to add

- The fake game: a frame thread calling a stand-in frame function in a loop
  until a flag is set, and a "WinMain" thread that calls a stand-in exit
  routine, then sets the flag. A fixture plugin subscribes BEGIN/END, posts
  tasks from another thread and records EXITING. Check: callbacks on the
  frame thread, tasks drained at the last frame, EXITING once and before
  the flag, and the backup path when the exit routine is skipped.
- `AsteraLoader.dll` still imports only KERNEL32.

## In-game check afterwards

One run with the DLSS mod and site_probe: the SHUTDOWN summary shows
Main:Update with `layers=mhw_dlss,AsteraLoader,site_probe`, `frame_state`
lines with the frame count, and `phase_exiting` before teardown; DLSS frame
generation still on.
