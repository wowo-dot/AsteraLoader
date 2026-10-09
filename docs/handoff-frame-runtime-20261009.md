# Native frame layer ready (API 1.5, 2026-10-09)

The frame layer and early EXITING site from `handoff-frame-layer-20261009.md`
are implemented. The build is staged separately under `local/stage`; this
work did not replace the installed MO2 build or start MHW.

## Wiring

- `src/core/frame/sites.cpp`: known MHW 421810 version + exe SHA-256;
  fixed measured RVAs, bounded against the image. Byte validation stays
  in the game-hook registry, including its original-byte view of existing
  AL chains. There is no exported-test-function fallback in the production DLL.
- `src/core/frame/layer.cpp`: core client `AsteraLoader`, Main:Update at
  order -500 (inside Reflex -1000 and outside plugins at 0), plus the
  exit routine. Both are registered before the installer activates pending
  chains; the layer runs only after both states are ACTIVE.
- `phases::arm`: prepares the sites, sets the installer, checks activation,
  logs registry/frame summaries. The actual WinMain thread is recorded by
  the WinMain interceptor. SHUTDOWN logs both summaries again.
- Frame entry: BEGIN, original, END; a set exit flag then raises EXITING
  once and drains the queue through `last_frame` on the frame thread.
- Exit entry: on WinMain after frames started, raises EXITING before the
  original; startup calls by the frame thread do not announce a quit.
- Last error, four pointer arguments and the return survive callbacks.
  Recursion / other-thread calls bypass the clock. No flag reads after STOPPED.
- Pair failures leave the clock UNAVAILABLE and disable any loader layer
  already registered; other plugins and the game's original remain in the
  chain. A hook failure shows the existing warning. Unknown unused frame
  services retain a status/log reason without a popup on every synthetic exe.

## Evidence

Release build with project warnings as errors; `dev.py check`: 50/50 pass.
The core imports only KERNEL32.

The fake-game integration uses the same production layer/runtime/installer
with a test-only resolver for its synthetic functions. Tests verify:

- BEGIN/END and queued tasks on the frame thread, in order;
- the loader between Reflex-style and plugin detours;
- an initial task posted by a third thread, before the first BEGIN;
- a last-END task drained after the flag, and post refused afterwards;
- normal EXITING once on WinMain before the flag;
- backup EXITING once on the frame thread when the early site is skipped;
- the pre-loop frame-thread call to the exit routine ignored;
- arguments, return and last error, and final summary counts.

Resolver/refusal tests cover identity, bounds, signatures and both partial
installation failures. The empty wrapper also makes no allocations in
100,000 warmed synthetic frame-thread calls. Its local timing (~44 ns
versus ~1.4 ns for the stand-in original) excludes MinHook and does not
predict MHW's full frame cost.

## Next run (Claude/user)

Prepare a named run and install the staged build with the DLSS AL plugin
and site_probe. Let the user launch through MO2 and quit with menu Exit Game.
Check `Main:Update` has `layers=mhw_dlss,AsteraLoader,site_probe`,
`frame_state=stopped` with a plausible count and game thread, and one
`phase_exiting source=exit_site` before teardown. Frame generation should
remain on. Collect and seal the run before a tested-build tag.

The known small window after the frame check can still skip the final task
drain; SHUTDOWN drops those tasks with a warning. Alt+F4's abrupt process
termination is unchanged. The real MHW sites and coexistence with DLSS
have not yet been exercised by this layer. Legacy plugins need to consume
the new lifecycle service to stop their own threads; this does not rewrite them.
