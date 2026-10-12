# Handoff: al.patch 1.0 (API 1.8), 2026-10-12

For the next AL session. The previous session ran out of road: a safety
check stopped one of its replies, then the same check stopped the
compaction of the session.

## State

- **Design:** `docs/design-patch-1.8.md` (committed, bc706b1). The user
  chose the general version and said "I will follow your lead".
- **Code:** written and building cleanly with warnings as errors.
  Uncommitted, in the working tree:
  - the header: the `al.patch` section and API 1.8;
  - `src/core/image/` (layout, claims, footprint);
  - the hook registry's claims and pattern overlay;
  - `src/core/patch/` (registry and native writer);
  - the crash handler's list of changes;
  - the host glue and `get_interface("al.patch")`;
  - the phases runtime (writer set at PRE_MAIN end, summary lines);
  - `CMakeLists.txt`; the ABI test's 1.8 layout.
- **Tests:** the existing suite was at 58/60 before the version pins
  (`tests/test_plugins.cpp`, `tests/test_abi.c`) were updated to 1.8. It
  has not been re-run since.

## Next

1. Build and run the suite (`tools/dev.py check`). Commit what passes.
2. al.patch's own tests, as listed in the design doc's "Tests" section.
   Registry tests with a stand-in writer come first; they need no real
   writes.
3. Docs: the plugin guide, an ABI review entry for 1.8, and the design
   doc's status.
4. The CMFM handoff (use al.patch for its Start menu values), then the
   in-game run and AL 0.2.0.

## Careful

The safety check stopped this work right after the native writer
(`src/core/patch/writer.cpp`). See the memory note "What trips the
safety check". Keep replies about that file short. If a reply is stopped
again, offer the user a spec for that part's tests instead.
