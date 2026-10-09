# Graphics hook installer (2026-10-09)

The native `cmfm.d3d12` installer is implemented. No work in the sibling
DLSS project or MO2 profiles was done. The previously tested CMFM build is
still installed; the new package is staged separately under `local/stage`.

## Implementation

- `src/core/graphics/installer.cpp`: resolves the five COM families,
  verifies the System32 native target by file identity and executable PE
  section, logs existing jump owners, builds ABI-preserving entry stubs,
  creates one MinHook hook per target and links all layers before activation.
- `src/core/graphics/interface_limits.cpp`: SDK-derived slot bounds and
  minimum interface versions. Objects must implement the required version.
- `src/core/phases/runtime.cpp`: registers the installer only when the hub
  has chains, after PRE_MAIN; reports again after SHUTDOWN.
- The hub reports after each install result. Failure restores the new
  family's previous `next` values; its entries forward to their originals.
  A later-family failure bypasses previously installed CMFM chains too.
  Hook contexts, native modules and trampolines remain until process exit.

No allocations, locking or diagnostics occur in the per-call selector.
Event callback timing remains in `Hub::summary()`; the complete graphics
cost in MHW has not been measured.

## Verification

Release build with project warnings as errors; all 37 CTest cases pass.
The core's only imported DLL is `KERNEL32.dll`.

`graphics_native_chains` uses the production launcher and core with an
original fake game, two real plugin DLLs and WARP on a hidden window:

- device, queue, command-list, factory and swap-chain families all install;
- the probe and presentation owner run their queue detours in order;
- four Present/Present1 calls raise one pair of events each, in order;
- the owner's nested internal regions invoke native Present without
  entering its own detour or raising another pair of events;
- both ResizeBuffers and eight-argument ResizeBuffers1 forward correctly;
- SWAPCHAIN_CREATED and FIRST_PRESENT each fire once;
- an earlier Signal hook in a separate MinHook instance stays in the
  original chain, and the log identifies `graphics_owner.dll` through its relay;
- install and SHUTDOWN summaries show installed chains and final counts.

`graphics_installer` checks mixed GPR/XMM and stack arguments, variadic
arguments, return value / last-error preservation, interface and target
refusals, duplicate target / next storage, read-only next storage, rollback
after a second hook cannot be created, nested bypass and unavailable routing.
Existing phase / plugin / launcher tests continue to pass.

## Next real-game work

The actual DLSS mod still owns independent native hooks and checks their
bytes. Activating this hub beside that mode can trigger its self-check.
Migrate the DLSS CMFM plugin to the shared detours / internal regions as
described in `design-d3d12-1.3.md`, or use a user-selected test profile without
the standalone graphics hooks. The sibling project is read-only from CMFM.

After that, prepare and install a named run through `tools/dev.py` and let
the user launch through MO2. Confirm event counts and graphics status, then
test loading a save, resize / alt-tab / full-screen transitions, SPL and
overlay coexistence. No real MHW graphics-hook run was performed here, and
no tested-in-game tag was created for this implementation.
