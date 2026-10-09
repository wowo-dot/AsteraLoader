// Original stand-in: production launcher + core + plugin API, real native
// DXGI/D3D12 on WARP. The window remains hidden throughout the test.
#include "graphics_fixture.hpp"
#include "common/log_file.hpp"
#include "common/windows.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <bit>
#include <iterator>

namespace {
using Microsoft::WRL::ComPtr;
struct Entry { unsigned token; void* object; std::uintptr_t a, b; };
std::array<Entry, 256> trace{};
unsigned used{};
bool invalid{};
struct Dump {
    cmfm::LogFile& log;
    ~Dump() {
        for (unsigned i = 0; i < used; ++i) log.line(L"trace=" + std::to_wstring(trace[i].token) + L" object=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(trace[i].object)) + L" a=" + std::to_wstring(trace[i].a) + L" b=" + std::to_wstring(trace[i].b));
        log.line(L"invalid=" + std::to_wstring(invalid));
    }
};
bool expect(unsigned begin, std::initializer_list<unsigned> tokens) {
    // Native DXGI can signal its queue inside Present. That still reaches
    // the pre-existing external patch while CMFM's internal region bypasses
    // its own Signal layer. ResizeBuffers1 also queries GetNodeCount; those
    // calls are allowed between the ordered resize events being compared.
    for (const auto token : tokens) {
        while (begin < used && (trace[begin].token == ExistingSignal ||
            (trace[begin].token == DeviceDetour && token != DeviceDetour))) ++begin;
        if (begin >= used || trace[begin++].token != token) return false;
    }
    while (begin < used && (trace[begin].token == ExistingSignal || trace[begin].token == DeviceDetour)) ++begin;
    return begin == used;
}
bool wait(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    cmfm::Handle done(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    return done && SUCCEEDED(queue->Signal(fence, value)) && SUCCEEDED(fence->SetEventOnCompletion(value, done.get())) &&
        WaitForSingleObject(done.get(), 5000) == WAIT_OBJECT_0;
}
}
extern "C" __declspec(dllexport) void cmfm_test_graphics_record(unsigned token, void* object, std::uintptr_t a, std::uintptr_t b) {
    if (token == Invalid || used >= trace.size()) { invalid = true; return; }
    trace[used++] = {token, object, a, b};
}
extern "C" BOOL cmfm_test_startup_restored();
extern "C" BOOL cmfm_test_cookie_unchanged();
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, char*, int) {
    cmfm::LogFile log;
    if (!log.open(cmfm::environment(L"CMFM_FAKE_OUTPUT"))) return 90;
    Dump dump{log};
    if (!cmfm_test_startup_restored() || !cmfm_test_cookie_unchanged()) return 91;
    ComPtr<IDXGIFactory4> factory; ComPtr<IUnknown> adapter; ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) ||
        FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return 92;
    const auto nodes_begin = used;
    if (device->GetNodeCount() != 1 || !expect(nodes_begin, {DeviceDetour}) || trace[nodes_begin].object != device.Get()) return 93;
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue))) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) return 94;
    const auto list_begin = used;
    if (FAILED(list->Close()) || !expect(list_begin, {ListDetour}) || trace[list_begin].object != list.Get()) return 95;
    ID3D12CommandList* lists[]{list.Get()};
    const auto queue_begin = used;
    queue->ExecuteCommandLists(1, lists);
    if (!expect(queue_begin, {ProbeQueue, OwnerQueue}) || trace[queue_begin].object != queue.Get() ||
        trace[queue_begin].a != 1 || trace[queue_begin].b != reinterpret_cast<std::uintptr_t>(lists)) return 96;
    if (!wait(queue.Get(), fence.Get(), 1)) return 97;
    const WNDCLASSW wc{0, DefWindowProcW, 0, 0, instance, nullptr, nullptr, nullptr, nullptr, L"CMFM hidden graphics test"};
    if (!RegisterClassW(&wc)) return 98;
    const auto window = CreateWindowExW(0, wc.lpszClassName, L"CMFM WARP fixture", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240,
        nullptr, nullptr, instance, nullptr);
    if (!window || IsWindowVisible(window)) return 99;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 320; desc.Height = 240; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain;
    const auto creation_begin = used;
    if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &chain)) ||
        !expect(creation_begin, {FactoryDetour, SwapchainPhase, ProbeCreated, OwnerCreated})) return 100;
    ComPtr<IDXGISwapChain3> chain3;
    if (FAILED(chain.As(&chain3))) return 101;
    for (unsigned i = 0; i < 4; ++i) {
        const auto present_begin = used;
        DXGI_PRESENT_PARAMETERS parameters{};
        const auto result = i & 1 ? chain->Present1(0, 0, &parameters) : chain->Present(0, 0);
        const auto correct = i == 0 ? expect(present_begin, {FirstPresentPhase, ProbePre, OwnerPre, PresentDetour, ProbePost, OwnerPost}) :
            expect(present_begin, {ProbePre, OwnerPre, PresentDetour, ProbePost, OwnerPost});
        if (FAILED(result) || !correct) return 102;
        for (unsigned k = present_begin; k < used; ++k) if (trace[k].token != ExistingSignal && trace[k].object != chain.Get()) return 103;
        if (!wait(queue.Get(), fence.Get(), i + 2)) return 104;
        if (i == 1 || i == 2) {
            const auto resize_begin = used;
            UINT masks[]{1, 1}; IUnknown* queues[]{queue.Get(), queue.Get()};
            const auto resize = i == 1 ? chain->ResizeBuffers(2, 400, 300, DXGI_FORMAT_R8G8B8A8_UNORM, 0) :
                chain3->ResizeBuffers1(2, 480, 360, DXGI_FORMAT_R8G8B8A8_UNORM, 0, masks, queues);
            if (FAILED(resize) || !expect(resize_begin, {ProbePreResize, OwnerPreResize, ResizeDetour, ProbePostResize, OwnerPostResize})) return 105;
            DXGI_SWAP_CHAIN_DESC1 actual{};
            if (FAILED(chain->GetDesc1(&actual)) || actual.Width != (i == 1 ? 400u : 480u) || actual.Height != (i == 1 ? 300u : 360u)) return 106;
        }
    }
    const auto has = [](unsigned token) { return std::count_if(trace.begin(), trace.begin() + used, [=](const auto& e) { return e.token == token; }); };
    if (invalid || has(ProbePre) != 4 || has(OwnerPre) != 4 || has(PresentDetour) != 4 || has(ProbePost) != 4 || has(ExistingSignal) < 5) return 107;
    chain3.Reset(); chain.Reset(); DestroyWindow(window); UnregisterClassW(wc.lpszClassName, instance);
    log.line(L"graphics_valid=1 frames=4 resizes=2 queue_layers=2 internal_bypass=1 existing_patch=1 hidden=1");
    return 37;
}
