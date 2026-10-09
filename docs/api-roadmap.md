# AL API roadmap (2026-10-09)

What the plugin API should offer beyond 1.0, in order. Agreed with the user on
2026-10-09. Each step names who builds it: "Claude" for work that needs no
game or system hooks, "user" for hooks and the launcher's injection code.

## Where we are (2026-10-09)

The loader was called CMFM until 2026-10-09; CMFM is now its menu plugin
(decisions.md item 11). Finishing AL v1 therefore does not include the
menu: CMFM is a separate track, the first large plugin built on AL.

| API | What | Confirmed in the game |
| --- | --- | --- |
| 1.0 | launcher, plugin loading, load order, refusals, per-plugin logs, fault containment, Stracker compatibility | pre-main-3, plugin-mo2-1, stracker-mo2-1 to -3 |
| 1.1 | interfaces by name, game identity, timing, dependencies, data folders, plugin interfaces, cached scanner | api11-mo2-2 (v0.1.0-dev.7) |
| 1.2 | phases WIN_MAIN, DEVICE_CREATED, SHUTDOWN | phases-mo2-1 (dev.9) |
| 1.3 | "al.d3d12" shared graphics hooks; SWAPCHAIN_CREATED, FIRST_PRESENT | graphics-mo2-1 (dev.10), graphics-dlss-1 |
| 1.4 | "al.hooks" shared game-function hooks | hooks-mo2-1 (dev.12), dlss-astera-1 (dev.14) |
| — | crash handler | crash-mo2-1/2 (dev.11) |
| — | rename to Astera Loader | astera-mo2-1 (dev.13), asteraloader-mo2-1 |
| 1.5 | "al.frame" and AL_PHASE_EXITING | frame-input-1 (dev.15) |
| 1.6 | "al.input": hotkeys, key and pad state, window messages, capture | input-check-3 (dev.16) |
| 1.7 | AL_PHASE_GAME_READY: the title screen started | tested outside the game; the run is next (design-game-ready-1.7.md) |

The DLSS mod 0.15.0 runs on 1.3 and 1.4 with frame generation on.

## The path to AL v1

In order. From 1.5's frame layer on, Claude builds these end to end,
hooks included (the user's call, 2026-10-09); the user runs the game.

1. ~~**1.5 in the game**~~: done (frame-input-1, dev.15).
2. ~~**Input**~~ (item 12 below): done (input-check-3, dev.16). Capture
   works at the game's own keyboard, mouse and pad functions
   (design-input-1.6.md), which the Steam overlay does not wrap.
3. **GAME_READY** (item 8): when the title screen starts (aTitle's start,
   found by reversing; design-game-ready-1.7.md). Built; one run.
4. **Release:** freeze ABI 1.x (reviewed for 1.5: `abi-freeze-review.md`),
   a plugin author guide (drafted for 1.5: `plugin-guide.md`; add input when
   1.6 lands), one archive, a clean-machine install test (both in
   `release-checklist.md`).
5. **The deferred test matrix** (decisions.md item 9): SPL force-loaded, Steam
   launch options, a manual install outside MO2, overlay off, and a stability
   pass (loading screens, cutscenes, alt-tab, resize, full screen) with frame
   generation on. These are the user's runs.

After v1, unless a plugin needs it sooner: **per-save plugin data** (item 9;
no client yet). It may not be needed at all: SKSE's co-save matters because
Skyrim players juggle many saves, while MHW players mostly keep one save
for the whole game (the user, 2026-10-09). Separate tracks: **CMFM** (settings, status pages, popups,
drawn through al.d3d12; publishes its own interface, so the core API does
not grow for it) and **the MHW SDK** (names, structures and gameplay events
on top of al.hooks).

## Principles

- C ABI only; additive changes within a major version. A plugin built for a
  newer minor version is refused with a reason (already enforced).
- New services come as separate, versioned C tables a plugin asks for by
  name (`get_interface`), as SKSE does, instead of one ever-growing table.
- Nothing costs time per frame when no plugin uses it.
- Fail closed and visibly: a failing service or plugin is reported in the
  status, never fatal to the game.
- The core stays small and stable. Reversed game structures and signature
  tables ship separately, as an SDK that can change faster.

## API 1.1: services that need no hooks (Claude)

Status 2026-10-09: done. Implemented, tested outside the game and confirmed
in the game in api11-mo2-2, tag v0.1.0-dev.7 (handoff-api-1.1-20261009.md).
Dependencies are tested outside the game only.

In this order; the first four are small.

1. **`get_interface(name, version)`** in the API table: the way every later
   service is requested. Unknown name or newer version: NULL.
2. **Game identity:** exe SHA-256, PE timestamp, the game version string
   (the Stracker shim already finds "421810"), DX11 or DX12. Lets a plugin
   refuse an unknown game version without scanning for it.
3. **Startup cost per plugin:** AL times each plugin's load, init and
   phase calls and writes them to the report and the core log. Today they
   are measured by hand from the watcher (QuestLoader: about 462 ms).
4. **Dependencies:** `requires` with minimum versions. A missing or too old
   dependency refuses the plugin with a reason. Today there is only ordering.
5. **A writable data folder per plugin**, separate from its install folder,
   in one known place under MO2.
6. **Plugin-to-plugin interfaces:** a plugin publishes a named, versioned C
   table (`mhw_dlss.status.v1`); others look it up through
   `get_interface`. No linking against each other's DLLs.
7. **Shared signature scanner with a cache.** The largest measured win:
   MonsterLoader and QuestLoader spent about 610 ms of start-up scanning the
   84 MB image, each on its own, several times (stracker-mo2-1).
   - One pass for all patterns registered so far, code sections by default.
   - Results cached per exe hash, so later launches skip the scan.
   - Stracker's bit notation and the usual `48 8B ?? ??` form; helpers for
     RIP-relative operands.
   - Helps AL-native and ported plugins. Existing Stracker plugins keep
     their own scanner.

## API 1.2: services that need hooks (user)

8. **Remaining phases:** WIN_MAIN, DEVICE_CREATED, SWAPCHAIN_CREATED,
   FIRST_PRESENT, GAME_READY, and a shutdown notice so plugins can save.
   Status 2026-10-09: WIN_MAIN, DEVICE_CREATED and SHUTDOWN done and
   confirmed in the game (phases-mo2-1, phases-timing-1/2, tag
   v0.1.0-dev.9). SWAPCHAIN_CREATED and FIRST_PRESENT come with graphics
   coordination (11) and are confirmed in the game (graphics-mo2-1, tag
   v0.1.0-dev.10). GAME_READY needs a game-side signal.
9. **Per-save plugin data** (SKSE's co-save): each plugin stores its own
   data per save slot, kept in step with saving, loading and deleting.
   Needs hooks into MHW's save and load. Moved up because it is what made
   SKSE indispensable for gameplay mods.
10. **Shared hook service:** plugins ask AL to hook a function; one hook
    engine in the process, a defined order when two plugins hook the same
    function, clean removal. Extends the "one hook per entry point" rule from
    D3D12/DXGI to game functions. Today every Stracker plugin bundles its
    own MinHook.
    Status 2026-10-09: API 1.4 "al.hooks" designed and implemented on the
    core side and native installer, tested with real patches in the fake
    game (design-game-hooks-1.4.md). Confirmed in MHW on Main:Update
    (hooks-mo2-1). First plugin client: the DLSS mod 0.15.0 moved its three
    live frame hooks onto it after a second hook on Main:Update had turned
    its frame generation off; it now shares Main:Update with site_probe,
    frame generation on (dlss-astera-1).
11. **Graphics coordination** (the DLSS mod's main need,
    requirements-dlss-mod.md §3): one owner per D3D12/DXGI entry point,
    ordered Present and resize callbacks, the presentation-owner role,
    device, queue and swap-chain access.
    Status 2026-10-09: API 1.3 "al.d3d12" implemented (core side and
    native hook layer), tested on WARP/DXGI stand-ins and confirmed in the
    game with the DLSS mod off (graphics-mo2-1, tag v0.1.0-dev.10;
    design-d3d12-1.3.md). The DLSS mod 0.14.0 runs on it with frame
    generation on (graphics-dlss-1): its first real client.
12. **Input:** hotkeys and input capture. Status 2026-10-10: API 1.6
    "al.input" captures at the game's own input functions through
    al.hooks; confirmed in MHW with the Steam overlay on (input-check-3,
    tag v0.1.0-dev.16). XInput pads only; DirectInput pads and Steam
    Input on/off are unverified.
13. **Run on the game thread:** queue work for a safe point each frame;
    nothing runs when the queue is empty.
    Site found in hooks-mo2-1: Main:Update (exe+0x1af2b60) runs once per
    frame (9710 calls for 9708 presents), always on the game's main thread.
    That is the thread that creates the D3D12 device, not the WinMain thread.
    The "game exiting" event still needs its site: frames stopped 1.5 s
    before WinMain returned, and a Stracker plugin crashed on freed memory
    43 ms before it.
    Status 2026-10-09: API 1.5 designed (design-frame-1.5.md): "al.frame"
    1.0 (BEGIN/END callbacks on the game thread, post() from any thread) and
    the AL_PHASE_EXITING phase, from the exit site astera-mo2-1/2 found.
    Core side implemented and unit-tested; the frame layer and exit site
    wait for the hook layer (handoff-frame-layer-20261009.md).

## CMFM Menu milestone

- Typed settings; the plugin's own file stays the source of truth.
- Warning popups with "don't show again".
- A status page per plugin, showing the report AL already builds.
- A crash handler that names the faulting plugin and chains to existing
  handlers; opt-in minidumps; a "collect logs" bundle.
  Status 2026-10-09: the crash handler is done ahead of the menu, tested
  outside the game (design-crash-handler.md); the "collect logs" bundle
  waits for the menu.

## Later

- Game events: quest start and end, area changes, loading screens. Needs
  reversing the game; the most valuable layer for gameplay mods.
- Optional ImGui pages for plugins built against the same pinned ImGui.
- The SDK package: the public header, reversed MHW structures, signature
  tables.
- Entries inside the game's own menus (decisions.md item 2).

## Out of scope

- A managed runtime (SharpPluginLoader's role).
- Hot reload of native plugins.
- Any per-frame cost when nobody uses a feature.

## Lessons from SKSE (from memory)

- Same model: a launcher starts the game paused and loads the framework;
  plugins export a version query and a load entry, and ask for versioned
  interfaces by ID (messaging, serialization, task, trampoline, scripting,
  UI).
- Its most valuable services are startup and game messages, the per-save
  co-save and "run on the main thread"; this roadmap has all three.
- The richness lives outside SKSE: Address Library (stable IDs across game
  versions) and CommonLibSSE (reversed classes, hook helpers). AL follows
  that split with a separate SDK.
- MHW is at its final version (one exe hash), so signatures plus a cache
  replace an address library. It has no scripting language, and its UI is
  not Flash, so those two SKSE interfaces have no counterpart.
