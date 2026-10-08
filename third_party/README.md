# Pinned dependencies and references

- MinHook **v1.3.4**, commit `c3fcafdc10146beb5919319d0683e44e3c30d537`,
  from <https://github.com/TsudaKageyu/minhook>. `minhook/include` and
  `minhook/src` are based on that commit. `hook.c` has one CMFM adapter patch:
  with `CMFM_MINHOOK_PROCESS_THREADS`, thread enumeration first uses
  `src/core/hooks/thread_snapshot.cpp` (Windows PSS, current process, IDs only).
  Failure falls back to the original Toolhelp enumerator. Thread suspension,
  instruction-pointer relocation, patching and resume code are unchanged.
  No public MinHook API was changed. All other vendored files are unchanged.
  The x64 build uses
  `buffer.c`, `hook.c`, `trampoline.c` and `hde64.c`. Its BSD-2 notices,
  including HDE notices, are in `minhook/LICENSE.txt` and installed with CMFM.
  The build requires no download.
- SharpPluginLoader **7be239303f90af7f7356a3695adc07cb6b0707ef**,
  <https://github.com/Fexty12573/SharpPluginLoader>, is a source reference for
  the MSVC common-main signature in `entry_signature.cpp`. Its MIT notice
  is retained in `notices/SharpPluginLoader-MIT.txt` and installed with CMFM.
  No SPL implementation or binary is linked into CMFM.

CMFM does not copy SPL's security-cookie reset. It verifies the normal CRT
wrapper and lets the original cookie initializer run unchanged.
- Stracker's Loader **977030295d4635d815259eaacf77969215d2b7a4**,
  <https://github.com/Strackeror/MHW-QuestLoader>, MIT, Copyright (c) 2020
  Remi Lavergne. `plugins/stracker_compat/loader_abi.h` reproduces its
  `loader.h` declarations exactly (the ABI Stracker plugins import), and
  `loader.cpp` follows its start-up, log texts and version pattern. Its `LOG`
  class derives from BSD-2-Clause code, Copyright (c) 2018 Philipp
  Kraehenbuehl. Both notices are in `notices/StrackerLoader-MIT.txt` and
  `notices/StrackerLoader-log-BSD-2.txt` and installed with CMFM. No Stracker
  binary is linked or shipped.
