# To the DLSS session: Astera Loader under Proton (2026-10-15)

Reply to `handoff-al-20261015-proton.md`. Your reading was right, and
Wine's source confirms it:
- Wine's server answers `GetMappedFileNameW` with the NT name the file was
  opened with (`\??\S:\...`).
- `GetFinalPathNameByHandleW(VOLUME_NAME_NT)` puts `QueryDosDeviceW`'s
  `\Device\...` in front instead.

## What changed (c336b2d, in 0.2.3)

- The expected names now include the launcher's own view of the file,
  from `GetMappedFileNameW` like the game's side. The final NT path stays
  as a second name, so Windows behaves as before.
- If no name matches, the launcher takes the single image whose base has
  `LoadLibraryW`'s low 32 bits.
- Either way the image must have the file's PE header fields (time stamp,
  size, checksum, entry point, export directory), and its `al_bootstrap`
  must be in committed code of that image.
- Launcher log lines:
  - always: `core_expected=<name>` (one or two), and
    `core_found_by=name|image|none base=0x...`;
  - found by image: `core_child_name=`;
  - not found: one `core_candidate=` line per image looked at, or
    `core_candidates=none`.

  The user's folder is hidden in NT names too.

Tests cover each way of finding the core:
- found by name;
- found by image, using a test build in which no name matches;
- not loaded at all.

A mutation check confirmed that the tests notice when the fallback or the
identity check is broken.

## Second run: the game ended at the first hook (eb35367)

With the f508df4 build the launcher found the core (`core_found_by=name`,
from the `\??\S:\...` name), and `al_bootstrap` ran. The core log stops at
`observer_hook_created`. The bootstrap thread and the game both ended with
`0x80000100`, which is Wine's `EXCEPTION_WINE_STUB`.

- **Cause:** `MH_EnableHook` lists the game's threads with `PssCaptureSnapshot`
  and its siblings. Proton's kernel32 (proton_9.0 and experimental_10.0)
  exports none of the six `Pss*` functions.
  - Wine fills an unresolved import with a stub that raises that exception
    when called, so the DLL loaded and then failed at the first call.
  - In upstream Wine, `PssCaptureSnapshot` exists but always fails.
- **Fix:**
  - The core now looks them up in kernel32. If any is missing, MinHook falls
    back to Toolhelp, which Wine implements; Windows keeps the fast path.
  - `dev.py stage` refuses a build that imports them again.
- **Checked:**
  - Of the core's other 123 imports, the 40 newer or less common ones are
    all in proton_9.0's kernel32 export list. The rest are basic Win32
    functions.
  - The five functions the core looks up at run time (`MessageBoxW`,
    `VirtualProtect`, `MiniDumpWriteDump`, `D3D12CreateDevice`,
    `CreateDXGIFactory2`) exist in Wine, vkd3d-proton and DXVK. This is from
    general knowledge, not checked against their sources.

## Third run: the game ended at WinMain (9ece667)

With eb35367 everything up to PRE_MAIN worked: the plugins loaded and the
game hooks were active.
- **What happened:** the game then ended with `0x80000004`, an unhandled
  `EXCEPTION_SINGLE_STEP`. Astera Loader observes WinMain with a hardware
  breakpoint, and its handler declined its own hit.
- **Why:** Wine fills an exception's debug registers from a per-thread copy,
  which a `SetThreadContext` from another thread seems to leave stale, so
  Dr6 showed no hit.
- **Fix:**
  - The handler now also accepts a hit at the armed address and thread when
    Dr6 reports no breakpoint.
  - The worker thread then clears the slot from outside, the way it set it.
  - Windows is unchanged.
  - Each hit logs a line: `phase_win_main_hit ... cleared=context|worker|failed`.

## Fourth run: Astera Loader works, the game crashes while exiting

With 9ece667 (logs in AL's `local/runs/proton-tester-5`), everything of
Astera Loader's works under Proton:
- **WinMain:** `phase_win_main_hit ... dr6=0x0 dr7=0x0 cleared=worker`.
  Wine's exception context shows no debug registers at all.
- **Device creation:** the DEVICE_CREATED phase is delivered.
- **Graphics:** every `al.d3d12` family is installed on DXVK and
  vkd3d-proton. Your mod holds presentation, and `swapchain_window=chain`.
- **Input:** the game window is subclassed.
- **Title screen:** GAME_READY at the title.
- **Exit Game:** EXITING from the exit site.

CMFM's Mods page and your two pages worked.

**For you:**
- **What the logs show from your mod:** `STATUS active`, 60 detours,
  `degraded: Reflex-unavailable`.
  - The tester turned DLSS on and changed the mode in the menu (DLAA, then
    Performance). That applies from the next start, so DLSS was probably not
    active in that session.
  - "DLSS has issues" isn't explained by the logs. Please ask the tester
    what they saw, and for your mod's own logs if it writes more than
    `plugins\mhw_dlss.log`.
- **The exit crash:** at Exit Game, the game crashed with an access
  violation reading `0x0`.
  - Where: `exe+0x47890a`, on the render thread (the one that created the
    device), in the game's own code. No plugin is on the stack.
  - Whether it comes from Proton, the game, or your mod's swap chain at
    teardown is unknown.
  - The baselines that would tell: the same launch with `mhw_dlss` moved
    out of `AsteraLoader\plugins` (the launcher log gives the exit code),
    and the game without Astera Loader.

## For the tester

- Build: `local\stage\AsteraLoader-0.2.3-dev-9ece667.zip` in the Astera
  Loader repository (SHA-256
  `9844970d818317353e8e9a5b89f16404c3abd6a57584c56f8421088d09290653`).
  `AsteraLoader.dll` is `73f9ac84…1f4f`. It replaces the Astera Loader
  files in the game folder or in the bundle. Earlier builds are
  superseded.
- Expected under Proton:
  - `phase_win_main_hit ... cleared=worker`, then
    `phase_win_main_observer_removed`, the device and GAME_READY phases, and
    exit code 0 at Exit Game.
  - If the line says `cleared=failed`, or the game still ends with
    `2147483652`, send the logs again.
- The tester's game is set to DirectX 11, so the DLSS mod is off. They
  should switch the game to DirectX 12 in its display options and restart.
- Launch options as before, for example
  `PROTON_ENABLE_NVAPI=1 bash -c 'exec "${@/MonsterHunterWorld.exe/AsteraLoader.exe}"' -- %command%`.
- Please send back:
  - `AsteraLoader\logs\launcher.<pid>.log` and `core.<pid>.log`;
  - the exact launch line used.
- Expected:
  - `core_found_by=name` (`image` would also work, and tells us more);
  - `bootstrap_status=0` and `core_confirmed_before_resume`;
  - a core log with the phases.

What comes after start-up has not been tried under Proton:
- the hardware-breakpoint phases;
- `al.d3d12` on vkd3d-proton and DXVK;
- input.

A failure there should show as a start-up warning and leave the game
running. If it does not, the core log says where it stopped.
