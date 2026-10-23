# Astera Loader 0.2.4

Hooks that plugins add while the game is loading no longer fail for lack of
memory near the game. Plugin API 1.8 as in 0.2.0; plugins built for any
earlier version keep working.

Two downloads:

- `AsteraLoader-0.2.4.zip`: the loader. Install it as an MO2 mod over
  0.2.3, or as a new one (see the README).
- `AsteraLoader-plugin-kit-0.2.4.zip`: for plugin authors. The C header,
  the plugin guide and a small example plugin that builds with CMake.

## Before you install: Windows Defender

As with earlier releases, Windows Defender may remove `AsteraLoader.exe` as
`Behavior:Win32/DefenseEvasion.A!ml`, because the launcher starts the game
paused and loads its DLL into it. An exclusion for the Astera Loader mod
folder covers the new files too; the README shows how.

VirusTotal for this release's files: to be added.

## Fixed

- **A plugin's hooks could fail with `MH_ERROR_MEMORY_ALLOC`** when it
  added them a few seconds after start. Reported through the DLSS mod:
  with cpuopt's parallel hit detection on as well, DLSS showed "Degraded"
  in 5 of 8 starts. Any setup whose plugins hook the game often enough
  could hit it.
  - Each hook needs a few bytes of code within 1 GB of the game. Astera
    Loader looked for that memory only when a hook was made. While the game
    loads, its own memory takes that space, so a late hook could find none.
  - Astera Loader now reserves 1 MB right next to the game when it starts,
    before the game uses the space. That is room for about 16,000 hooks and
    redirected calls (al.patch). Nothing is used until a hook needs it.
- A hook on a function whose first instructions refer to an address too far
  from Astera Loader's code is now refused, instead of installed with a
  wrong address. No known plugin hooks such a function.

For bug reports, the core log has a `near_memory` line after start-up and
at exit: how much of the reservation is used, and how much free memory is
left near the game (`free_64kb_within_reach`). A refused hook shows the same
numbers.

## Changed

- The start-up warning about plugins now names each plugin that isn't fully
  running, says whether it runs with some features off or not at all, and
  gives its reason. Before, it only gave a count. It also waits for every
  plugin's final state at the end of start-up, so it no longer misses a
  plugin that reports a problem late.

## Tested

With MHW 421810:

- **Windows, under Mod Organizer 2,** a development build of this release
  in a setup with 80 game hooks, Astera Loader's own and five plugins'
  (CMFM 0.2.0, cpuopt 0.18.0 with parallel hit detection, the DLSS mod
  0.31.0 and two more), and 10 Stracker plugins. Four starts through Exit Game. Every hook went in, and
  the hooks added late took their room from the reservation. With 0.2.3,
  one of two starts lost a DLSS hook to `MH_ERROR_MEMORY_ALLOC` the day
  before.
- The release archive: to be added.

Outside the game, the test suite fills the memory around a fake game the
way the game does: without the reservation the 64th hook fails with
`MH_ERROR_MEMORY_ALLOC`, with it 300 hooks and a redirected call fit.

## Known issues

As in 0.2.3, with one now explained: the crash while exiting, seen under
Proton, comes from Stracker's QuestLoader with custom quests installed, on
Windows too. The README's Known issues section has the details.

## Licence

MIT. Third-party notices (MinHook, Stracker's Loader, SharpPluginLoader) are
in the `licences` folder. The changes to the included MinHook are listed in
`third_party/README.md`.
