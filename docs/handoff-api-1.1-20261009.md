# API 1.1 handoff (2026-10-09)

API 1.1 from `api-roadmap.md` (items 1 to 7) is implemented and tested
outside the game: commits c0e27b7 to 3823c75, 29/29 CTest.

## What plugins get

- **`get_interface(self, name, version)`** and **`publish_interface`**:
  versioned C tables by name. A table matches with the same major version
  and at least the requested minor. Plugins publish only under their own
  name (`provider.math`); a disabled owner's tables are not handed out.
- **`cmfm.game` 1.0:** exe SHA-256, MHW's version string (Stracker's pattern,
  shared in `src/common/game_version`), exe path and folder as the game sees
  them, PE time stamp, and the graphics API `graphics_option.ini` selects
  (`DirectX12Enable`). Filled once, on first use.
- **`cmfm.scanner` 1.0:** `find(self, pattern, CMFM_SCAN_CODE | IMAGE, ...)`
  with hex (`48 8B ?? 05`) or Stracker-bit patterns, at most 1024 matches;
  `rip_target` for RIP-relative operands. Results are cached per exe hash in
  `<cmfm>\cache\scan.<hash>.txt`, re-checked in memory before use; patterns
  without a match are not cached. Each search is logged with its time.
- **`dependencies`** in the plugin info: required plugins with an optional
  minimum version (dotted numbers). Missing, refused, too old or
  uncomparable: the dependant is refused before start (cascading). A
  dependency that fails to start refuses its dependants at start.
  Dependencies always start first.
- **`data_folder`** in the init struct: `<cmfm>\data\<name>`, created for
  plugins built for 1.1 or later.
- **Timing:** the core's `plugin name=...` lines add `load_ms`, `init_ms`
  and `pre_main_ms`. The Stracker shim logs each Stracker plugin's load
  time to `stracker_compat.log` (CMFM's log only).

Compatibility: every 1.0 field keeps its offset (pinned in
`tests/abi_layout.h`); a plugin built for 1.0 (72-byte info) still loads, and
its info is never read past 1.0's size (test: `legacy10`).

## Choices made (say if you disagree)

- The info field is `dependencies`, not `requires`: `requires` is a C++20
  keyword.
- `data_folder` and `cache` sit beside CMFM's logs (`<cmfm>\data`,
  `<cmfm>\cache`), the real CMFM folder; under MO2, inside the CMFM mod.
- Versions compare as dotted numbers; a suffix after `-`, `+` or a space is
  ignored ("0.14.0-beta" equals "0.14.0").
- `stracker_compat` passes the version from `cmfm.game` to its loader.dll
  (private host struct version 2), so the pattern is searched once.
- `examples/hello` 0.2.0 uses every service, including a scan of the version
  code, so a game run shows the scanner and, on a second launch, its cache.

## Tested in the game

- **api11-mo2-1** (d746f80, two launches): everything above worked, with two
  defects. `init->data_folder` carried the `\\?\` long-path prefix, and
  `cmfm.game` searched the whole image for the version, which cost 11-13 ms on
  the PRE_MAIN thread.
- Both were fixed in 8c12961. Data folders are now plain paths. `cmfm.game`
  asks the scanner (code sections, cached per exe hash) first and keeps the
  whole-image search as a fallback.
- **api11-mo2-2** (759e686, tag v0.1.0-dev.7) confirmed both fixes:
  - the data folder had no prefix;
  - hello's `init_ms` dropped from 12.9-15.9 to 0.8 ms, with the version still
    421810;
  - the scan answered from the cache in 0.03 ms;
  - stracker_compat loaded 2 of 2 plugins;
  - the game exited with code 0.
- Dependencies (`info->dependencies`) are tested outside the game only,
  because no installed plugin declares one yet.
