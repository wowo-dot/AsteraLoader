// input_probe: a throwaway diagnostic plugin, not shipped. It answers the
// open questions in docs/design-input-1.6.md.
//
// 0.1 (frame-input-1) showed the game reads keyboard and mouse through
// DirectInput 8, not from its window's messages. 0.2 looks at DirectInput
// itself and tries blocking there, the way input capture would:
//
// - it hooks the two methods DirectInput devices hand out input through,
//   GetDeviceState and GetDeviceData, plus XInputGetState, and records
//   which devices the game reads, how, how often and on which thread;
// - F7 blocks the game's keyboard at DirectInput (no keys held, no key
//   events), F8 its mouse, F6 XInput pads; press again to undo. The keys
//   are read from the game window's messages, which DirectInput blocking
//   does not touch, so they still work while blocked;
// - it subclasses the game window (as SharpPluginLoader does) for those
//   keys and counts input messages;
// - as a client of "al.frame" it counts frames, posts a task every 250 ms
//   from a worker thread, and stops that thread at EXITING.
#define DIRECTINPUT_VERSION 0x0800
#include "al/al_plugin.h"

#include <windows.h>
#include <dinput.h>
#include <xinput.h>

#include <MinHook.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {
const al_api_t* g_api = nullptr;
al_plugin_t* g_self = nullptr;
const al_frame_t* g_frame = nullptr;

void say(al_log_level_t level, const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    g_api->log(g_self, level, text);
}

std::int64_t qpc() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

std::int64_t g_start = 0;
double seconds() {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart ? static_cast<double>(qpc() - g_start) / static_cast<double>(frequency.QuadPart) : 0;
}

// The GUIDs this needs, so the probe links no DirectInput library.
constexpr GUID kDirectInput8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
constexpr GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

// What the toggles block.
std::atomic<bool> g_block_keyboard{false};  // F7
std::atomic<bool> g_block_mouse{false};     // F8
std::atomic<bool> g_block_pads{false};      // F6

// The DirectInput devices the game reads, found as it calls them.
struct Device {
    IDirectInputDevice8W* self = nullptr;
    unsigned type = 0;  // DI8DEVTYPE_*
    char name[96] = "?";
    DWORD state_thread = 0, data_thread = 0;
    std::uint64_t state_calls = 0, data_calls = 0, events = 0, blocked = 0;
    DWORD state_size = 0;
};
std::mutex g_devices_mutex;
Device g_devices[16];
unsigned g_device_count = 0;

const char* type_name(unsigned type) {
    switch (type) {
    case DI8DEVTYPE_KEYBOARD: return "keyboard";
    case DI8DEVTYPE_MOUSE: return "mouse";
    case DI8DEVTYPE_GAMEPAD: return "gamepad";
    case DI8DEVTYPE_JOYSTICK: return "joystick";
    default: return "other";
    }
}

Device* device_of(IDirectInputDevice8W* self) {
    std::lock_guard lock(g_devices_mutex);
    for (unsigned k = 0; k < g_device_count; ++k) {
        if (g_devices[k].self == self) return &g_devices[k];
    }
    if (g_device_count == 16) return nullptr;
    auto& device = g_devices[g_device_count++];
    device.self = self;
    DIDEVICEINSTANCEW info{};
    info.dwSize = sizeof(info);
    if (SUCCEEDED(self->GetDeviceInfo(&info))) {
        device.type = GET_DIDEVICE_TYPE(info.dwDevType);
        WideCharToMultiByte(CP_UTF8, 0, info.tszProductName, -1, device.name, sizeof(device.name), nullptr, nullptr);
    }
    say(AL_LOG_INFO, "DirectInput device %p: %s \"%s\", first read on thread %lu at %.3f s", static_cast<void*>(self),
        type_name(device.type), device.name, GetCurrentThreadId(), seconds());
    return &device;
}

bool blocked(const Device& device) {
    return (device.type == DI8DEVTYPE_KEYBOARD && g_block_keyboard.load(std::memory_order_relaxed)) ||
           (device.type == DI8DEVTYPE_MOUSE && g_block_mouse.load(std::memory_order_relaxed));
}

using GetStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using GetDataFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using XInputStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
// One original per hooked method: the keyboard's and the mouse's device
// classes each have their own (input-probe-2).
GetStateFn g_get_state[2] = {};
GetDataFn g_get_data[2] = {};
XInputStateFn g_xinput_state = nullptr;

HRESULT on_state(GetStateFn original, IDirectInputDevice8W* self, DWORD size, LPVOID data) {
    const HRESULT result = original(self, size, data);
    if (Device* device = device_of(self)) {
        ++device->state_calls;
        device->state_size = size;
        if (!device->state_thread) device->state_thread = GetCurrentThreadId();
        if (SUCCEEDED(result) && data && blocked(*device)) {
            std::memset(data, 0, size);  // nothing held, no motion
            ++device->blocked;
        }
    }
    return result;
}

HRESULT on_data(GetDataFn original, IDirectInputDevice8W* self, DWORD object_size, LPDIDEVICEOBJECTDATA events,
                LPDWORD count, DWORD flags) {
    const HRESULT result = original(self, object_size, events, count, flags);
    if (Device* device = device_of(self)) {
        ++device->data_calls;
        if (!device->data_thread) device->data_thread = GetCurrentThreadId();
        if (SUCCEEDED(result) && count && events) {
            device->events += *count;
            if (*count && blocked(*device)) {
                *count = 0;  // the game gets no events
                ++device->blocked;
            }
        }
    }
    return result;
}

template <int K>
HRESULT STDMETHODCALLTYPE get_state(IDirectInputDevice8W* self, DWORD size, LPVOID data) {
    return on_state(g_get_state[K], self, size, data);
}
template <int K>
HRESULT STDMETHODCALLTYPE get_data(IDirectInputDevice8W* self, DWORD object_size, LPDIDEVICEOBJECTDATA events,
                                   LPDWORD count, DWORD flags) {
    return on_data(g_get_data[K], self, object_size, events, count, flags);
}

std::atomic<std::uint64_t> g_xinput_calls[4]{}, g_xinput_connected[4]{}, g_xinput_blocked{0};
std::atomic<DWORD> g_xinput_thread{0};

DWORD WINAPI xinput_state(DWORD user, XINPUT_STATE* state) {
    const DWORD result = g_xinput_state(user, state);
    if (user < 4) {
        g_xinput_calls[user].fetch_add(1, std::memory_order_relaxed);
        if (result == ERROR_SUCCESS) g_xinput_connected[user].fetch_add(1, std::memory_order_relaxed);
    }
    DWORD expected = 0;
    g_xinput_thread.compare_exchange_strong(expected, GetCurrentThreadId());
    if (result == ERROR_SUCCESS && state && g_block_pads.load(std::memory_order_relaxed)) {
        std::memset(&state->Gamepad, 0, sizeof(state->Gamepad));  // a neutral pad
        g_xinput_blocked.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

// Reads the device methods' addresses from throwaway devices of our own,
// then hooks the methods. Runs on the game thread once DINPUT8 is loaded.
bool g_hooked = false;
void hook_input() {
    g_hooked = true;
    const auto dinput8 = GetModuleHandleW(L"dinput8.dll");
    using Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    const auto create = dinput8 ? reinterpret_cast<Create>(reinterpret_cast<void*>(GetProcAddress(dinput8, "DirectInput8Create"))) : nullptr;
    IDirectInput8W* input = nullptr;
    if (!create || FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, kDirectInput8W,
                                 reinterpret_cast<void**>(&input), nullptr)) || !input) {
        say(AL_LOG_WARNING, "DirectInput8Create failed; DirectInput not watched");
        return;
    }
    void* methods[2][2] = {};  // [keyboard, mouse][GetDeviceState, GetDeviceData]
    const GUID* kinds[2] = {&kSysKeyboard, &kSysMouse};
    for (int k = 0; k < 2; ++k) {
        IDirectInputDevice8W* device = nullptr;
        if (SUCCEEDED(input->CreateDevice(*kinds[k], &device, nullptr)) && device) {
            auto* const* table = *reinterpret_cast<void* const* const*>(device);
            methods[k][0] = table[9];   // GetDeviceState
            methods[k][1] = table[10];  // GetDeviceData
            device->Release();
        }
    }
    input->Release();
    say(AL_LOG_INFO, "DirectInput methods: keyboard GetDeviceState %p GetDeviceData %p; mouse %p %p", methods[0][0],
        methods[0][1], methods[1][0], methods[1][1]);
    if (MH_Initialize() != MH_OK) {
        say(AL_LOG_WARNING, "MinHook did not start; nothing hooked");
        return;
    }
    // One hook per distinct method; a class that shares the keyboard's
    // methods goes through the keyboard's detours.
    GetStateFn* state_originals[2] = {&g_get_state[0], &g_get_state[1]};
    GetDataFn* data_originals[2] = {&g_get_data[0], &g_get_data[1]};
    void* state_detours[2] = {reinterpret_cast<void*>(&get_state<0>), reinterpret_cast<void*>(&get_state<1>)};
    void* data_detours[2] = {reinterpret_cast<void*>(&get_data<0>), reinterpret_cast<void*>(&get_data<1>)};
    unsigned hooked = 0;
    for (int k = 0; k < 2; ++k) {
        if (methods[k][0] && (k == 0 || methods[k][0] != methods[0][0]) &&
            MH_CreateHook(methods[k][0], state_detours[k], reinterpret_cast<void**>(state_originals[k])) == MH_OK) {
            ++hooked;
        }
        if (methods[k][1] && (k == 0 || methods[k][1] != methods[0][1]) &&
            MH_CreateHook(methods[k][1], data_detours[k], reinterpret_cast<void**>(data_originals[k])) == MH_OK) {
            ++hooked;
        }
    }
    const auto xinput = GetModuleHandleW(L"xinput9_1_0.dll");
    void* xinput_target = xinput ? reinterpret_cast<void*>(GetProcAddress(xinput, "XInputGetState")) : nullptr;
    if (xinput_target && MH_CreateHook(xinput_target, reinterpret_cast<void*>(&xinput_state),
                                       reinterpret_cast<void**>(&g_xinput_state)) != MH_OK) {
        xinput_target = nullptr;
    }
    const bool ok = MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    say(ok ? AL_LOG_INFO : AL_LOG_WARNING, "%u DirectInput method(s) hooked%s; XInputGetState %s at %.3f s", hooked,
        ok ? "" : " but enabling failed", xinput_target ? "hooked" : "not found", seconds());
    say(AL_LOG_INFO, "F7 blocks the keyboard at DirectInput, F8 the mouse, F6 XInput pads; press again to undo");
}

// User32, looked up once the window exists, so the probe imports only KERNEL32.
using SetLongFn = LONG_PTR(WINAPI*)(HWND, int, LONG_PTR);
using CallProcFn = LRESULT(WINAPI*)(WNDPROC, HWND, UINT, WPARAM, LPARAM);
SetLongFn g_set_long = nullptr;
CallProcFn g_call_proc = nullptr;
WNDPROC g_game_proc = nullptr;
HWND g_window = nullptr;
std::uint64_t g_key_messages = 0, g_mouse_messages = 0;

void toggle(std::atomic<bool>& flag, const char* what) {
    const bool now = !flag.load();
    flag.store(now);
    say(AL_LOG_INFO, "%s at %.3f s: %s", what, seconds(), now ? "blocked" : "reaches the game again");
}

LRESULT CALLBACK probe_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR) ++g_key_messages;
    if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ++g_mouse_messages;
    if (message == WM_KEYDOWN && !(lparam & (1 << 30))) {
        if (wparam == VK_F7) toggle(g_block_keyboard, "F7: the keyboard at DirectInput");
        if (wparam == VK_F8) toggle(g_block_mouse, "F8: the mouse at DirectInput");
        if (wparam == VK_F6) toggle(g_block_pads, "F6: XInput pads");
    }
    return g_call_proc(g_game_proc, window, message, wparam, lparam);
}

void subclass(HWND window) {
    const auto user32 = GetModuleHandleW(L"user32.dll");
    if (g_window || !window || !user32) return;
    g_set_long = reinterpret_cast<SetLongFn>(reinterpret_cast<void*>(GetProcAddress(user32, "SetWindowLongPtrW")));
    g_call_proc = reinterpret_cast<CallProcFn>(reinterpret_cast<void*>(GetProcAddress(user32, "CallWindowProcW")));
    if (!g_set_long || !g_call_proc) return;
    g_game_proc = reinterpret_cast<WNDPROC>(g_set_long(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&probe_proc)));
    g_window = window;
    say(AL_LOG_INFO, "watching window %p for F6/F7/F8", static_cast<void*>(window));
}

void report(const char* when) {
    say(AL_LOG_INFO, "report %s at %.3f s: %llu key and %llu mouse window messages", when, seconds(),
        static_cast<unsigned long long>(g_key_messages), static_cast<unsigned long long>(g_mouse_messages));
    {
        std::lock_guard lock(g_devices_mutex);
        for (unsigned k = 0; k < g_device_count; ++k) {
            const auto& device = g_devices[k];
            say(AL_LOG_INFO,
                "  %s \"%s\": GetDeviceState %llu calls (size %lu, thread %lu), GetDeviceData %llu calls (thread %lu, "
                "%llu events), blocked %llu times",
                type_name(device.type), device.name, static_cast<unsigned long long>(device.state_calls), device.state_size,
                device.state_thread, static_cast<unsigned long long>(device.data_calls), device.data_thread,
                static_cast<unsigned long long>(device.events), static_cast<unsigned long long>(device.blocked));
        }
    }
    say(AL_LOG_INFO, "  XInputGetState on thread %lu: pad 0 %llu calls (%llu connected), pad 1 %llu, pad 2 %llu, pad 3 %llu; "
        "blocked %llu", g_xinput_thread.load(), static_cast<unsigned long long>(g_xinput_calls[0].load()),
        static_cast<unsigned long long>(g_xinput_connected[0].load()), static_cast<unsigned long long>(g_xinput_calls[1].load()),
        static_cast<unsigned long long>(g_xinput_calls[2].load()), static_cast<unsigned long long>(g_xinput_calls[3].load()),
        static_cast<unsigned long long>(g_xinput_blocked.load()));
}

// The al.frame client side.
std::atomic<std::uint64_t> g_begins{0}, g_posted{0}, g_ran{0};
std::atomic<bool> g_stop{false};
HANDLE g_worker = nullptr;
DWORD g_main_thread = 0;

void AL_CALL on_frame(al_plugin_t*, al_frame_event_t, const al_frame_info_t* frame, void*) {
    g_begins.fetch_add(1, std::memory_order_relaxed);
    // DINPUT8 loads about 8 s in; check twice a second until it is there.
    if (!g_hooked && frame->index % 30 == 0 && GetModuleHandleW(L"dinput8.dll")) hook_input();
    if (frame->index == 3000) report("at frame 3000");
}

void AL_CALL counted(al_plugin_t*, void*) {
    g_ran.fetch_add(1, std::memory_order_relaxed);
}

DWORD WINAPI worker(void*) {
    while (!g_stop.load()) {
        if (g_frame && g_frame->post(g_self, counted, nullptr) == AL_OK) g_posted.fetch_add(1, std::memory_order_relaxed);
        Sleep(250);
    }
    return 0;
}

void AL_CALL on_phase(al_plugin_t*, al_phase_t phase, const void* data) {
    if (phase == AL_PHASE_WIN_MAIN) {
        g_main_thread = GetCurrentThreadId();
        if (g_frame) g_worker = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    } else if (phase == AL_PHASE_SWAPCHAIN_CREATED) {
        const auto* swapchain = static_cast<const al_d3d12_swapchain_t*>(data);
        if (swapchain && swapchain->size >= offsetof(al_d3d12_swapchain_t, window) + sizeof(void*)) {
            subclass(static_cast<HWND>(swapchain->window));
        }
    } else if (phase == AL_PHASE_EXITING) {
        // Stop blocking and stop our own thread while the game is intact.
        g_block_keyboard = g_block_mouse = g_block_pads = false;
        g_stop = true;
        if (g_worker) WaitForSingleObject(g_worker, 2000);
        say(AL_LOG_INFO, "EXITING on thread %lu at %.3f s; worker stopped", GetCurrentThreadId(), seconds());
        report("at EXITING");
    } else if (phase == AL_PHASE_SHUTDOWN) {
        al_frame_status_t status{sizeof(status)};
        if (g_frame) g_frame->status(g_self, &status);
        say(AL_LOG_INFO, "al.frame: state %u, %llu frames; this plugin saw %llu BEGIN; posted %llu tasks, %llu ran",
            status.state, static_cast<unsigned long long>(status.frames), static_cast<unsigned long long>(g_begins.load()),
            static_cast<unsigned long long>(g_posted.load()), static_cast<unsigned long long>(g_ran.load()));
    }
}

const al_plugin_info_t kInfo = {
    sizeof(al_plugin_info_t), AL_API_VERSION, "input_probe", "Astera Loader input probe (diagnostic)", "0.2.1",
    AL_PHASE_BIT(AL_PHASE_WIN_MAIN) | AL_PHASE_BIT(AL_PHASE_SWAPCHAIN_CREATED) | AL_PHASE_BIT(AL_PHASE_EXITING) |
        AL_PHASE_BIT(AL_PHASE_SHUTDOWN),
    0, nullptr, nullptr, nullptr, on_phase, nullptr,
};
}  // namespace

extern "C" AL_PLUGIN_EXPORT const al_plugin_info_t* AL_CALL al_plugin_info(void) {
    return &kInfo;
}

extern "C" AL_PLUGIN_EXPORT al_result_t AL_CALL al_plugin_init(const al_api_t* api, const al_init_t* init) {
    g_api = api;
    g_self = init->self;
    g_start = qpc();
    g_frame = static_cast<const al_frame_t*>(api->get_interface(init->self, AL_FRAME_INTERFACE, AL_FRAME_VERSION));
    if (g_frame) g_frame->subscribe(g_self, AL_FRAME_BEGIN, on_frame, nullptr);
    say(AL_LOG_INFO, "input_probe 0.2.1: al.frame %s", g_frame ? "yes" : "no");
    return AL_OK;
}
