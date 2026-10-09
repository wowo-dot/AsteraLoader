# API 1.6 input layer ready (2026-10-10)

The implementation from `design-input-1.6.md` is complete. The public
header `include/al/al_plugin.h` is the exact ABI; the design now describes
the implemented semantics. This task did not launch MHW or replace MO2's
installed build. A separate build is staged under `local/stage`.

## What landed

- `al.input` 1.0: keyboard/mouse hotkeys, modifier combinations, four pad
  snapshots and pad-button hotkeys, ordered consumable window messages,
  nested capture/release, the game window and availability status.
- Snapshots come from game-thread DirectInput/XInput reads before capture
  changes the buffers. Plugins see real state; captured game calls receive
  neutral state. No system-wide polling or input-value logging.
- Keyboard/mouse methods are found through separate throwaway devices.
  Each distinct target has one process-lifetime hook. Forwarded/aliased
  XInput calls preserve the real snapshot. Installation failures make the
  layer forward again and report a warning/reason.
- The input request arranges swap-chain observation even if no plugin
  separately requests graphics. The window subclass keeps the previous
  procedure and coexists with a later SPL-style subclass. It forwards
  destruction messages and clears the dead window.
- Hotkeys run just before al.frame END callbacks, inside the frame chain.
  Capture can be released by the owner's own hotkey. Other owners suppress
  hotkeys, and those edges are not replayed. Callback faults disable and
  release that plugin; focus loss suspends capture and clears observable
  state until fresh reads. EXITING stops the input service.

Request the interface in init/PRE_MAIN. A plugin should check capture's
return and status.devices: all requested paths must be installed. Keyboard
and mouse can be requested separately when no pad path has loaded yet.
The window path is currently DX12. Standard DirectInput keyboard and mouse
formats are handled; snapshots/hotkeys follow GetDeviceState, as measured
in MHW. Steam Input on/off and any alternative pad path remain unverified.

## Validation

`dev.py check`: 54/54 pass. Warnings-as-errors Release build; C/C++ ABI
layouts pinned. The core still
imports only KERNEL32. The input tests cover pure logic, concurrency,
capture, faults, allocation refusal, a real test window, real MinHook
entries over fake devices, nested XInput, and a DLL using the public API.
Existing frame/native-hook/launcher/plugin tests are included in dev.py check.

The empty input frame makes no allocations. Six synthetic native reads
plus END also allocate nothing; a local observation was ~0.2 microseconds
per frame, including fake originals and MinHook. This is not MHW timing.

## Next run (Claude/user)

1. Disable the old input_probe: its independent hooks would conflict.
2. Build a small client of al.input: Ctrl+F7 toggles keyboard/mouse capture,
   a pad button toggles pad capture, and its UI receives mouse/WM_CHAR.
   Report aggregate counts/status only. `tests/input_plugin.cpp` illustrates
   the public calls; its DLL is a synthetic fixture, not a play-test plugin.
3. Prepare a named MO2 run with the staged AL build, that test plugin and
   the DLSS plugin. Let the user launch, test movement/camera/pad with
   capture both ways, use the toggle to close capture, alt-tab and return,
   type in the mod UI, then menu Exit Game.
4. Verify native paths reached RUNNING with the expected device bits,
   callbacks use the frame/window threads, controls recover, DLSS FG stays
   on, and EXITING/SHUTDOWN are clean. Repeat with SPL, and compare Steam
   Input on/off before making wider pad-support claims.
5. Seal logs and hashes and record the result before an in-game-tested tag.
