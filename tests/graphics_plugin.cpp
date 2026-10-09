#include "graphics_fixture.hpp"
#include "cmfm/cmfm_plugin.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <bit>
#include <iterator>

namespace {
const cmfm_d3d12_t* graphics{};
cmfm_plugin_t* self{};
GraphicsRecord record{};
void* queue_next{};
void STDMETHODCALLTYPE execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
#ifdef GRAPHICS_OWNER
    record(OwnerQueue, queue, count, reinterpret_cast<std::uintptr_t>(lists));
#else
    record(ProbeQueue, queue, count, reinterpret_cast<std::uintptr_t>(lists));
#endif
    std::bit_cast<decltype(&execute)>(queue_next)(queue, count, lists);
}
void CMFM_CALL event(cmfm_plugin_t*, cmfm_d3d12_event_t kind, const void* data, void*) {
    unsigned token = Invalid;
    void* chain{};
    std::uintptr_t a = 0, b = 0;
    switch (kind) {
    case CMFM_D3D12_SWAPCHAIN_CREATED: {
        const auto& p = *static_cast<const cmfm_d3d12_swapchain_t*>(data);
        token = ProbeCreated; chain = p.swapchain; a = p.width; b = p.height; break;
    }
    case CMFM_D3D12_PRE_PRESENT: case CMFM_D3D12_POST_PRESENT: {
        const auto& p = *static_cast<const cmfm_d3d12_present_t*>(data);
        token = kind == CMFM_D3D12_PRE_PRESENT ? ProbePre : ProbePost; chain = p.swapchain->swapchain;
        a = p.sync_interval; b = p.flags; break;
    }
    case CMFM_D3D12_PRE_RESIZE: case CMFM_D3D12_POST_RESIZE: {
        const auto& p = *static_cast<const cmfm_d3d12_resize_t*>(data);
        token = kind == CMFM_D3D12_PRE_RESIZE ? ProbePreResize : ProbePostResize; chain = p.swapchain->swapchain;
        a = p.width; b = p.height; break;
    }
    default: break;
    }
#ifdef GRAPHICS_OWNER
    ++token;
#endif
    record(token, chain, a, b);
}
#ifdef GRAPHICS_OWNER
void *present_next{}, *present1_next{}, *resize_next{}, *resize1_next{}, *factory_next{};
void *list_next{}, *device_next{}, *signal_next{}, *external_signal_next{};
HRESULT STDMETHODCALLTYPE present(IDXGISwapChain* chain, UINT sync, UINT flags) {
    record(PresentDetour, chain, sync, flags);
    if (!present_next) return E_FAIL;
    // Model Streamline reaching the same native object again. Nesting must
    // bypass every CMFM layer and balance back to zero after the region.
    graphics->begin_internal(self); graphics->begin_internal(self);
    const auto result = chain->Present(sync, flags);
    graphics->end_internal(self); graphics->end_internal(self);
    return result;
}
HRESULT STDMETHODCALLTYPE present1(IDXGISwapChain1* chain, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) {
    record(PresentDetour, chain, sync, flags);
    if (!present1_next || !parameters || parameters->DirtyRectsCount) return E_FAIL;
    graphics->begin_internal(self);
    const auto result = chain->Present1(sync, flags, parameters);
    graphics->end_internal(self);
    return result;
}
HRESULT STDMETHODCALLTYPE resize(IDXGISwapChain* chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    record(ResizeDetour, chain, width, height);
    return std::bit_cast<decltype(&resize)>(resize_next)(chain, count, width, height, format, flags);
}
HRESULT STDMETHODCALLTYPE resize1(IDXGISwapChain3* chain, UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                  UINT flags, const UINT* masks, IUnknown* const* queues) {
    record(ResizeDetour, chain, width, height);
    if (count != 2 || format != DXGI_FORMAT_R8G8B8A8_UNORM || flags || !masks || masks[0] != 1 || masks[1] != 1 ||
        !queues || !queues[0] || queues[0] != queues[1]) { record(Invalid, chain, 0, 0); return E_FAIL; }
    return std::bit_cast<decltype(&resize1)>(resize1_next)(chain, count, width, height, format, flags, masks, queues);
}
HRESULT STDMETHODCALLTYPE create(IDXGIFactory2* factory, IUnknown* queue, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* restrict_to, IDXGISwapChain1** out) {
    record(FactoryDetour, queue, desc->Width, desc->Height);
    if (!window || fullscreen || restrict_to || !out || desc->BufferCount != 2) return E_FAIL;
    return std::bit_cast<decltype(&create)>(factory_next)(factory, queue, window, desc, fullscreen, restrict_to, out);
}
HRESULT STDMETHODCALLTYPE close_list(ID3D12GraphicsCommandList* list) {
    record(ListDetour, list, 0, 0);
    return std::bit_cast<decltype(&close_list)>(list_next)(list);
}
UINT STDMETHODCALLTYPE node_count(ID3D12Device* device) {
    record(DeviceDetour, device, 0, 0);
    return std::bit_cast<decltype(&node_count)>(device_next)(device);
}
HRESULT STDMETHODCALLTYPE signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    record(SignalDetour, queue, static_cast<std::uintptr_t>(value), 0);
    return std::bit_cast<decltype(&signal)>(signal_next)(queue, fence, value);
}
HRESULT STDMETHODCALLTYPE external_signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    record(ExistingSignal, queue, static_cast<std::uintptr_t>(value), 0);
    return std::bit_cast<decltype(&external_signal)>(external_signal_next)(queue, fence, value);
}
bool prepatch() {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory; ComPtr<IUnknown> adapter; ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC desc{};
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) ||
        FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) ||
        FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)))) return false;
    const auto target = (*reinterpret_cast<void***>(queue.Get()))[14];
    return MH_Initialize() == MH_OK && MH_CreateHook(target, std::bit_cast<void*>(&external_signal), &external_signal_next) == MH_OK &&
        MH_EnableHook(target) == MH_OK;
}
#endif
void CMFM_CALL phase(cmfm_plugin_t*, cmfm_phase_t id, const void* data) {
#ifdef GRAPHICS_OWNER
    if (id == CMFM_PHASE_PRE_MAIN && !prepatch()) record(Invalid, nullptr, 0, 0);
    if (id == CMFM_PHASE_SWAPCHAIN_CREATED) record(SwapchainPhase, static_cast<const cmfm_d3d12_swapchain_t*>(data)->swapchain, 0, 0);
    if (id == CMFM_PHASE_FIRST_PRESENT) record(FirstPresentPhase, static_cast<const cmfm_d3d12_present_t*>(data)->swapchain->swapchain, 0, 0);
    if (id == CMFM_PHASE_SHUTDOWN) {
        cmfm_d3d12_status_t status{}; status.size = sizeof(status); graphics->status(self, &status);
        record(status.state == CMFM_D3D12_RUNNING && status.frames == 4 ? ShutdownPhase : Invalid, nullptr, 0, 0);
    }
#else
    (void)id; (void)data;
#endif
}
#ifdef GRAPHICS_OWNER
constexpr const char* name = "graphics_owner";
constexpr int order = 10;
constexpr std::uint32_t phases = CMFM_PHASE_BIT(CMFM_PHASE_PRE_MAIN) | CMFM_PHASE_BIT(CMFM_PHASE_SWAPCHAIN_CREATED) |
    CMFM_PHASE_BIT(CMFM_PHASE_FIRST_PRESENT) | CMFM_PHASE_BIT(CMFM_PHASE_SHUTDOWN);
#else
constexpr const char* name = "graphics_probe";
constexpr int order = -10;
constexpr std::uint32_t phases = 0;
#endif
const cmfm_plugin_info_t info{sizeof(info), CMFM_API_VERSION, name, name, "0.0.1", phases, order,
    nullptr, nullptr, nullptr, phase, nullptr};
}
extern "C" CMFM_PLUGIN_EXPORT const cmfm_plugin_info_t* CMFM_CALL cmfm_plugin_info() { return &info; }
extern "C" CMFM_PLUGIN_EXPORT cmfm_result_t CMFM_CALL cmfm_plugin_init(const cmfm_api_t* api, const cmfm_init_t* init) {
    self = init->self;
    record = std::bit_cast<GraphicsRecord>(GetProcAddress(GetModuleHandleW(nullptr), "cmfm_test_graphics_record"));
    graphics = static_cast<const cmfm_d3d12_t*>(api->get_interface(self, CMFM_D3D12_INTERFACE, CMFM_D3D12_VERSION));
    if (!record || !graphics || graphics->subscribe(self, (1u << CMFM_D3D12_EVENT_COUNT) - 1, order, event, nullptr) != CMFM_OK) return CMFM_ERROR;
    const cmfm_d3d12_hook_t queue{CMFM_D3D12_QUEUE_EXECUTE_COMMAND_LISTS, order,
        std::bit_cast<void*>(&execute), &queue_next};
    if (graphics->hook(self, &queue, 1) != CMFM_OK) return CMFM_ERROR;
#ifdef GRAPHICS_OWNER
    if (graphics->claim_presentation(self) != CMFM_OK) return CMFM_ERROR;
#define HOOK(method, fn, next) {method, order, std::bit_cast<void*>(&fn), &next}
    const cmfm_d3d12_hook_t hooks[]{
        HOOK(CMFM_D3D12_SWAPCHAIN_PRESENT, present, present_next), HOOK(CMFM_D3D12_SWAPCHAIN_PRESENT1, present1, present1_next),
        HOOK(CMFM_D3D12_SWAPCHAIN_RESIZE_BUFFERS, resize, resize_next), HOOK(CMFM_D3D12_SWAPCHAIN_RESIZE_BUFFERS1, resize1, resize1_next),
        HOOK(CMFM_D3D12_FACTORY_CREATE_SWAPCHAIN_FOR_HWND, create, factory_next),
        HOOK(CMFM_D3D12_METHOD(CMFM_D3D12_COMMAND_LIST, 9), close_list, list_next),
        HOOK(CMFM_D3D12_METHOD(CMFM_D3D12_DEVICE, 7), node_count, device_next),
        HOOK(CMFM_D3D12_QUEUE_SIGNAL, signal, signal_next)};
#undef HOOK
    if (graphics->hook(self, hooks, static_cast<std::uint32_t>(std::size(hooks))) != CMFM_OK) return CMFM_ERROR;
#endif
    return CMFM_OK;
}
