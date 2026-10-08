# PRE_MAIN implementation handoff (2026-10-08)

Step 3 is wired into the core. The runtime loads CMFM plugins and dispatches
`CMFM_PHASE_PRE_MAIN` once, on the executable's startup thread, before its
normal CRT startup. The private launcher/core request and public C API are
unchanged; no launcher source changes are part of this step.

## How the startup point is reached

SPL's published preloader forces a time-query callback by resetting the game's
security cookie. CMFM does not reset or otherwise write that cookie.

For a packed entry in `.bind`, CMFM temporarily observes the existing Windows
`VirtualProtect` API. The observer calls the original first and preserves its
arguments, return value, old-protection output and last-error value. Only a
successful transition of the main image's entire `.text` to executable,
non-writable memory, called from `.bind`, triggers a signature scan. It does
not hook any SteamStub instruction or change protection requests or results.

The scan requires one unique MSVC startup wrapper: a relative call to a
verified cookie initializer and a tail jump to the known common-main body.
All reads and branch targets must be in readable executable ranges. Ambiguous,
out-of-bounds, encrypted or already modified code is refused. Windows exports
are checked for pre-existing detours before installing the observer.

CMFM hooks the small CRT entry wrapper, rather than the common-main function
SPL hooks. At entry, CMFM restores both its Windows observer and its CRT entry
patch before loading plugins. It then invokes the restored original entry;
the normal security-cookie initializer and game CRT execute unchanged. The
common-main code remains available to SPL. Actual SPL force-load coexistence
still needs a real run.

An executable whose entry is already in `.text` can use the same verified
entry directly. This is used by the plain stand-in tests. No unpacking or
decryption code is part of CMFM or the synthetic tests.

If the expected transition/startup entry is not reached within 15 seconds,
plugins are skipped, the observer is removed, and a warning is shown from a
separate UI thread. The game keeps running. A patch changed by another hook
is left intact rather than overwritten during removal. Original API trampolines
and CMFM's state remain pinned for process lifetime so in-flight callbacks and
plugin API pointers cannot outlive their storage. Normal startup restores the
patched functions and has no continuing observer or per-frame callback.

## Plugin integration

- The core stores the background EXE SHA-256 behind a lock and completion
  event. PRE_MAIN waits at most 15 seconds for that result. Empty/unavailable
  hashes continue to refuse hash-restricted plugins through the existing
  manifest check. Wait duration and the hash used are logged.
- Discovery uses `<exe folder>\cmfm\plugins` derived from
  `GetModuleFileNameW(nullptr)`, preserving MO2's virtual game-folder view.
  Log files use the real folder beside CMFM's DLL.
- `Host::load_all`, per-plugin name/version/status/reason reports, then
  `Host::dispatch(CMFM_PHASE_PRE_MAIN, nullptr)` run on the startup thread.
  Updated statuses are logged after dispatch; refusals receive a warning popup.
- Metadata, init and phase callbacks guard C++ exceptions and recoverable
  native faults. A faulty plugin is disabled and healthy plugins continue.
  This is fault containment, not a sandbox: fail-fast corruption, stack overflow
  and arbitrary plugin effects cannot safely be recovered in a shared process.
- Framework/proxy names are reserved, and already loaded DLL base-name
  collisions are refused before loading a plugin.

MinHook 1.3.4 is vendored and pinned by full commit, with its BSD-2/HDE notices.
SPL's MIT notice is retained for the common-main signature reference. All
notices are included in the install layout; see `third_party/README.md`.
The linked core's import table remains **KERNEL32.dll only**. User32 is resolved
from System32 only when a warning needs UI.

## Verification and next user run

The synthetic packed-entry test uses original test code in a `.bind`-named
section, a normal protection transition, and the stock MSVC CRT. Its fixture
plugin verifies PRE_MAIN once on the main thread, unchanged security cookie,
unchanged common-main code, restored CRT wrapper, and the actual background
EXE hash reaching the plugin host. A second test omits the transition and
verifies that no plugin is called, the observer is removed after its deadline,
and the stand-in still exits with code 37.

Other tests cover signature ambiguity/bounds/mutation, reserved names, bad
metadata and callback exceptions, C/C++ ABI layouts, manifests/order, launcher
confirmation/argument handling/exit codes, and suspended-child cleanup.

The real MHW protection-transition sequence and startup-wrapper match are
**not yet measured for this implementation**. A first MO2 run should use the
existing `cmfm` profile and `CMFM Launcher`, no force-load, overlay on, and no
newly enabled mods or fixture plugins. Prepare its run folder and build first;
the user launches and closes the game. Check:

1. `phase_pre_main_observer_armed` at bootstrap.
2. `unpacked_crt_verified_after_protection_change`, the verified entry/cookie/
   common-main RVAs, and `startup_api_observer_removed`.
3. `phase_pre_main_enter` and `phase_pre_main_complete`, with plugins-root path
   in the virtual game folder and recorded hash wait.
4. Normal game startup, existing MO2 mod files, and exit code 0.

If the transition/signature does not match, use the diagnostic and read-only
run evidence to refine the detector. Do not reset the security cookie or alter
SteamStub to force it. WinMain/device/swap-chain phases, the Stracker shim,
menu/status UI and SDK packaging remain later steps.

API references: [VirtualProtect](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect),
[CRT cookie initialization](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/security-init-cookie),
and [MinHook 1.3.4](https://github.com/TsudaKageyu/minhook/releases/tag/v1.3.4).
