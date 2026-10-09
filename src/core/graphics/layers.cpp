// AL's own layer in the d3d12 chains: plain detours with each method's
// signature that tell the hub about swap-chain creation, Present and
// ResizeBuffers, then continue with `next` like any plugin detour.
#include "core/graphics/hub.hpp"

#include <windows.h>
#include <dxgi1_4.h>

#include <atomic>

namespace al::graphics {
namespace {
std::atomic<Hub*> g_hub{nullptr};

// A Present or ResizeBuffers this thread makes from inside another one
// (Present1 inside Present, say) raises no second set of events.
thread_local int t_depth = 0;

struct Depth {
    Depth() { ++t_depth; }
    ~Depth() { --t_depth; }
    Depth(const Depth&) = delete;
    Depth& operator=(const Depth&) = delete;
};

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                      IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                             const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                                     const UINT*, IUnknown* const*);

void* g_next_create_swapchain = nullptr;
void* g_next_create_swapchain_for_hwnd = nullptr;
void* g_next_present = nullptr;
void* g_next_present1 = nullptr;
void* g_next_resize_buffers = nullptr;
void* g_next_resize_buffers1 = nullptr;

template <typename Fn>
Fn next(void* stored) {
    return reinterpret_cast<Fn>(stored);
}

HRESULT STDMETHODCALLTYPE create_swapchain(IDXGIFactory* self, IUnknown* queue, DXGI_SWAP_CHAIN_DESC* desc,
                                           IDXGISwapChain** out) {
    const HRESULT result = next<CreateSwapChainFn>(g_next_create_swapchain)(self, queue, desc, out);
    Hub* hub = g_hub.load();
    if (hub && SUCCEEDED(result) && out && *out) hub->swapchain_created(*out, queue);
    return result;
}

HRESULT STDMETHODCALLTYPE create_swapchain_for_hwnd(IDXGIFactory2* self, IUnknown* queue, HWND window,
                                                    const DXGI_SWAP_CHAIN_DESC1* desc,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                                                    IDXGIOutput* output, IDXGISwapChain1** out) {
    const HRESULT result = next<CreateSwapChainForHwndFn>(g_next_create_swapchain_for_hwnd)(
        self, queue, window, desc, fullscreen, output, out);
    Hub* hub = g_hub.load();
    if (hub && SUCCEEDED(result) && out && *out) hub->swapchain_created(*out, queue);
    return result;
}

HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* self, UINT sync_interval, UINT flags) {
    Hub* hub = g_hub.load();
    if (!hub || t_depth) return next<PresentFn>(g_next_present)(self, sync_interval, flags);
    Depth depth;
    auto call = hub->begin_present(self, sync_interval, flags);
    const HRESULT result = next<PresentFn>(g_next_present)(self, sync_interval, flags);
    hub->end_present(call, result);
    return result;
}

HRESULT STDMETHODCALLTYPE present1(IDXGISwapChain1* self, UINT sync_interval, UINT flags,
                                   const DXGI_PRESENT_PARAMETERS* parameters) {
    Hub* hub = g_hub.load();
    if (!hub || t_depth) return next<Present1Fn>(g_next_present1)(self, sync_interval, flags, parameters);
    Depth depth;
    auto call = hub->begin_present(self, sync_interval, flags);
    const HRESULT result = next<Present1Fn>(g_next_present1)(self, sync_interval, flags, parameters);
    hub->end_present(call, result);
    return result;
}

HRESULT STDMETHODCALLTYPE resize_buffers(IDXGISwapChain* self, UINT count, UINT width, UINT height,
                                         DXGI_FORMAT format, UINT flags) {
    Hub* hub = g_hub.load();
    if (!hub || t_depth) return next<ResizeBuffersFn>(g_next_resize_buffers)(self, count, width, height, format, flags);
    Depth depth;
    auto call = hub->begin_resize(self, count, width, height, static_cast<std::uint32_t>(format), flags);
    const HRESULT result = next<ResizeBuffersFn>(g_next_resize_buffers)(self, count, width, height, format, flags);
    hub->end_resize(call, result);
    return result;
}

HRESULT STDMETHODCALLTYPE resize_buffers1(IDXGISwapChain3* self, UINT count, UINT width, UINT height,
                                          DXGI_FORMAT format, UINT flags, const UINT* node_masks,
                                          IUnknown* const* queues) {
    Hub* hub = g_hub.load();
    if (!hub || t_depth) {
        return next<ResizeBuffers1Fn>(g_next_resize_buffers1)(self, count, width, height, format, flags, node_masks,
                                                              queues);
    }
    Depth depth;
    auto call = hub->begin_resize(self, count, width, height, static_cast<std::uint32_t>(format), flags);
    const HRESULT result = next<ResizeBuffers1Fn>(g_next_resize_buffers1)(self, count, width, height, format, flags,
                                                                         node_masks, queues);
    hub->end_resize(call, result);
    return result;
}
}  // namespace

void set_layer_hub(Hub* hub) noexcept {
    g_hub.store(hub);
}

void release_layer_hub(Hub* hub) noexcept {
    // Only the hub the layer reports to lets go of it.
    g_hub.compare_exchange_strong(hub, nullptr);
}

void* layer_detour(al_d3d12_method_t method) noexcept {
    switch (method) {
    case AL_D3D12_FACTORY_CREATE_SWAPCHAIN: return reinterpret_cast<void*>(&create_swapchain);
    case AL_D3D12_FACTORY_CREATE_SWAPCHAIN_FOR_HWND: return reinterpret_cast<void*>(&create_swapchain_for_hwnd);
    case AL_D3D12_SWAPCHAIN_PRESENT: return reinterpret_cast<void*>(&present);
    case AL_D3D12_SWAPCHAIN_PRESENT1: return reinterpret_cast<void*>(&present1);
    case AL_D3D12_SWAPCHAIN_RESIZE_BUFFERS: return reinterpret_cast<void*>(&resize_buffers);
    case AL_D3D12_SWAPCHAIN_RESIZE_BUFFERS1: return reinterpret_cast<void*>(&resize_buffers1);
    default: return nullptr;
    }
}

void** layer_next(al_d3d12_method_t method) noexcept {
    switch (method) {
    case AL_D3D12_FACTORY_CREATE_SWAPCHAIN: return &g_next_create_swapchain;
    case AL_D3D12_FACTORY_CREATE_SWAPCHAIN_FOR_HWND: return &g_next_create_swapchain_for_hwnd;
    case AL_D3D12_SWAPCHAIN_PRESENT: return &g_next_present;
    case AL_D3D12_SWAPCHAIN_PRESENT1: return &g_next_present1;
    case AL_D3D12_SWAPCHAIN_RESIZE_BUFFERS: return &g_next_resize_buffers;
    case AL_D3D12_SWAPCHAIN_RESIZE_BUFFERS1: return &g_next_resize_buffers1;
    default: return nullptr;
    }
}

}  // namespace al::graphics
