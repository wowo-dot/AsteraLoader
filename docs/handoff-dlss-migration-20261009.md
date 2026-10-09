# Moving the DLSS mod onto cmfm.d3d12 (2026-10-09)

For the DLSS project (`D:\mh world modding\dlssupdate`; read-only from CMFM).
CMFM side: API 1.3, tag v0.1.0-dev.10, confirmed in the game in
`graphics-mo2-1` with the DLSS mod off. Contract: `include/cmfm/cmfm_plugin.h`
("cmfm.d3d12"); design: `design-d3d12-1.3.md`. The DLSS mod's hook list
below comes from reading its shipped source on 2026-10-09 (`mhw_dlss_alpha`
target, 0.13.11). Check it against the current source before changing
anything.

## Why

In CMFM mode the mod's own MinHook patches would land on the same functions
CMFM now hooks. Its byte self-check would then turn it off, and the patches
would stack. As a cmfm.d3d12 client the mod keeps its detour bodies. CMFM
owns the one hook per function and calls the detours in order.

## Hook by hook

| The mod hooks today | In CMFM mode | Role |
|---|---|---|
| `D3D12CreateDevice` export (post-call: slInit, slSetD3DDevice, `Presentation::install`) | `on_phase` DEVICE_CREATED. `cmfm_phase_device_t` gives `device`, `riid`, `adapter` (factory = `adapter->GetParent`) and `minimum_feature_level`, on the game's thread, before the game uses the device | none |
| `CreateDXGIFactory2` export (counting only) | drop | none |
| `ID3D12Device::CreateCommandQueue` (8) | `CMFM_D3D12_DEVICE_CREATE_COMMAND_QUEUE` | none |
| `IDXGIFactory::CreateSwapChain` (10), `IDXGIFactory2::CreateSwapChainForHwnd` (15), `CreateSwapChainForCoreWindow` (16) | `CMFM_D3D12_FACTORY_CREATE_SWAPCHAIN`, `..._FOR_HWND`, `CMFM_D3D12_METHOD(CMFM_D3D12_FACTORY, 16)` | presentation |
| `IDXGISwapChain` Present 8, GetBuffer 9, SetFullscreenState 10, GetDesc 12, ResizeBuffers 13, ResizeTarget 14 | `CMFM_D3D12_SWAPCHAIN_PRESENT`, `_GET_BUFFER`, `_SET_FULLSCREEN_STATE`, `METHOD(SWAPCHAIN, 12)`, `_RESIZE_BUFFERS`, `_RESIZE_TARGET` | presentation |
| Present1 22, GetCurrentBackBufferIndex 36, ResizeBuffers1 39 | `CMFM_D3D12_SWAPCHAIN_PRESENT1`, `_GET_CURRENT_BACK_BUFFER_INDEX`, `_RESIZE_BUFFERS1` | presentation |
| `ID3D12CommandQueue::ExecuteCommandLists` (10) | `CMFM_D3D12_QUEUE_EXECUTE_COMMAND_LISTS` | none |
| Command list Close 9, Reset 10, DrawInstanced 12, DrawIndexedInstanced 13, Dispatch 14, CopyResource 17, ResourceBarrier 26, and about 25 binding setters | `CMFM_D3D12_METHOD(CMFM_D3D12_COMMAND_LIST, slot)` with the same slots | none |
| The 3 game functions for Reflex, and the game's own NGX functions | unchanged: these are game code, not D3D12. They wait for the shared hook service (roadmap item 10) | |

Each `cmfm_d3d12_hook_t` is `{method, order, detour, &next}`. The detour keeps
its signature; `next` replaces the MinHook trampoline. The installer refuses
a slot that the object's interface does not contain, so take the slot
numbers from the SDK headers, counting QueryInterface as 0.

## Order of events in CMFM mode

1. **`cmfm_plugin_init`** (PRE_MAIN thread, outside the loader lock):
   - get `"cmfm.d3d12"` 1.0;
   - call `claim_presentation()`;
   - call `hook()` with every row above, all in one call or in a few
     calls.

   Registration closes when PRE_MAIN ends. Nothing may be registered later
   from a worker thread, as the mod does today.
2. **DEVICE_CREATED:** CMFM first installs the device, queue, command-list
   and factory chains. Then the mod's `on_phase` runs. That is the place for
   today's D3D12CreateDevice post-call work: Streamline init, set device,
   upgrade the device and factory into the mod's private proxies.

   The detours are live a moment before that `on_phase`. The game is still
   blocked inside device creation then, so in practice nothing calls them
   early, but they should tolerate "not set up yet" by just calling `next`.
3. **The game creates its swap chain.**
   - CMFM's factory layer runs first, then the mod's CreateSwapChain*
     detour. That detour creates the proxy chain, returns the native one and
     keeps the proxy, as today.
   - CMFM then installs the swap-chain chains, before the game gets the
     swap chain. From the game's first Present on, the mod's swap-chain
     detours run, after CMFM's event layer.
4. **Every Present on the game's thread:** CMFM's PRE_PRESENT callbacks
   (overlays), then the mod's Present detour (routing to the proxy), then
   POST_PRESENT. Streamline's pacer thread raises no CMFM events; the mod's
   own detour still sees it and keeps routing it straight on.

## Replace, keep, drop

- **Replace** the thread-local bypass counter with `begin_internal()` /
  `end_internal()` around every call into Streamline's proxies and APIs that
  may call DXGI/D3D12 themselves:
  - proxy Present and ResizeBuffers;
  - proxy queue and device creation;
  - temporary objects.

  Inside, every CMFM-hooked method on that thread goes straight to the
  original, including the mod's own command-list detours. That is today's
  semantics, shared with CMFM's layer and other plugins. The original still
  includes the Steam overlay's hooks, which sit under CMFM's.
- **Keep:**
  - creation-thread and pacer-thread routing inside the detours;
  - the exe hash check;
  - the NVIDIA runtime pins and Authenticode;
  - the "sl.* already loaded" refusal;
  - the single-instance mutex;
  - the standalone winmm.dll mode (no CMFM), unchanged.
- **Drop in CMFM mode:**
  - the "function bytes unchanged" refusal and the vtable-target comparisons
    for methods CMFM hooks. Other tools patch under CMFM (the Steam overlay
    patches all six of CMFM's present-path targets);
  - the `gate(0x23a4e2e)` caller and function-hash check. DEVICE_CREATED
    comes from the game's own import call of D3D12CreateDevice.
- **Fail closed:**
  - check `status()` in DEVICE_CREATED and after the swap chain is created.
    UNAVAILABLE means CMFM will not call the detours, so `set_status`
    DEGRADED with `reason`, frame generation off;
  - if a `*next` the mod needs is still NULL when its family should be
    live, do the same.

## Test run to ask CMFM for afterwards

Profile `cmfm-stracker` with the mod's CMFM plugin build in its plugin folder
and hello enabled, but not the standalone winmm.dll beside it; frame
generation on. CMFM checks:

- the core log's `d3d12_chain` lines show `layers=cmfm,mhw_dlss` on the
  present path and `presentation_owner=mhw_dlss`;
- hello's PRE_PRESENT count equals the game's own presents, not the
  generated frames;
- the Steam overlay still works;
- alt-tab, resize and window-mode changes;
- the exit code is 0.

The mod's logs show whether frame generation ran.
