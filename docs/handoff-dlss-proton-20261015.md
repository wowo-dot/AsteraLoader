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

## For the tester

- Build: `local\stage\AsteraLoader-0.2.3-dev-f508df4.zip` in the Astera
  Loader repository (SHA-256
  `90cb4c53caca989a745423567e360f2cecf1d84befc8deed4253f69cdd40cd0c`).
  `AsteraLoader.dll` is `c545fbda…c072`. It replaces the Astera Loader
  files in the game folder or in the bundle.
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
