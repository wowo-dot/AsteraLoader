// "al.d3d12" core side without hooks: registration rules, chain order,
// AL's event layer and the presentation role. A stand-in installer links
// each chain's layers the way the hook layer does, ending at fake DXGI
// methods, and the test calls the first layer as the game's call would.
#include "core/graphics/hub.hpp"

#include <windows.h>
#include <dxgi1_4.h>

#include <atomic>
#include <climits>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace graphics = al::graphics;

namespace {
int failures = 0;
void check(bool condition, const std::string& label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        ++failures;
    }
}

std::vector<std::string> g_trace;  // what ran, in order

struct FakeClient {
    std::atomic<bool> stopped{false};
    std::vector<std::string> log;
    std::string disabled;
    graphics::Client client;

    FakeClient(const char* name, std::size_t load_index) {
        client.self = reinterpret_cast<al_plugin_t*>(this);
        client.name = name;
        client.load_index = load_index;
        client.stopped = &stopped;
        client.log = [this](std::string_view level, std::string_view text) {
            log.push_back(std::string(level) + " " + std::string(text));
        };
        client.disable = [this](std::string reason) {
            disabled = std::move(reason);
            stopped = true;
        };
    }
};

// A fake swap chain: a vtable with the slots AL's layer calls.
struct FakeSwapchain {
    void** vtable;
    UINT width, height, buffers, back_buffer;
    DXGI_FORMAT format;
    HWND window;
    bool composition = false;  // made for composition: GetHwnd fails, as DXGI's does
};

HRESULT STDMETHODCALLTYPE fake_query(FakeSwapchain* self, REFIID, void** out) {
    *out = self;
    return S_OK;
}
ULONG STDMETHODCALLTYPE fake_add_ref(FakeSwapchain*) { return 2; }
ULONG STDMETHODCALLTYPE fake_release(FakeSwapchain*) { return 1; }
HRESULT STDMETHODCALLTYPE fake_get_desc1(FakeSwapchain* self, DXGI_SWAP_CHAIN_DESC1* desc) {
    *desc = {};
    desc->Width = self->width;
    desc->Height = self->height;
    desc->Format = self->format;
    desc->BufferCount = self->buffers;
    desc->Flags = 2;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE fake_get_hwnd(FakeSwapchain* self, HWND* out) {
    *out = self->composition ? nullptr : self->window;
    return self->composition ? DXGI_ERROR_INVALID_CALL : S_OK;
}
UINT STDMETHODCALLTYPE fake_back_buffer(FakeSwapchain* self) { return self->back_buffer; }

void* g_swapchain_vtable[64] = {};
FakeSwapchain g_swapchain{g_swapchain_vtable, 2560, 1440, 3, 1, DXGI_FORMAT_R10G10B10A2_UNORM,
                          reinterpret_cast<HWND>(0x1234)};
int g_queue = 0;  // stands for the game's ID3D12CommandQueue
int g_device = 0;
FakeSwapchain* g_made = &g_swapchain;  // what the creation methods return

void fake_vtable() {
    for (auto& slot : g_swapchain_vtable) slot = nullptr;
    g_swapchain_vtable[0] = reinterpret_cast<void*>(&fake_query);
    g_swapchain_vtable[1] = reinterpret_cast<void*>(&fake_add_ref);
    g_swapchain_vtable[2] = reinterpret_cast<void*>(&fake_release);
    g_swapchain_vtable[18] = reinterpret_cast<void*>(&fake_get_desc1);
    g_swapchain_vtable[20] = reinterpret_cast<void*>(&fake_get_hwnd);
    g_swapchain_vtable[36] = reinterpret_cast<void*>(&fake_back_buffer);
}

// The original methods at the end of each chain.
HRESULT STDMETHODCALLTYPE original_create_for_hwnd(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                   IDXGISwapChain1** out) {
    g_trace.push_back("original create");
    *out = reinterpret_cast<IDXGISwapChain1*>(g_made);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE original_create(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain** out) {
    *out = reinterpret_cast<IDXGISwapChain*>(g_made);
    return S_OK;
}

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

void* g_present1_entry = nullptr;  // the Present1 chain's first layer
bool g_nest = false;               // the original Present calls Present1, as Streamline might

HRESULT STDMETHODCALLTYPE original_present(IDXGISwapChain* self, UINT, UINT) {
    g_trace.push_back("original present");
    if (g_nest && g_present1_entry) {
        reinterpret_cast<Present1Fn>(g_present1_entry)(reinterpret_cast<IDXGISwapChain1*>(self), 0, 0, nullptr);
    }
    return DXGI_STATUS_OCCLUDED;
}
HRESULT STDMETHODCALLTYPE original_present1(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*) {
    g_trace.push_back("original present1");
    return S_OK;
}
HRESULT STDMETHODCALLTYPE original_resize(IDXGISwapChain*, UINT, UINT width, UINT height, DXGI_FORMAT, UINT) {
    g_trace.push_back("original resize");
    g_swapchain.width = width;
    g_swapchain.height = height;
    return S_OK;
}

// Plugin detours.
void* g_owner_next = nullptr;
HRESULT STDMETHODCALLTYPE owner_present(IDXGISwapChain* self, UINT sync, UINT flags) {
    g_trace.push_back("owner present");
    return reinterpret_cast<PresentFn>(g_owner_next)(self, sync, flags);
}
void* g_queue_next_a = nullptr;
void* g_queue_next_b = nullptr;
void STDMETHODCALLTYPE list_a(void*) {}
void STDMETHODCALLTYPE list_b(void*) {}

// Event callbacks; `user` names them.
std::vector<al_d3d12_present_t> g_presents;
al_d3d12_swapchain_t g_created{};
al_d3d12_resize_t g_resize_before{}, g_resize_after{};
void AL_CALL on_event(al_plugin_t*, al_d3d12_event_t event, const void* data, void* user) {
    g_trace.push_back(std::string(static_cast<const char*>(user)) + " event " + std::to_string(event));
    if (event == AL_D3D12_SWAPCHAIN_CREATED) g_created = *static_cast<const al_d3d12_swapchain_t*>(data);
    if (event == AL_D3D12_POST_PRESENT) g_presents.push_back(*static_cast<const al_d3d12_present_t*>(data));
    if (event == AL_D3D12_PRE_RESIZE) g_resize_before = *static_cast<const al_d3d12_resize_t*>(data);
    if (event == AL_D3D12_POST_RESIZE) g_resize_after = *static_cast<const al_d3d12_resize_t*>(data);
}
void AL_CALL on_event_faulting(al_plugin_t*, al_d3d12_event_t, const void*, void*) {
    volatile int* nowhere = nullptr;
    *nowhere = 1;
}

// Links every layer to the next one and the last to the original method.
class StandInInstaller : public graphics::Installer {
public:
    std::vector<std::string> calls;
    std::string refuse;

    std::string install(std::uint32_t family, void* object, const std::vector<const graphics::Chain*>& chains) override {
        calls.push_back(std::to_string(family) + "@" + (object == &g_device ? "device" : object == &g_swapchain ? "swapchain" : "?"));
        if (!refuse.empty()) return refuse;
        for (const auto* chain : chains) {
            void* original = original_for(chain->method);
            for (std::size_t k = 0; k < chain->layers.size(); ++k) {
                *chain->layers[k].next = k + 1 < chain->layers.size() ? chain->layers[k + 1].detour : original;
            }
        }
        return {};
    }

    static void* original_for(al_d3d12_method_t method) {
        switch (method) {
        case AL_D3D12_FACTORY_CREATE_SWAPCHAIN: return reinterpret_cast<void*>(&original_create);
        case AL_D3D12_FACTORY_CREATE_SWAPCHAIN_FOR_HWND: return reinterpret_cast<void*>(&original_create_for_hwnd);
        case AL_D3D12_SWAPCHAIN_PRESENT: return reinterpret_cast<void*>(&original_present);
        case AL_D3D12_SWAPCHAIN_PRESENT1: return reinterpret_cast<void*>(&original_present1);
        case AL_D3D12_SWAPCHAIN_RESIZE_BUFFERS: return reinterpret_cast<void*>(&original_resize);
        default: return reinterpret_cast<void*>(&list_b);
        }
    }
};

const graphics::Chain* find_chain(const graphics::Hub& hub, al_d3d12_method_t method) {
    for (const auto& chain : hub.chains()) {
        if (chain.method == method) return &chain;
    }
    return nullptr;
}

void* entry(const graphics::Hub& hub, al_d3d12_method_t method) {
    const auto* chain = find_chain(hub, method);
    return chain && !chain->layers.empty() ? chain->layers.front().detour : nullptr;
}

bool logged(const FakeClient& client, const std::string& part) {
    for (const auto& line : client.log) {
        if (line.find(part) != std::string::npos) return true;
    }
    return false;
}

void registration_rules() {
    graphics::Hub hub;
    FakeClient a("alpha", 0), b("beta", 1);
    const auto all = (1u << AL_D3D12_EVENT_COUNT) - 1;
    check(!hub.subscribe(a.client, 0, 0, on_event, nullptr).empty(), "empty mask refused");
    check(!hub.subscribe(a.client, 1u << AL_D3D12_EVENT_COUNT, 0, on_event, nullptr).empty(), "unknown event refused");
    check(!hub.subscribe(a.client, all, 0, nullptr, nullptr).empty(), "no callback refused");
    check(hub.subscribe(a.client, all, 0, on_event, const_cast<char*>("a")).empty(), "subscribe accepted");

    void* next = reinterpret_cast<void*>(1);
    al_d3d12_hook_t present{AL_D3D12_SWAPCHAIN_PRESENT, 0, reinterpret_cast<void*>(&owner_present), &next};
    const auto problem = hub.hook(a.client, &present, 1);
    check(problem.find("need the presentation role; claim it first") != std::string::npos, "swap chain needs the role");
    check(hub.claim_presentation(a.client).empty(), "first claim wins");
    check(hub.claim_presentation(b.client) == "alpha already holds the presentation role", "second claim refused");
    check(hub.hook(b.client, &present, 1).find("which alpha holds") != std::string::npos, "role holder named");

    al_d3d12_hook_t bad[] = {
        {AL_D3D12_QUEUE_EXECUTE_COMMAND_LISTS, 0, reinterpret_cast<void*>(&list_a), &next},
        {AL_D3D12_METHOD(AL_D3D12_QUEUE, 1), 0, reinterpret_cast<void*>(&list_a), &next},
    };
    check(hub.hook(b.client, bad, 2).find("IUnknown methods cannot be hooked") != std::string::npos, "IUnknown refused");
    al_d3d12_hook_t beyond{AL_D3D12_METHOD(AL_D3D12_QUEUE, 300), 0, reinterpret_cast<void*>(&list_a), &next};
    check(!hub.hook(b.client, &beyond, 1).empty(), "slot limit");
    al_d3d12_hook_t alien{AL_D3D12_METHOD(9, 10), 0, reinterpret_cast<void*>(&list_a), &next};
    check(!hub.hook(b.client, &alien, 1).empty(), "unknown family");
    al_d3d12_hook_t twice[] = {bad[0], bad[0]};
    check(hub.hook(b.client, twice, 2).find("hooked twice") != std::string::npos, "twice in one call");
    check(hub.hook(b.client, bad, 1).empty(), "queue open to everyone");
    check(hub.hook(b.client, bad, 1).find("hooked twice") != std::string::npos, "twice across calls");
    check(next == nullptr, "next cleared on registration");

    hub.close(0);
    check(hub.subscribe(b.client, all, 0, on_event, nullptr) == "registration closed when PRE_MAIN ended",
          "closed after PRE_MAIN");
    check(!hub.claim_presentation(b.client).empty(), "no claims after close");
}

void nothing_registered() {
    graphics::Hub hub;
    hub.close(0);
    check(hub.chains().empty(), "no chains when nobody asked");
    StandInInstaller installer;
    hub.set_installer(&installer);
    hub.device_created(&g_device);
    check(installer.calls.empty(), "nothing hooked when nobody asked");
}

void phases_only() {
    graphics::Hub hub;
    hub.close(AL_PHASE_BIT(AL_PHASE_FIRST_PRESENT));
    check(find_chain(hub, AL_D3D12_SWAPCHAIN_PRESENT) && find_chain(hub, AL_D3D12_FACTORY_CREATE_SWAPCHAIN_FOR_HWND),
          "FIRST_PRESENT alone gets Astera Loader's present and creation layers");
    check(!find_chain(hub, AL_D3D12_SWAPCHAIN_RESIZE_BUFFERS), "no resize layer without resize subscribers");
}

void full_flow() {
    g_trace.clear();
    g_presents.clear();
    fake_vtable();

    graphics::Hub hub;
    FakeClient owner("mhw_dlss", 0), overlay("overlay", 1), early("early", 2), faulty("faulty", 3);
    std::vector<std::pair<al_phase_t, std::uint64_t>> phases;
    hub.set_phase_dispatch([&](al_phase_t phase, const void* data) {
        phases.emplace_back(phase, phase == AL_PHASE_FIRST_PRESENT
                                       ? static_cast<const al_d3d12_present_t*>(data)->frame
                                       : static_cast<const al_d3d12_swapchain_t*>(data)->width);
        g_trace.push_back("phase " + std::to_string(phase));
    });
    const auto all = (1u << AL_D3D12_EVENT_COUNT) - 1;
    check(hub.claim_presentation(owner.client).empty(), "owner claims");
    al_d3d12_hook_t owner_hooks[] = {
        {AL_D3D12_SWAPCHAIN_PRESENT, 0, reinterpret_cast<void*>(&owner_present), &g_owner_next},
        {AL_D3D12_QUEUE_EXECUTE_COMMAND_LISTS, 10, reinterpret_cast<void*>(&list_a), &g_queue_next_a},
    };
    check(hub.hook(owner.client, owner_hooks, 2).empty(), "owner detours");
    al_d3d12_hook_t overlay_hook{AL_D3D12_QUEUE_EXECUTE_COMMAND_LISTS, -5, reinterpret_cast<void*>(&list_b),
                                   &g_queue_next_b};
    check(hub.hook(overlay.client, &overlay_hook, 1).empty(), "queue detour");
    check(hub.subscribe(overlay.client, all, 0, on_event, const_cast<char*>("overlay")).empty(), "overlay subscribes");
    check(hub.subscribe(early.client, AL_D3D12_EVENT_BIT(AL_D3D12_PRE_PRESENT), -1, on_event,
                        const_cast<char*>("early")).empty(), "early subscribes");
    check(hub.subscribe(faulty.client, AL_D3D12_EVENT_BIT(AL_D3D12_PRE_PRESENT), 0, on_event_faulting, nullptr)
              .empty(), "faulty subscribes");
    hub.close(AL_PHASE_BIT(AL_PHASE_SWAPCHAIN_CREATED) | AL_PHASE_BIT(AL_PHASE_FIRST_PRESENT));

    const auto* queue = find_chain(hub, AL_D3D12_QUEUE_EXECUTE_COMMAND_LISTS);
    check(queue && queue->layers.size() == 2 && queue->layers[0].client == &overlay.client &&
              queue->layers[1].client == &owner.client, "detours ordered by order");
    const auto* present = find_chain(hub, AL_D3D12_SWAPCHAIN_PRESENT);
    check(present && present->layers.size() == 2 && !present->layers[0].client &&
              present->layers[1].client == &owner.client, "Astera Loader's layer before the owner's Present");

    StandInInstaller installer;
    hub.set_installer(&installer);
    al_d3d12_status_t status{sizeof(status)};
    hub.status(status);
    check(status.state == AL_D3D12_WAITING && status.owner && std::string(status.owner) == "mhw_dlss",
          "waiting, owner named");
    hub.device_created(&g_device);
    check(installer.calls == std::vector<std::string>{"4@device", "1@device"}, "queue and factory chains hooked at device creation");
    check(g_queue_next_b == reinterpret_cast<void*>(&list_a) &&
              g_queue_next_a == StandInInstaller::original_for(AL_D3D12_QUEUE_EXECUTE_COMMAND_LISTS),
          "queue chain linked");

    // The game creates its swap chain.
    IDXGISwapChain1* created = nullptr;
    using CreateFn = decltype(&original_create_for_hwnd);
    const auto create = reinterpret_cast<CreateFn>(entry(hub, AL_D3D12_FACTORY_CREATE_SWAPCHAIN_FOR_HWND));
    check(create && create(nullptr, reinterpret_cast<IUnknown*>(&g_queue), nullptr, nullptr, nullptr, nullptr,
                           &created) == S_OK && created == reinterpret_cast<IDXGISwapChain1*>(&g_swapchain),
          "creation reaches the original");
    check(installer.calls.size() == 3 && installer.calls[2] == "2@swapchain", "swap-chain chains hooked on creation");
    check(g_created.swapchain == &g_swapchain && g_created.queue == &g_queue && g_created.device == &g_device &&
              g_created.width == 2560 && g_created.height == 1440 && g_created.buffer_count == 3 &&
              g_created.format == DXGI_FORMAT_R10G10B10A2_UNORM && g_created.window == g_swapchain.window,
          "swap chain described");
    check(phases.size() == 1 && phases[0].first == AL_PHASE_SWAPCHAIN_CREATED && phases[0].second == 2560,
          "SWAPCHAIN_CREATED phase once");

    // Two presents from the game's thread; the original also calls Present1.
    g_present1_entry = entry(hub, AL_D3D12_SWAPCHAIN_PRESENT1);
    g_nest = true;
    const auto game_present = reinterpret_cast<PresentFn>(entry(hub, AL_D3D12_SWAPCHAIN_PRESENT));
    g_trace.clear();
    check(game_present(reinterpret_cast<IDXGISwapChain*>(&g_swapchain), 1, 0) == DXGI_STATUS_OCCLUDED,
          "Present result passes through");
    const std::vector<std::string> frame0 = {"phase 4",        "early event 1",   "overlay event 1",
                                             "owner present",  "original present", "original present1",
                                             "overlay event 2"};
    check(g_trace == frame0, "frame 0: phase, ordered PRE_PRESENT, owner, original, nested Present1 silent, POST");
    if (g_trace != frame0) for (const auto& line : g_trace) std::cerr << "  " << line << '\n';
    check(!faulty.disabled.empty() && faulty.disabled.find("pre_present callback raised exception 3221225477") !=
                                          std::string::npos, "faulting callback disables its plugin");
    g_swapchain.back_buffer = 2;
    game_present(reinterpret_cast<IDXGISwapChain*>(&g_swapchain), 0, 8);
    check(g_presents.size() == 2 && g_presents[0].frame == 0 && g_presents[1].frame == 1 &&
              g_presents[0].back_buffer == 1 && g_presents[1].back_buffer == 2 && g_presents[1].flags == 8 &&
              g_presents[0].result == DXGI_STATUS_OCCLUDED && g_presents[1].previous_qpc == g_presents[0].present_qpc &&
              g_presents[0].previous_qpc == 0, "present data");
    check(phases.size() == 2, "FIRST_PRESENT once");

    // Another thread's Present reaches the original without events.
    g_nest = false;
    g_trace.clear();
    std::thread([&] { game_present(reinterpret_cast<IDXGISwapChain*>(&g_swapchain), 0, 0); }).join();
    check(g_trace == std::vector<std::string>{"owner present", "original present"}, "other threads: detours only");

    // Resize.
    using ResizeEntry = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    check(find_chain(hub, AL_D3D12_SWAPCHAIN_RESIZE_BUFFERS) != nullptr, "resize chain present");
    const auto resize = reinterpret_cast<ResizeEntry>(entry(hub, AL_D3D12_SWAPCHAIN_RESIZE_BUFFERS));
    resize(reinterpret_cast<IDXGISwapChain*>(&g_swapchain), 0, 1920, 1080, DXGI_FORMAT_UNKNOWN, 0);
    check(g_resize_before.width == 1920 && g_resize_before.swapchain->width == 1920 && g_resize_after.result == S_OK,
          "resize events (the description is shared and current)");
    check(g_resize_after.swapchain->width == 1920 && g_resize_after.swapchain->height == 1080, "described after resize");

    hub.status(status);
    check(status.state == AL_D3D12_RUNNING && status.frames == 2 && status.last_present_qpc ==
                                                                          g_presents[1].present_qpc,
          "status counts the game's presents");
    const auto lines = hub.summary();
    bool counted = false;
    for (const auto& line : lines) {
        if (line.find("plugin=early event=pre_present order=-1 calls=2") != std::string::npos) counted = true;
    }
    check(counted, "summary counts calls");

    hub.set_unavailable("a test reason");
    hub.status(status);
    check(status.state == AL_D3D12_UNAVAILABLE && std::string(status.reason) == "a test reason", "unavailable");
    check(logged(owner, "WARN al.d3d12 is unavailable: a test reason") &&
              logged(overlay, "al.d3d12 is unavailable"), "registered plugins told");
}

void no_hook_layer() {
    graphics::Hub hub;
    FakeClient a("alpha", 0);
    void* next = nullptr;
    al_d3d12_hook_t hook{AL_D3D12_DEVICE_CREATE_COMMAND_QUEUE, 0, reinterpret_cast<void*>(&list_a), &next};
    check(hub.hook(a.client, &hook, 1).empty(), "device detour");
    hub.close(0);
    hub.device_created(&g_device);
    al_d3d12_status_t status{sizeof(status)};
    hub.status(status);
    check(status.state == AL_D3D12_UNAVAILABLE &&
              std::string(status.reason) == "this Astera Loader build cannot hook Direct3D 12 methods yet", "no installer");
    check(next == nullptr, "next stays null when nothing is hooked");
}

void installer_refuses() {
    graphics::Hub hub;
    FakeClient a("alpha", 0);
    void* next = nullptr;
    al_d3d12_hook_t hook{AL_D3D12_DEVICE_CREATE_COMMAND_QUEUE, 0, reinterpret_cast<void*>(&list_a), &next};
    hub.hook(a.client, &hook, 1);
    hub.close(0);
    StandInInstaller installer;
    installer.refuse = "the target is already patched";
    hub.set_installer(&installer);
    hub.device_created(&g_device);
    al_d3d12_status_t status{sizeof(status)};
    hub.status(status);
    check(status.state == AL_D3D12_UNAVAILABLE &&
              std::string(status.reason) == "cannot hook device methods: the target is already patched",
          "installer refusal reported");

    // A factory family succeeded, then swap-chain installation failed.
    // The creation layer already on the stack must not announce a working
    // swap chain; future calls must not raise phases or callbacks either.
    graphics::Hub later;
    unsigned delivered = 0;
    later.set_phase_dispatch([&](al_phase_t, const void*) { ++delivered; });
    later.subscribe(a.client, (1u << AL_D3D12_EVENT_COUNT) - 1, 0, on_event, const_cast<char*>("alpha"));
    later.close(AL_PHASE_BIT(AL_PHASE_FIRST_PRESENT));
    StandInInstaller second;
    later.set_installer(&second);
    later.device_created(&g_device);
    second.refuse = "fixture swap-chain failure";
    g_trace.clear();
    later.swapchain_created(&g_swapchain, &g_queue, g_swapchain.window);
    auto present = later.begin_present(&g_swapchain, 0, 0);
    later.end_present(present, S_OK);
    auto resize = later.begin_resize(&g_swapchain, 2, 640, 480, 28, 0);
    later.end_resize(resize, S_OK);
    later.status(status);
    check(status.state == AL_D3D12_UNAVAILABLE && status.frames == 0 && !present.raised && !resize.raised &&
        delivered == 0 && g_trace.empty(), "failed swap-chain install raises no graphics phase or event");
}

// The window the game created its swap chain for. A presentation owner may
// hand the game a composition swap chain instead (the DLSS mod's stand-in
// for frame generation), which has no HWND; input still needs the window.
void creation_window() {
    fake_vtable();
    const auto game_window = reinterpret_cast<HWND>(0x5678);
    const auto own_window = reinterpret_cast<HWND>(0x1234);
    struct Case {
        const char* label;
        bool composition;
        HWND own;      // the swap chain's own HWND
        HWND created;  // the window the game passed
        bool legacy;   // CreateSwapChain, with the window in its description
        HWND expected;
        const char* source;
    };
    const Case cases[] = {
        {"composition chain: the creation window", true, nullptr, game_window, false, game_window, "creation"},
        {"composition chain through CreateSwapChain", true, nullptr, game_window, true, game_window, "creation"},
        {"ordinary chain: its own window", false, own_window, game_window, false, own_window, "chain"},
        {"neither: no window", true, nullptr, nullptr, false, nullptr, "none"},
    };
    for (const auto& item : cases) {
        const std::string label = item.label;
        FakeSwapchain chain{g_swapchain_vtable, 1280, 720, 2, 0, DXGI_FORMAT_R8G8B8A8_UNORM, item.own, item.composition};
        g_made = &chain;
        graphics::Hub hub;
        FakeClient client("menu", 0);
        const void* phase_window = reinterpret_cast<void*>(1);
        hub.set_phase_dispatch([&](al_phase_t phase, const void* data) {
            if (phase == AL_PHASE_SWAPCHAIN_CREATED) phase_window = static_cast<const al_d3d12_swapchain_t*>(data)->window;
        });
        hub.subscribe(client.client, (1u << AL_D3D12_EVENT_COUNT) - 1, 0, on_event, const_cast<char*>("menu"));
        hub.close(AL_PHASE_BIT(AL_PHASE_SWAPCHAIN_CREATED));
        StandInInstaller installer;
        hub.set_installer(&installer);
        hub.device_created(&g_device);
        g_created = {};
        if (item.legacy) {
            DXGI_SWAP_CHAIN_DESC desc{};
            desc.OutputWindow = item.created;
            IDXGISwapChain* out = nullptr;
            const auto create = reinterpret_cast<decltype(&original_create)>(entry(hub, AL_D3D12_FACTORY_CREATE_SWAPCHAIN));
            check(create && create(nullptr, reinterpret_cast<IUnknown*>(&g_queue), &desc, &out) == S_OK, label);
        } else {
            IDXGISwapChain1* out = nullptr;
            const auto create = reinterpret_cast<decltype(&original_create_for_hwnd)>(
                entry(hub, AL_D3D12_FACTORY_CREATE_SWAPCHAIN_FOR_HWND));
            check(create && create(nullptr, reinterpret_cast<IUnknown*>(&g_queue), item.created, nullptr, nullptr,
                                   nullptr, &out) == S_OK, label);
        }
        check(g_created.swapchain == &chain && g_created.window == item.expected && g_created.width == 1280,
              label + ": event data");
        check(phase_window == item.expected, label + ": SWAPCHAIN_CREATED phase data");
        bool named = false;
        for (const auto& line : hub.summary()) {
            if (line.find(std::string("swapchain_window=") + item.source) != std::string::npos) named = true;
        }
        check(named, label + ": the summary names where the window came from");

        // A resize describes the chain again; the window stays.
        g_resize_after = {};
        auto resize = hub.begin_resize(&chain, 2, 1920, 1080, 0, 0);
        hub.end_resize(resize, S_OK);
        check(g_resize_after.swapchain && g_resize_after.swapchain->window == item.expected &&
                  g_resize_after.swapchain->width == 1280, label + ": a resize keeps the window");
    }
    g_made = &g_swapchain;
}

void internal_regions() {
    check(!graphics::internal(), "not internal by default");
    graphics::begin_internal();
    graphics::begin_internal();
    graphics::end_internal();
    check(graphics::internal(), "pairs nest");
    graphics::end_internal();
    graphics::end_internal();  // one too many is ignored
    check(!graphics::internal(), "balanced");
    bool other = true;
    graphics::begin_internal();
    std::thread([&] { other = graphics::internal(); }).join();
    graphics::end_internal();
    check(!other, "per thread");
}

void old_status_size() {
    graphics::Hub hub;
    al_d3d12_status_t status{};
    status.size = offsetof(al_d3d12_status_t, frames);
    status.frames = 77;
    hub.status(status);
    check(status.frames == 77 && status.size == offsetof(al_d3d12_status_t, frames), "status honours size");
}
}  // namespace

int main() {
    registration_rules();
    nothing_registered();
    phases_only();
    full_flow();
    no_hook_layer();
    installer_refuses();
    creation_window();
    internal_regions();
    old_status_size();
    if (failures) {
        std::cerr << failures << " d3d12 hub check(s) failed\n";
        return 1;
    }
    std::cout << "al.d3d12 registration, chain order, events and the presentation role checked\n";
    return 0;
}
