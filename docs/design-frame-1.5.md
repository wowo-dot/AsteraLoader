# Design: frame tick, game-thread tasks and the EXITING phase (API 1.5)

Status 2026-10-09: API, clock, frame layer and exit site implemented;
50/50 tests pass, including both native-hook fake-game paths. The real
MHW/DLSS/site-probe run remains pending (`handoff-frame-runtime-20261009.md`).
Roadmap item 13 ("run on the game thread") and the
"game exiting" event the plugin survey asked for. The sites come from runs
hooks-mo2-1/2 and astera-mo2-1/2 (their `result.json` files).

## Why

- **A frame tick.** Plugins that do per-frame game work today hook a game
  function of their own choosing, or poll from a thread of their own.
- **Work on the game thread.** Game state is consistent on the game's
  frame thread, between frames. A plugin thread reading or changing game
  objects races the game.
- **"Game exiting".** auto_food_skill and more_auto_shoutouts crash at
  nearly every exit: their threads read game memory while the game frees it.
  The game spends about 1.4 s tearing down after its last frame; a notice
  before that lets plugins stop their threads.

## What the runs showed

- The game's frame thread (not the WinMain thread; it also creates the D3D12
  device) runs a loop that calls Main:Update once per frame, then
  SteamAPI_RunCallbacks, then checks an exit flag. The loop ends when the flag
  is set; the same thread then tears the game down for about 1.4 s.
- The WinMain thread pumps window messages. On WM_QUIT it calls one routine,
  then sets the flag, then waits for the frame thread to finish. That routine
  is called from exactly two places: once by the frame thread before its loop,
  and once by the WinMain thread at quit.
- Exit Game (and any close that goes through WM_QUIT) takes that path.
  Alt+F4 does nothing in MHW (the user, 2026-10-09). A process ended from
  outside (Task Manager) gets no notice to any DLL and no teardown, so
  plugin threads cannot crash on freed memory then. (Two earlier run notes
  read such an ending as Alt+F4; corrected.)
- The DLSS mod hooks Main:Update at order -1000 so its Reflex simulation
  markers wrap other per-frame work.

## The API

### The EXITING phase

`AL_PHASE_EXITING` (7, new in 1.5; `AL_PHASE_COUNT` becomes 8). No payload.

- Raised once, when the game starts its own quit: before it sets its exit
  flag and before any teardown. Game objects are alive; frames may still
  run while EXITING is dispatched.
- Normally on the WinMain thread. If AL only notices the quit after a frame
  (see the backup below), on the game thread.
- What plugins do here: stop and join their own threads, stop touching game
  memory from other threads, flush their own files. Do not post game-thread
  tasks and expect them to run (see "Tasks at the end").
- It does not fire when the process is ended from outside or by a crash.
  Plugins save their settings when they change, not at exit.
- SHUTDOWN stays as it is: WinMain returned.

### "al.frame" 1.0

```c
#define AL_FRAME_INTERFACE "al.frame"
#define AL_FRAME_VERSION AL_MAKE_VERSION(1, 0)

typedef uint32_t al_frame_event_t;
#define AL_FRAME_BEGIN 0u  /* before the game's frame update */
#define AL_FRAME_END 1u    /* after it */

typedef struct al_frame_info_t {
    uint32_t size;
    uint32_t reserved;
    uint64_t index;  /* frames since the first one AL saw, from 0 */
    int64_t qpc;     /* QueryPerformanceCounter when this event began */
} al_frame_info_t;

typedef void(AL_CALL* al_frame_fn)(al_plugin_t* self, al_frame_event_t event,
                                   const al_frame_info_t* frame, void* user);
typedef void(AL_CALL* al_task_fn)(al_plugin_t* self, void* user);

typedef uint32_t al_frame_state_t;
#define AL_FRAME_WAITING 0u      /* no frame yet */
#define AL_FRAME_RUNNING 1u
#define AL_FRAME_STOPPED 2u      /* the game left its frame loop */
#define AL_FRAME_UNAVAILABLE 3u  /* AL cannot follow this game's frames */

typedef struct al_frame_status_t {
    uint32_t size;          /* the caller sets sizeof(al_frame_status_t) */
    al_frame_state_t state;
    uint64_t frames;        /* frames begun so far */
    uint32_t game_thread;   /* the frame thread's id; 0 before the first frame */
    uint32_t reserved;
    const char* reason;     /* why UNAVAILABLE, else ""; valid until exit */
} al_frame_status_t;

typedef struct al_frame_t {
    uint32_t size;
    uint32_t version;  /* AL_FRAME_VERSION */
    uint32_t(AL_CALL* subscribe)(al_plugin_t* self, al_frame_event_t event,
                                 al_frame_fn callback, void* user);
    al_result_t(AL_CALL* unsubscribe)(al_plugin_t* self, uint32_t handle);
    al_result_t(AL_CALL* post)(al_plugin_t* self, al_task_fn task, void* user);
    void(AL_CALL* status)(al_plugin_t* self, al_frame_status_t* status);
} al_frame_t;
```

Rules:

- **subscribe** returns a handle, or 0 with the reason in the plugin's log
  (no callback, unknown event). Allowed from `al_plugin_init` on, from any
  thread, including inside a callback (it takes effect from the next frame).
- **Where callbacks run:** on the game's frame thread, inside the game's
  frame function's chain, after the DLSS mod's markers (AL's layer has an
  order above -1000) and before plugin detours at order 0.
- **Order at BEGIN:** queued tasks first, in posting order across plugins;
  then BEGIN callbacks in load order (subscription order within a plugin).
  END callbacks in load order.
- **post** queues `task` to run once on the game thread at the next BEGIN.
  From any thread, including the game thread (then it runs next frame).
  Returns AL_ERROR when frames have stopped or are unavailable.
- **Faults:** callbacks and tasks run under the same fault guard as phase
  callbacks. A fault disables the plugin with the reason, and the frame
  goes on.
- **Cost:** the empty path still updates the clock and checks the exit
  flag, without allocation per frame. Subscriber lists are rebuilt only
  on subscribe/unsubscribe, and the task queue reuses its storage. The
  synthetic layer measurement is recorded under verification below.

### Tasks at the end

- When AL sees the game's exit flag after a frame, that was the last frame:
  AL marks frames STOPPED and runs the tasks still queued, on the game
  thread, while the game is intact. From then on post is refused, also from
  inside those tasks.
- If the flag flips in the short window between AL's check and the game's
  (about 0.6% of exits by the measured timings), frames stop without that
  last drain. Tasks still queued at SHUTDOWN are dropped, and the plugin's
  log says how many.

### Availability

The frame layer and the exit site exist for exe 421810 only, checked by
their bytes. On any other exe, or if a hook fails, `status` reports
UNAVAILABLE with the reason, `post` returns AL_ERROR, EXITING does not fire,
and the core log and status page say why. Subscribing still succeeds, so
plugins need not special-case it.

## Core side (Claude)

- `src/core/frame/clock.{hpp,cpp}`: subscriptions (copy-on-write lists,
  never freed while the process runs, so the game thread reads them without
  a lock), the task queue (a mutex-guarded vector swapped with a second one
  each frame, so storage is reused), states, per-plugin timing for the
  SHUTDOWN summary.
  - `begin_frame()` and `end_frame()` for the frame layer;
  - `last_frame()` when the frame layer sees the exit flag;
  - `set_unavailable(reason)`, `close()` at SHUTDOWN.
- `Host::raise_exiting()`: dispatches EXITING once (an atomic exchange), from
  whichever caller comes first.
- The C table for "al.frame" and the phase plumbing in `host.cpp`; the
  guarded calls in `guard.cpp`.
- Tests: the clock on its own (order, many posting threads, unsubscribe,
  faults, last-frame drain, SHUTDOWN drop, no allocation per frame), the ABI
  tables, and a fake-game test once the layer exists.

## Hook layer (implemented)

Two loader-owned hooks through the game-hook registry, installed at arming,
each checked by its bytes for exe 421810 (sites and bytes in the run
results above):

1. **Frame layer** on Main:Update, order between -1000 and 0: call
   `begin_frame()`, the original, `end_frame()`; then if the game's exit
   flag is set, `host.raise_exiting()` (no-op if already raised) and
   `last_frame()`.
2. **Exit site** on the routine the WinMain thread calls just before it sets
   the flag: when called on the WinMain thread after the first frame,
   `host.raise_exiting()` before the original.

If either cannot be installed, `clock.set_unavailable(reason)` and a visible
warning, as with the other services.

`src/core/frame/sites.cpp` selects these sites only for version 421810 and
the measured exe SHA-256, and bounds them against the loaded PE image.
`Host::game_identity()` reuses the game service's cached identity. The
registry performs the byte checks; the frame signature at target+25 also
works when another plugin already shares the entry through `al.hooks`.

`src/core/frame/layer.cpp` owns a process-lifetime client named
`AsteraLoader`. `phases::arm` registers both detours before setting the
registry installer, then arms the layer only after both hooks are active.
The WinMain interceptor records its actual thread before WIN_MAIN dispatch.
Shutdown logs the clock and registry summaries.

Main:Update forwards four pointer arguments and its integer/pointer return,
matching the probe and the DLSS frame hook. It preserves last error around
BEGIN/END callbacks and the final drain. Recursion and calls from other
threads forward without running the clock. The exit flag is a guarded
read, with no allocation or VirtualQuery per frame; a null main pointer is
allowed. Once the clock stops, the layer no longer reads game state.

If registration or activation fails, the loader's partially installed
detours are disabled and original calls remain reachable. The clock is
UNAVAILABLE and posting is refused. Installation failures raise the existing
visible warning. An unsupported identity is reported in status and the log;
its startup warning is shown when a plugin requested frames/EXITING or the
game service identified a build. Bare unknown synthetic executables using
neither feature do not get an unsolicited frame-service popup.

## Verification of the layer

- `frame_layer_normal` and `frame_layer_backup`: production phase runtime,
  native game-hook installer, host and clock in a test-only loader with a
  synthetic site provider. The production DLL has only the MHW resolver.
  The original fake game runs a frame thread and a WinMain thread; a
  separate poster queues work, and two fixtures place the loader between
  Reflex-style markers at -1000 and plugin detours at 0.
  - Three frames: BEGIN/END and tasks on the frame thread, indices in order,
    arguments/return/last error preserved, task before BEGIN.
  - The frame thread's startup call to the exit routine is ignored.
  - Normal exit: EXITING once on WinMain before the flag is set.
  - Backup: EXITING once on the frame thread after the flag is set.
  - A task posted at the last END drains before the loop returns; further
    posting is refused, also inside that final task.
  - Registry and clock summaries show the order and final counts.
- `frame_layer_identity`, `frame_layer_bad_signature`,
  `frame_layer_first_failure`, `frame_layer_second_failure`: wrong identity
  / image bounds and partial-pair failures are refused, status and warning
  are correct, and no enabled loader detour is left after failure.
- `frame_layer_cost`: 100,000 warmed calls on a synthetic frame thread,
  with no subscribers/tasks, allocate nothing. A local observation was
  about 44 ns per wrapper call versus 1.4 ns for the stand-in original.
  This excludes MinHook entry cost and is not a measurement in MHW.
- `AsteraLoader.dll` imports only KERNEL32. No actual game run or tested
  in-game tag was made for this layer yet.
