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

As with earlier releases, Windows Defender may remove `AsteraLoader.exe`,
because the launcher starts the game paused and loads its DLL into it. This
release's launcher is also flagged by Microsoft's machine-learning file
scan, as `Trojan:Win32/Wacatac.B!ml`, so Defender may remove it right after
download or unpacking.
- That is a false positive: the launcher's code is the same as in 0.2.3,
  which Microsoft does not flag. Only its version information differs.
- An exclusion for the Astera Loader mod folder covers the new files too;
  the README shows how, and how to restore a removed file.

VirusTotal for this release's files (2026-10-23):

| File | SHA-256 | Result |
|---|---|---|
| `AsteraLoader.exe` | `5e4b52e5e50460403362d589d3156c4bac1dc1c924675b1f541f185797f2782a` | 2/71 (Microsoft, Trapmine) |
| `AsteraLoader\AsteraLoader.dll` | `7006173f2e71370d6fb25aec80a2da308425bfe858575dc4f369516d352da6e7` | 0/71 |
| `stracker_compat.dll` | `89cc277f465abcfa20b6beb03795308abdb7746ce82f12daa3a2be5aa0b14e2d` | 1/71 (Cynet) |
| `loader.dll` | `f62ea73643d65a022ffe1a5a6d1f1a6af5b954f99f718b0c35416e32080ed2d0` | 0/71 |

Archive `AsteraLoader-0.2.4.zip`: SHA-256
`bc11c1f791dbd9d180df9b00ffe525b997f69fed810e2dc12835b19b49b217ce`. Plugin
kit `AsteraLoader-plugin-kit-0.2.4.zip`: SHA-256
`bb19b23419a537b1fd504406a701ca62b488db915191a5355ecc625505cb8c41`.

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
- **This archive**, in the same setup:
  - a normal start through Exit Game;
  - a start with Stracker's Loader left switched on in Mod Organizer 2, where the
    warning named it and said what to do.
- **Linux (Proton):** not tested again for this release. If the reservation
  can't be made, Astera Loader logs that and works as in 0.2.3.

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
