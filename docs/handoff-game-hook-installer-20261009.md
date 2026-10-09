# Native game-hook installer (2026-10-09)

The `cmfm.hooks` native installer is implemented and registered after
PRE_MAIN. The current installed MO2 build stays in place; the new build is
staged separately under `local/stage`. No MHW game run was started here.

## Changes

- `src/core/game_hooks/installer.{hpp,cpp}` implements inactive MinHook
  creation and activation. Its RX entry is exactly `mov r11, imm64(head);
  jmp [r11]`; arguments, flags and stack survive without a TLS check or
  helper call. The original trampoline is returned before activation.
- Existing jump patches are accepted and their module is logged through
  bounded relay resolution. Unsupported relocation returns a reason.
- Successfully created entries and trampolines stay until process exit;
  later creation/activation runs under the registry lock.
- `phases::arm` registers it even if there are no pending hooks, for later
  plugin registrations, then logs the registry summary. SHUTDOWN logs it too.
- The registry preserves MinHook's actual patch footprint for late
  signature checks. Both pattern and address joins can validate the
  original bytes after CMFM's patch. The live patch is checked before
  overlaying its saved bytes; other bytes are checked as they are. Pattern
  searches include existing chains and still require one unique match.

The original registry's stand-in tests did not patch code, so they missed
the late-join signature failure. The native fixtures now cover it. Normal
and hotpatch-above footprints are handled separately so unchanged bytes
belonging to another chain are not substituted during validation.

## Verification

Release build with warnings as errors; `dev.py check`: 41/41 tests pass.
The core still imports only `KERNEL32.dll`.

`game_hooks_native` runs the production launcher/core on an original fake
game and three plugin DLLs. It checks:

- pattern + address registrations on one target, in declared order;
- a third plugin joining by pattern at WIN_MAIN, without repatching;
- eight mixed integer/floating-point arguments and the returned value;
- a new target installed from a worker after startup;
- 5,000 off/on cycles while another thread calls the function, with
  overlapping calls, legal per-call traces and unchanged target bytes;
- a pre-existing MinHook patch still running beneath CMFM, with its owning
  fixture DLL named in the log;
- no graphics `internal()` bypass on game functions;
- registry summaries after arming and at SHUTDOWN.

`game_hook_installer` additionally checks inactive creation, live head
updates, four XMM registers + stack arguments, last-error preservation,
invalid/duplicate requests, a relocation refusal, original-signature
address joins and wrong-signature rejection, and hotpatch-above entries.

Actual MHW signatures, game detours, clients such as G8F and the remaining
DLSS game hooks stay unverified. The frame-tick/exit probe and those client
migrations are separate work. Prepare any real run through `tools/dev.py`
and let the user launch through MO2, then collect and seal its results.
