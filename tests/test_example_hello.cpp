// Runs the hello example plugin through the plugin host:
// argv[1] is the built hello.dll, argv[2] a work folder.
// Phases get stand-in payloads; "al.d3d12" runs on fake DXGI objects,
// with a stand-in installer linking each chain to fake original methods.
#include "work_dir.hpp"
#include "core/graphics/hub.hpp"
#include "core/plugins/host.hpp"

#include <windows.h>
#include <dxgi1_4.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace {
// A fake swap chain: a vtable with the slots AL's layer calls.
struct FakeSwapchain {
    void** vtable;
    UINT width, height;
};
HRESULT STDMETHODCALLTYPE fake_query(FakeSwapchain* self, REFIID, void** out) {
    *out = self;
    return S_OK;
}
ULONG STDMETHODCALLTYPE fake_count(FakeSwapchain*) { return 1; }
HRESULT STDMETHODCALLTYPE fake_get_desc1(FakeSwapchain* self, DXGI_SWAP_CHAIN_DESC1* desc) {
    *desc = {};
    desc->Width = self->width;
    desc->Height = self->height;
    desc->Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc->BufferCount = 2;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE fake_get_hwnd(FakeSwapchain*, HWND* out) {
    *out = nullptr;
    return S_OK;
}
UINT STDMETHODCALLTYPE fake_back_buffer(FakeSwapchain*) { return 1; }

void* g_vtable[64] = {};
FakeSwapchain g_swapchain{g_vtable, 1280, 720};
int g_queue = 0;

HRESULT STDMETHODCALLTYPE original_create(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                          IDXGISwapChain1** out) {
    *out = reinterpret_cast<IDXGISwapChain1*>(&g_swapchain);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE original_present(IDXGISwapChain*, UINT, UINT) { return S_OK; }
HRESULT STDMETHODCALLTYPE original_resize(IDXGISwapChain*, UINT, UINT width, UINT height, DXGI_FORMAT, UINT) {
    g_swapchain.width = width;
    g_swapchain.height = height;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE original_other() { return E_NOTIMPL; }

class Linker : public al::graphics::Installer {
public:
    std::string install(std::uint32_t, void*, const std::vector<const al::graphics::Chain*>& chains) override {
        for (const auto* chain : chains) {
            void* original = reinterpret_cast<void*>(&original_other);
            if (chain->method == AL_D3D12_FACTORY_CREATE_SWAPCHAIN_FOR_HWND) original = reinterpret_cast<void*>(&original_create);
            if (chain->method == AL_D3D12_SWAPCHAIN_PRESENT) original = reinterpret_cast<void*>(&original_present);
            if (chain->method == AL_D3D12_SWAPCHAIN_RESIZE_BUFFERS) original = reinterpret_cast<void*>(&original_resize);
            for (std::size_t k = 0; k < chain->layers.size(); ++k) {
                *chain->layers[k].next = k + 1 < chain->layers.size() ? chain->layers[k + 1].detour : original;
            }
        }
        return {};
    }
};

void* entry(al::graphics::Hub& hub, al_d3d12_method_t method) {
    for (const auto& chain : hub.chains()) {
        if (chain.method == method) return chain.layers.front().detour;
    }
    return nullptr;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    g_vtable[0] = reinterpret_cast<void*>(&fake_query);
    g_vtable[1] = g_vtable[2] = reinterpret_cast<void*>(&fake_count);
    g_vtable[18] = reinterpret_cast<void*>(&fake_get_desc1);
    g_vtable[20] = reinterpret_cast<void*>(&fake_get_hwnd);
    g_vtable[36] = reinterpret_cast<void*>(&fake_back_buffer);

    const auto work = al_test::fresh_work_dir(argv[2]);
    fs::create_directories(work / L"plugins" / L"hello");
    fs::copy_file(argv[1], work / L"plugins" / L"hello" / L"hello.dll");

    al::plugins::Host host;
    host.load_all({(work / L"plugins").wstring(), (work / L"logs").wstring(), L""});
    const auto report = host.report();
    const bool loaded = report.size() == 1 && report[0].name == L"hello" && report[0].initialised &&
                        report[0].status == AL_STATUS_ACTIVE && report[0].version == "0.4.0" &&
                        report[0].display_name == "Astera Loader Hello";
    host.dispatch(AL_PHASE_PRE_MAIN, nullptr);  // closes al.d3d12 registration

    // Stand-in payloads; hello only logs them. The IID is ID3D12Device's.
    const al_phase_win_main_t win_main{sizeof(win_main), 0, GetModuleHandleW(nullptr), "-test", SW_SHOWDEFAULT, 0};
    host.dispatch(AL_PHASE_WIN_MAIN, &win_main);
    Linker linker;
    auto& hub = host.graphics();
    hub.set_installer(&linker);
    static const GUID device_iid{0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
    int stand_in_device = 0;
    const al_phase_device_t device{sizeof(device), 0, &stand_in_device, &device_iid, nullptr, 0xb000u, 0};
    host.dispatch(AL_PHASE_DEVICE_CREATED, &device);

    // The game's swap chain, three presents and a resize, through the chains.
    using CreateFn = decltype(&original_create);
    using PresentFn = decltype(&original_present);
    using ResizeFn = decltype(&original_resize);
    IDXGISwapChain1* created = nullptr;
    const auto create = reinterpret_cast<CreateFn>(entry(hub, AL_D3D12_FACTORY_CREATE_SWAPCHAIN_FOR_HWND));
    const auto present = reinterpret_cast<PresentFn>(entry(hub, AL_D3D12_SWAPCHAIN_PRESENT));
    const auto resize = reinterpret_cast<ResizeFn>(entry(hub, AL_D3D12_SWAPCHAIN_RESIZE_BUFFERS));
    const bool chained = create && present && resize;
    if (chained) {
        create(nullptr, reinterpret_cast<IUnknown*>(&g_queue), nullptr, nullptr, nullptr, nullptr, &created);
        for (int k = 0; k < 3; ++k) present(reinterpret_cast<IDXGISwapChain*>(created), 1, 0);
        resize(reinterpret_cast<IDXGISwapChain*>(created), 0, 1920, 1080, DXGI_FORMAT_UNKNOWN, 0);
    }
    const al_phase_shutdown_t shutdown{sizeof(shutdown), 7};
    host.dispatch(AL_PHASE_SHUTDOWN, &shutdown);

    std::ifstream file(work / L"logs" / L"plugins" / L"hello.log", std::ios::binary);
    std::ostringstream log;
    log << file.rdbuf();
    const auto text = log.str();
    bool logged = true;
    for (const auto part : {" INFO hello 0.4.0 initialised on thread ", "(Astera Loader API 1.6)",
                            " INFO folder as the game sees it: ", " INFO folder resolved through a file handle: ",
                            " INFO this DLL: ", " INFO data folder: ",
                            " INFO game version unknown, exe SHA-256 unknown..., graphics ",
                            " INFO version code found 0 time(s)",
                            " INFO al.d3d12: subscribed to pre_present,post_resize at order 0",
                            " INFO phase 0 on thread ", " INFO game exe as the game sees it: ",
                            " INFO phase 1 on thread ",
                            " INFO WinMain: instance is the game exe: yes, command line \"-test\", show command 10",
                            " INFO phase 2 on thread ",
                            " INFO device present, adapter default, minimum feature level 0xb000",
                            " INFO device interface the game asked for: {189819F1-1DB6-4B57-BE54-1821339B85F7}",
                            " INFO phase 3 on thread ", " INFO swap chain 1280x720, format 28, 2 buffers, queue given",
                            " INFO phase 4 on thread ", " INFO first present: frame 0, back buffer 1, sync interval 1",
                            " INFO resized to 1920x1080, result 0x00000000",
                            " INFO phase 6 on thread ", " INFO WinMain returned 7",
                            " INFO presents counted 3; al.d3d12 counted 3, state 1"}) {
        if (text.find(part) == std::string::npos) {
            std::cerr << "missing in hello.log: " << part << '\n';
            logged = false;
        }
    }
    if (!loaded) std::cerr << "hello was not loaded and initialised as expected\n";
    if (!chained) std::cerr << "the hub built no creation, present or resize chain\n";
    if (!loaded || !logged || !chained) {
        std::cerr << text;
        for (const auto& line : hub.summary()) std::cerr << line << '\n';
        return 1;
    }
    std::cout << "hello example plugin loaded, logged every phase and counted presents through al.d3d12\n";
    return 0;
}
