# Pinned dependencies and references

- MinHook **v1.3.4**, commit `c3fcafdc10146beb5919319d0683e44e3c30d537`,
  from <https://github.com/TsudaKageyu/minhook>. `minhook/include` and
  `minhook/src` are based on that commit, with three AL changes, each marked
  `AL` in the source:
  - `hook.c`, with `AL_MINHOOK_PROCESS_THREADS`: thread enumeration first
    uses `src/core/hooks/thread_snapshot.cpp` (Windows PSS, current process,
    IDs only). Failure falls back to the original Toolhelp enumerator.
    Thread suspension, instruction-pointer relocation, patching and resume
    code are unchanged.
  - `buffer.c`, with `AL_MINHOOK_NEAR_MEMORY`: a new trampoline block comes
    first from AL's reservation next to the game
    (`src/core/hooks/near_memory.cpp`). When it has no page within reach,
    MinHook's own search runs as before. Blocks from the reservation go back
    to it instead of being released (`decisions.md` item 19).
  - `trampoline.c`: when an instruction moved into a trampoline has a
    RIP-relative operand whose new displacement no longer fits in 32 bits,
    the hook is refused (`MH_ERROR_UNSUPPORTED_FUNCTION`) instead of written
    with a truncated one.

  No public MinHook API was changed. All other vendored files are unchanged.
  The x64 build uses
  `buffer.c`, `hook.c`, `trampoline.c` and `hde64.c`. Its BSD-2 notices,
  including HDE notices, are in `minhook/LICENSE.txt` and installed with AL.
  The build requires no download.
- SharpPluginLoader **7be239303f90af7f7356a3695adc07cb6b0707ef**,
  <https://github.com/Fexty12573/SharpPluginLoader>, is a source reference for
  the MSVC common-main signature in `entry_signature.cpp`. Its MIT notice
  is retained in `notices/SharpPluginLoader-MIT.txt` and installed with AL.
  No SPL implementation or binary is linked into AL.

AL does not copy SPL's security-cookie reset. It verifies the normal CRT
wrapper and lets the original cookie initializer run unchanged.
- Stracker's Loader **977030295d4635d815259eaacf77969215d2b7a4**,
  <https://github.com/Strackeror/MHW-QuestLoader>, MIT, Copyright (c) 2020
  Remi Lavergne. `plugins/stracker_compat/loader_abi.h` reproduces its
  `loader.h` declarations exactly (the ABI Stracker plugins import), and
  `loader.cpp` follows its start-up, log texts and version pattern. Its `LOG`
  class derives from BSD-2-Clause code, Copyright (c) 2018 Philipp
  Kraehenbuehl. Both notices are in `notices/StrackerLoader-MIT.txt` and
  `notices/StrackerLoader-log-BSD-2.txt` and installed with AL. No Stracker
  binary is linked or shipped.
