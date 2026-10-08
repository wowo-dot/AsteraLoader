# Pinned dependencies and references

- MinHook **v1.3.4**, commit `c3fcafdc10146beb5919319d0683e44e3c30d537`,
  from <https://github.com/TsudaKageyu/minhook>. `minhook/include` and
  `minhook/src` are copied unchanged from that commit. The x64 build uses
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
