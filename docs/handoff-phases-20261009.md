# Stage 2 phase hooks (2026-10-09)

Implemented WIN_MAIN, DEVICE_CREATED and SHUTDOWN; all 34 Release CTest cases
pass with warnings as errors. This builds on the existing
API 1.2 header and phase payloads. Their public field layouts are unchanged.
Swapchain, first-present and game-ready are still reserved. MHW confirmation
is pending; see `design-phases-1.2.md` for the implementation and test limits.

The one startup integration call is after PRE_MAIN dispatch and reporting:
`phases::arm(host, log, visible_warning, verified_crt_wrapper)`.
The rest lives under `src/core/phases/`. Per-plugin timings now take the plugin
lock when being updated/read, and phase callbacks are serialized.

Two changes to the initial hook plan matter:

- Redirecting the CRT call before cookie initialization would change the
  target SPL later resolves. WinMain uses a one-time execution breakpoint,
  preserving that call and the entry bytes. CMFM's callback runs before the
  original entry, including an SPL hook installed there later.
- The standalone DLSS mod hashes the complete game device-creation function
  after the API returns. A return-site code patch would trip that check. The
  device import forwards with the original return address; a one-time
  execution breakpoint handles its return, with no game-code changes.

Observers reserve only a free local debug slot on their calling thread, using
a short helper-worker suspension and documented context APIs. Foreign/global/
single-step state is preserved or refused. The exception handler removes the
owned slot and transfers execution; plugin code runs outside the handler.
Missing sites and failed preparation use the existing visible warning.

The packed stand-in creates a real WARP device through a delay import. A test
export hook checks the original caller and an unchanged function both before
and after API execution, like DLSS. A static initializer resolves the CRT call
and installs a WinMain inline hook after CMFM arms, like SPL. Main-thread and
separate setup-thread cases verify phase order, payload/device identity,
at-most-once delivery, last-error preservation, capability-probe rearming and
normal-return shutdown. Pure tests cover signatures, bounds, ambiguity,
import-slot conflicts and debug-state ownership. Core imports remain KERNEL32
only. These tests do not establish actual DLSS/SPL coexistence in MHW.

For the first game run, keep the current MO2 profile and launch CMFM normally.
Look for the three new phase entries, device payload, observer/import cleanup,
active plugins and a clean exit. SHUTDOWN requires returning from WinMain;
ExitProcess/TerminateProcess and crashes do not deliver it. Game objects local
to WinMain may already be destroyed; plugins should save only their own data.
