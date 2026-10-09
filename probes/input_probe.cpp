// input_probe: a throwaway diagnostic plugin, not shipped. It answers the
// open questions in docs/design-input-1.6.md without patching any code:
//
// - it subclasses the game window (SetWindowLongPtr, as SharpPluginLoader
//   does) and counts the input and close messages the game gets; close
//   messages are logged the moment they arrive, since Alt+F4 ends the
//   process at once;
// - F9 toggles keeping keyboard messages from the game, F10 mouse messages
//   (raw input included). If the hunter still moves while they are kept
//   away, the game reads that device through DirectInput instead;
// - it lists the raw-input devices the game registered
//   (GetRegisteredRawInputDevices);
// - as a client of "al.frame" (API 1.5) it counts BEGIN/END, posts a task
//   every 250 ms from a worker thread, and stops that thread at EXITING:
//   the pattern plugins with threads should follow.
#include "al/al_plugin.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

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

double seconds_since(std::int64_t from) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart ? static_cast<double>(qpc() - from) / static_cast<double>(frequency.QuadPart) : 0;
}

std::int64_t g_start = 0;

// User32, looked up once the window exists, so the probe imports only KERNEL32.
using SetLongFn = LONG_PTR(WINAPI*)(HWND, int, LONG_PTR);
using GetLongFn = LONG_PTR(WINAPI*)(HWND, int);
using CallProcFn = LRESULT(WINAPI*)(WNDPROC, HWND, UINT, WPARAM, LPARAM);
using DefProcFn = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
using RawDevicesFn = UINT(WINAPI*)(PRAWINPUTDEVICE, PUINT, UINT);
using ThreadOfFn = DWORD(WINAPI*)(HWND, LPDWORD);
using ClassNameFn = int(WINAPI*)(HWND, LPSTR, int);
SetLongFn g_set_long = nullptr;
GetLongFn g_get_long = nullptr;
CallProcFn g_call_proc = nullptr;
DefProcFn g_def_proc = nullptr;
RawDevicesFn g_raw_devices = nullptr;
ThreadOfFn g_thread_of = nullptr;
ClassNameFn g_class_name = nullptr;

HWND g_window = nullptr;
WNDPROC g_game_proc = nullptr;
DWORD g_main_thread = 0;
bool g_keep_keys = false;   // F9
bool g_keep_mouse = false;  // F10
unsigned g_counts[0x400] = {};     // messages below WM_USER, window thread only
unsigned g_kept_keys = 0, g_kept_mouse = 0;

bool keyboard_message(UINT message) {
    return message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR || message == WM_DEADCHAR;
}

bool mouse_message(UINT message) {
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_INPUT;
}

const char* close_name(UINT message, WPARAM wparam) {
    if (message == WM_SYSKEYDOWN && wparam == VK_F4) return "WM_SYSKEYDOWN F4";
    if (message == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_CLOSE) return "WM_SYSCOMMAND SC_CLOSE";
    if (message == WM_CLOSE) return "WM_CLOSE";
    if (message == WM_DESTROY) return "WM_DESTROY";
    if (message == WM_NCDESTROY) return "WM_NCDESTROY";
    if (message == WM_QUERYENDSESSION) return "WM_QUERYENDSESSION";
    if (message == WM_ENDSESSION) return "WM_ENDSESSION";
    return nullptr;
}

LRESULT CALLBACK probe_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message < 0x400) ++g_counts[message];
    if (const char* name = close_name(message, wparam)) {
        say(AL_LOG_INFO, "close: %s at %.3f s on thread %lu (WinMain thread %lu)", name, seconds_since(g_start),
            GetCurrentThreadId(), g_main_thread);
    }
    const bool first_press = message == WM_KEYDOWN && !(lparam & (1 << 30));
    if (first_press && wparam == VK_F9) {
        g_keep_keys = !g_keep_keys;
        say(AL_LOG_INFO, "F9 at %.3f s: keyboard messages %s", seconds_since(g_start),
            g_keep_keys ? "now kept from the game" : "reach the game again");
        return 0;
    }
    if (first_press && wparam == VK_F10) {
        g_keep_mouse = !g_keep_mouse;
        say(AL_LOG_INFO, "F10 at %.3f s: mouse and raw-input messages %s", seconds_since(g_start),
            g_keep_mouse ? "now kept from the game" : "reach the game again");
        return 0;
    }
    if (g_keep_keys && keyboard_message(message) && wparam != VK_F9 && wparam != VK_F10) {
        ++g_kept_keys;
        return 0;
    }
    if (g_keep_mouse && mouse_message(message)) {
        ++g_kept_mouse;
        // WM_INPUT must still reach DefWindowProc so Windows frees its data.
        return message == WM_INPUT ? g_def_proc(window, message, wparam, lparam) : 0;
    }
    return g_call_proc(g_game_proc, window, message, wparam, lparam);
}

void where(const void* address, char* out, std::size_t size) {
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      static_cast<LPCWSTR>(address), &module)) {
        GetModuleFileNameA(module, name, MAX_PATH);
    }
    const char* file = std::strrchr(name, '\\');
    std::snprintf(out, size, "%s+0x%llx", file ? file + 1 : name,
                  static_cast<unsigned long long>(static_cast<const std::uint8_t*>(address) -
                                                  reinterpret_cast<const std::uint8_t*>(module)));
}

void report_raw_input(const char* when) {
    if (!g_raw_devices) return;
    RAWINPUTDEVICE devices[16];
    UINT count = 16;
    const auto got = g_raw_devices(devices, &count, sizeof(RAWINPUTDEVICE));
    if (got == static_cast<UINT>(-1)) {
        say(AL_LOG_INFO, "raw input %s: query failed (%lu), %u registered", when, GetLastError(), count);
        return;
    }
    say(AL_LOG_INFO, "raw input %s: %u device(s) registered", when, got);
    for (UINT k = 0; k < got; ++k) {
        say(AL_LOG_INFO, "  usage page 0x%x usage 0x%x flags 0x%lx target %p", devices[k].usUsagePage,
            devices[k].usUsage, devices[k].dwFlags, static_cast<void*>(devices[k].hwndTarget));
    }
}

void subclass(HWND window) {
    if (g_window || !window) return;
    const auto user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        say(AL_LOG_WARNING, "user32 is not loaded; the window is not watched");
        return;
    }
    g_set_long = reinterpret_cast<SetLongFn>(reinterpret_cast<void*>(GetProcAddress(user32, "SetWindowLongPtrW")));
    g_get_long = reinterpret_cast<GetLongFn>(reinterpret_cast<void*>(GetProcAddress(user32, "GetWindowLongPtrW")));
    g_call_proc = reinterpret_cast<CallProcFn>(reinterpret_cast<void*>(GetProcAddress(user32, "CallWindowProcW")));
    g_def_proc = reinterpret_cast<DefProcFn>(reinterpret_cast<void*>(GetProcAddress(user32, "DefWindowProcW")));
    g_raw_devices = reinterpret_cast<RawDevicesFn>(
        reinterpret_cast<void*>(GetProcAddress(user32, "GetRegisteredRawInputDevices")));
    g_thread_of = reinterpret_cast<ThreadOfFn>(reinterpret_cast<void*>(GetProcAddress(user32, "GetWindowThreadProcessId")));
    g_class_name = reinterpret_cast<ClassNameFn>(reinterpret_cast<void*>(GetProcAddress(user32, "GetClassNameA")));
    if (!g_set_long || !g_get_long || !g_call_proc || !g_def_proc) {
        say(AL_LOG_WARNING, "user32 functions missing; the window is not watched");
        return;
    }
    char class_name[128] = "?";
    if (g_class_name) g_class_name(window, class_name, sizeof(class_name));
    const DWORD window_thread = g_thread_of ? g_thread_of(window, nullptr) : 0;
    const auto before = reinterpret_cast<const void*>(g_get_long(window, GWLP_WNDPROC));
    char owner[MAX_PATH + 32];
    where(before, owner, sizeof(owner));
    g_game_proc = reinterpret_cast<WNDPROC>(g_set_long(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&probe_proc)));
    g_window = window;
    say(AL_LOG_INFO, "window %p class \"%s\" on thread %lu (WinMain thread %lu); its procedure was %s; watching it",
        static_cast<void*>(window), class_name, window_thread, g_main_thread, owner);
    say(AL_LOG_INFO, "F9 toggles keyboard messages, F10 mouse messages, kept from the game");
    report_raw_input("at the swap chain");
}

void report_messages(const char* when) {
    struct Named {
        UINT message;
        const char* name;
    };
    static const Named names[] = {
        {WM_KEYDOWN, "WM_KEYDOWN"},         {WM_KEYUP, "WM_KEYUP"},           {WM_CHAR, "WM_CHAR"},
        {WM_SYSKEYDOWN, "WM_SYSKEYDOWN"},   {WM_SYSKEYUP, "WM_SYSKEYUP"},     {WM_INPUT, "WM_INPUT"},
        {WM_MOUSEMOVE, "WM_MOUSEMOVE"},     {WM_LBUTTONDOWN, "WM_LBUTTONDOWN"}, {WM_RBUTTONDOWN, "WM_RBUTTONDOWN"},
        {WM_MOUSEWHEEL, "WM_MOUSEWHEEL"},   {WM_SETCURSOR, "WM_SETCURSOR"},   {WM_ACTIVATE, "WM_ACTIVATE"},
        {WM_ACTIVATEAPP, "WM_ACTIVATEAPP"}, {WM_SETFOCUS, "WM_SETFOCUS"},     {WM_KILLFOCUS, "WM_KILLFOCUS"},
        {WM_INPUT_DEVICE_CHANGE, "WM_INPUT_DEVICE_CHANGE"}, {WM_IME_COMPOSITION, "WM_IME_COMPOSITION"},
    };
    unsigned total = 0;
    for (unsigned count : g_counts) total += count;
    say(AL_LOG_INFO, "messages %s: %u in all; kept from the game: %u keyboard, %u mouse", when, total, g_kept_keys,
        g_kept_mouse);
    for (const auto& entry : names) {
        if (g_counts[entry.message]) say(AL_LOG_INFO, "  %s %u", entry.name, g_counts[entry.message]);
    }
}

// The al.frame client side.
std::atomic<std::uint64_t> g_begins{0}, g_ends{0}, g_posted{0}, g_ran{0};
std::atomic<bool> g_stop{false};
HANDLE g_worker = nullptr;
DWORD g_begin_thread = 0;

void AL_CALL on_frame(al_plugin_t*, al_frame_event_t event, const al_frame_info_t* frame, void*) {
    if (event == AL_FRAME_BEGIN) {
        if (!g_begins.fetch_add(1, std::memory_order_relaxed)) {
            g_begin_thread = GetCurrentThreadId();
            say(AL_LOG_INFO, "first frame: index %llu on thread %lu at %.3f s", static_cast<unsigned long long>(frame->index),
                g_begin_thread, seconds_since(g_start));
        }
        if (frame->index == 2000) report_raw_input("at frame 2000");
    } else {
        g_ends.fetch_add(1, std::memory_order_relaxed);
    }
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
        // Stop our own thread while the game is intact: this is what keeps
        // plugin threads from reading freed game memory during teardown.
        g_stop = true;
        if (g_worker) WaitForSingleObject(g_worker, 2000);
        say(AL_LOG_INFO, "EXITING on thread %lu at %.3f s (WinMain thread %lu, frame thread %lu); worker stopped",
            GetCurrentThreadId(), seconds_since(g_start), g_main_thread, g_begin_thread);
        report_messages("at EXITING");
        report_raw_input("at EXITING");
    } else if (phase == AL_PHASE_SHUTDOWN) {
        report_messages("at SHUTDOWN");
        al_frame_status_t status{sizeof(status)};
        if (g_frame) g_frame->status(g_self, &status);
        say(AL_LOG_INFO, "al.frame: state %u, %llu frames, game thread %lu; this plugin saw %llu BEGIN, %llu END; "
            "posted %llu tasks, %llu ran", status.state, static_cast<unsigned long long>(status.frames), status.game_thread,
            static_cast<unsigned long long>(g_begins.load()), static_cast<unsigned long long>(g_ends.load()),
            static_cast<unsigned long long>(g_posted.load()), static_cast<unsigned long long>(g_ran.load()));
    }
}

const al_plugin_info_t kInfo = {
    sizeof(al_plugin_info_t), AL_API_VERSION, "input_probe", "Astera Loader input probe (diagnostic)", "0.1.0",
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
    if (g_frame) {
        g_frame->subscribe(g_self, AL_FRAME_BEGIN, on_frame, nullptr);
        g_frame->subscribe(g_self, AL_FRAME_END, on_frame, nullptr);
    }
    say(AL_LOG_INFO, "input_probe: al.frame %s", g_frame ? "yes" : "no");
    return AL_OK;
}
