# Design: "cmfm.d3d12", graphics coordination (API 1.3)

Status 2026-10-09: the plugin API, hub, event layer and native hook installer
are implemented and tested outside the game. The real WARP stand-in runs
through the production launcher and core, installs all five families, fires
SWAPCHAIN_CREATED / FIRST_PRESENT and verifies Present and resize ordering.
All 37 CTest cases pass; `cmfm.dll` imports only KERNEL32. First MHW run
`graphics-mo2-1` (tag v0.1.0-dev.10): both families installed over the Steam
overlay's own patches, the two phases fired once, every present was counted
and seven resizes raised events, with the DLSS mod off
(`research/plugins-in-game.md`). Coexistence with the actual DLSS mod and SPL
remains unverified; the DLSS mod must move to these detours first.

## What the DLSS mod actually does

The design follows the DLSS mod's shipped code (`mhw_dlss.dll` 0.13.11,
already a CMFM plugin), read 2026-10-09. It differs from
`requirements-dlss-mod.md` §3 in one important way:

- **The game keeps DXGI's own objects.** The mod creates Streamline's proxy
  swap chain and queue beside the native ones and returns the *native* ones
  to the game. It then redirects the native methods (Present, Present1,
  GetBuffer, GetDesc, GetCurrentBackBufferIndex, SetFullscreenState,
  ResizeBuffers, ResizeBuffers1, ResizeTarget) to the proxy. It does not
  substitute objects.
- It hooks about 45 methods. All are MinHook inline patches on code reached
  through vtables:
  - factory CreateSwapChain*;
  - device CreateCommandQueue;
  - nine swap-chain methods;
  - queue ExecuteCommandLists;
  - about 33 command-list methods for frame-generation inputs;
  - the D3D12CreateDevice and CreateDXGIFactory2 exports at startup.
- Calls Streamline makes from inside a detour reach the originals through a
  thread-local bypass counter. Streamline's pacer thread presents the same
  swap chain and is routed straight to native.
- It checks that each hooked function's bytes are unchanged and turns itself
  off when another hook appears. A CMFM hook on Present placed beside its
  own would therefore disable it.

So CMFM cannot add its own Present hook next to the mod's. It has to own the
one hook per method, with the mod as one detour in it. Object substitution
(the requirements' "upgrade") is not needed.

## The API

`"cmfm.d3d12"` 1.0, in `include/cmfm/cmfm_plugin.h`:

- `subscribe(events, order, callback, user)`. Events:
  - SWAPCHAIN_CREATED;
  - PRE_PRESENT, which is where an overlay draws;
  - POST_PRESENT;
  - PRE_RESIZE, where references to the buffers are released;
  - POST_RESIZE.

  Present events fire for the game's present thread only (the first thread
  to present), and only for the swap chain the game created last.
- `hook(hooks, count)`: detours with each method's own signature, named by
  COM family and vtable slot. CMFM writes `*next` before the first call,
  as MinHook's `ppOriginal` does. Families are factory, swap chain, device,
  queue and command list. IUnknown slots are refused.
- `claim_presentation()`: one plugin holds the right to detour factory and
  swap-chain methods. The first claim wins; the log names the holder.
- `begin_internal()` / `end_internal()`: a thread-local region. Calls the
  thread makes to any hooked method go straight to the original. This is
  the DLSS mod's bypass counter, shared by every plugin.
- `status()`: WAITING, RUNNING or UNAVAILABLE with a reason. It also gives
  the presentation owner, the frame count and the last present time.

Phases SWAPCHAIN_CREATED and FIRST_PRESENT fire with
`cmfm_d3d12_swapchain_t` and `cmfm_d3d12_present_t`, from the same layer.

### Rules

- Registration is open in `cmfm_plugin_init` and PRE_MAIN only. The host
  closes it when `dispatch(PRE_MAIN)` returns. CMFM then knows every
  method it must hook, and the event lists are fixed: the present path
  takes no lock.
- Order: a lower `order` runs first, further from the original; ties run
  in load order. CMFM's own layer (`INT_MIN`) is entered before every
  detour, so PRE_PRESENT callbacks draw before the presentation owner
  routes the frame.
- Nothing is hooked that no plugin uses:
  - CMFM's layer joins the factory chains (CreateSwapChain,
    CreateSwapChainForHwnd) only when an event, either phase, or a
    swap-chain detour needs to see swap-chain creation;
  - it joins Present/Present1 only for present events or FIRST_PRESENT;
  - it joins ResizeBuffers/ResizeBuffers1 only for resize events.
- When a callback faults, its plugin is disabled, as for `on_phase`.
  Detours run directly in the chain, so CMFM cannot catch their faults.
- Under DirectX 11 (`cmfm.game`), the host marks the service UNAVAILABLE
  at close.

### Cost per frame

- **Nobody registered:** no hooks at all.
- **Only detours:** the hook layer's entry check (a thread-local read) plus
  the detours themselves.
- **With present events:** CMFM's layer adds two QPC reads per callback, a
  `GetCurrentBackBufferIndex` call when PRE_PRESENT has subscribers, and
  the callbacks. Nothing allocates.
- **Measurement:** `Hub::summary()` reports calls, total, mean and maximum
  time per subscription and event.

## Core side (implemented)

- `src/core/graphics/hub.{hpp,cpp}`:
  - registration and validation, all or none per `hook` call;
  - the presentation role;
  - building and ordering the chains at `close`;
  - event dispatch with the fault guard and timing;
  - status and summary.
- `src/core/graphics/layers.cpp`: CMFM's layer. These are plain detours
  with the DXGI signatures. They report swap-chain creation, Present and
  ResizeBuffers to the hub, then continue with `next`. A Present or
  ResizeBuffers the same thread makes from inside another one raises no
  second set of events.
- `src/core/plugins/host.cpp`:
  - the C API table;
  - a lock-free per-plugin `stopped` flag;
  - closing registration after PRE_MAIN;
  - handing the device to the hub before plugins hear of DEVICE_CREATED.

  `Host::graphics()` gives the hook layer the hub.
- Tests:
  - `d3d12_hub`: registration rules, order, real chains linked by a
    stand-in installer over fake DXGI objects, nested Present1, another
    thread's Present, a faulting callback, resize, status and the
    unavailable paths;
  - `example_hello`: the same through the host and a real plugin DLL.
- `cmfm.dll` still imports only KERNEL32: the DXGI headers add no import.

## The hook layer (implemented)

`src/core/graphics/installer.{hpp,cpp}` implements the contract below.
`interface_limits.cpp` derives bounds from SDK C vtable sizes and selects
the oldest interface that contains each requested slot. The installer
queries that interface, checks executable native image sections and the
System32 file identity, and retains target modules until exit. Temporary
objects are created inside an internal-call region and released afterwards.

All targets and pointer-storage locations are checked before creating
disabled MinHook hooks. Every layer is linked before queued activation;
unpublished entries forward only to their original. An error restores the
old `next` values and disables that family's new hooks. Hook records and
trampolines stay pinned so partial activation cannot leave dangling entries.
If a later family fails, already-installed entries also bypass CMFM layers.

The entry bridge preserves flags, volatile integer registers and XMM0-5
around a nonthrowing selector, restores the original caller's stack, then
tail-jumps using R11. The selector reads TLS and publication flags; it
allocates nothing and takes no lock. Existing jump patches are logged,
including bounded relay resolution to the owning DLL, and remain in the
original/trampoline path.

`phases::arm` registers the installer only for a nonempty registry. The hub
serializes first installation and calls `Installer::report` after recording
the result; the phase runtime reports again after SHUTDOWN. No C plugin ABI
or standalone DLSS code changed.

The installer implements `cmfm::graphics::Installer` (`hub.hpp`) and is handed to the hub
in `phases::arm`, after PRE_MAIN:

```cpp
auto& hub = host.graphics();
if (!hub.chains().empty()) hub.set_installer(&g_d3d12_installer);  // nothing to hook otherwise
```

The hub calls `install(family, object, chains)` at these points:

- **At DEVICE_CREATED**, inside `Host::dispatch`, before plugins run, on
  the game's render-setup thread. It installs the DEVICE, QUEUE,
  COMMAND_LIST and FACTORY families, in that order. `object` is the game's
  device.
- **When the game creates its first swap chain**, from CMFM's factory
  layer, after the original returned and before the game sees the swap
  chain. It installs the SWAPCHAIN family, and `object` is that swap
  chain.

Each family is installed once. It returns "" or a reason, and the hub turns
a reason into UNAVAILABLE.

What `install` must do:

1. **Resolve each chain's target** (`method` = family << 16 | slot) to the
   function behind that vtable slot:
   - SWAPCHAIN: `object`;
   - DEVICE: `object`;
   - QUEUE and COMMAND_LIST: a temporary direct queue, allocator and list
     made on `object`, released after reading;
   - FACTORY: a factory from `CreateDXGIFactory2`, or the adapter's parent.

   Refuse a slot beyond the interfaces the object implements: QueryInterface
   the interface that introduces the slot first. Refuse a target outside the
   executable code of System32 `dxgi.dll`, `d3d12.dll` or `D3D12Core.dll`.
   Refuse two chains that resolve to one target.
2. **Allow a target someone else already patched** (Steam overlay, SPL,
   ReShade), and log whose module the jump leads to. Refusing would turn
   CMFM off whenever the overlay is on. Stacking with tools outside CMFM
   cannot be avoided; between CMFM plugins it no longer happens.
3. **One inline hook per target** (MinHook is vendored). The hook enters a
   small per-chain entry stub:
   - if `cmfm::graphics::internal()` is true, it goes to MinHook's
     trampoline (the original);
   - otherwise to `chain.layers.front().detour`.

   Both are tail jumps. The stub must leave the argument registers (RCX,
   RDX, R8, R9, XMM0-3) and the stack exactly as the caller left them,
   because the chains have up to eight arguments. Calling `internal()`
   needs those registers saved and restored around the call, or a direct
   TLS read.
4. **Before enabling,** write every `*layers[k].next`: the next layer's
   detour, and the trampoline for the last one. The hub's contract is that
   no `next` is read before the first call.
5. **Keep the hooks until the process exits,** as the DLSS mod does.
   Streamline renders through them.
6. **Log the hub's `summary()` lines** to the core log after each install
   and at SHUTDOWN.

Verification for the hook layer, in the fake game:

- the game creates a WARP device and a flip-model swap chain on a hidden
  window, presents a few frames, and resizes once;
- a probe plugin subscribes to every event and detours ExecuteCommandLists;
  Present detours belong to the presentation owner, as the API requires;
- a DLSS stand-in holds the presentation role. Its Present detour calls
  native Present again inside `begin_internal`/`end_internal`, as
  Streamline does;
- the checks:
  - events fire once per frame, in order;
  - the inner Present reaches the original without events or detours;
  - detours see the right objects;
  - a second plugin's detour on the same method runs in order;
  - `cmfm.dll` still imports only KERNEL32.

`graphics_native_chains` executes this plan with four presents (Present and
Present1), ResizeBuffers and eight-argument ResizeBuffers1, two ordered
queue layers, device and command-list detours, and a pre-existing Signal
hook in the stand-in's separate MinHook instance. It verifies summaries
after installation and shutdown. Native DXGI's own queue signals can still
reach the external patch inside an internal region, while CMFM layers do
not run twice.

`graphics_installer` checks mixed floating-point / integer / eight-argument
and variadic bridge calls, SDK slot bounds, unsupported interfaces,
non-native targets, duplicate targets / storage, read-only storage,
rollback after a second MinHook creation fails, internal nesting, and
original-only forwarding after a later-family failure. The real DLSS mod,
SPL and Steam overlay in MHW stay unverified until a game run.

## What the DLSS mod would change to use it

For the DLSS project, not done here. The full checklist, hook by hook, is
`handoff-dlss-migration-20261009.md`.

- Claim the presentation role in `cmfm_plugin_init`, and register its
  detours with `hook()` instead of MinHook. The detour bodies stay; the
  `next` pointer replaces the trampoline.
- Wrap its calls into Streamline's proxies in
  `begin_internal`/`end_internal` instead of its own bypass counter.
- Replace the D3D12CreateDevice export hook with DEVICE_CREATED. The phase
  comes from the game's own import call. It gives the device, the adapter
  (the factory is its parent) and the riid, on the game's thread, before
  the game uses the device.
- Drop the "target bytes unchanged" refusal for methods CMFM hooks. The
  pacer-thread and creation-thread routing stay in its detours.
