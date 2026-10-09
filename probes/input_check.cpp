// input_check: a test plugin for API 1.6's al.input, not shipped. It uses
// the service the way a menu would, so one run in MHW shows whether the
// input layer works:
// - Ctrl+F7 toggles keyboard and mouse capture. The hotkey still fires while
//   they are captured: a plugin's own capture never suppresses its hotkeys.
// - L3+R3 (both sticks clicked) on pad 0 toggles pad capture.
// - While the keyboard is captured, typed characters (WM_CHAR) are kept
//   from the game, as a menu's text box would; every other message passes.
// It logs counts and states only, never keys or typed text: each toggle as
// it happens, the input layer's state when it changes, and totals at
// EXITING and SHUTDOWN.
#include "al/al_plugin.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace {
const al_api_t* g_api = nullptr;
al_plugin_t* g_self = nullptr;
const al_input_t* g_input = nullptr;
const al_frame_t* g_frame = nullptr;

constexpr std::uint32_t kKeyboardMouse = AL_INPUT_KEYBOARD | AL_INPUT_MOUSE;

// Capture state, changed on the game thread only (hotkey callbacks). The
// plugin holds at most one capture at a time, so release always undoes it.
std::atomic<std::uint32_t> g_mask{0};
bool g_holding = false;

std::atomic<std::uint32_t> g_win_main_thread{0};

struct Counts {
    std::atomic<std::uint64_t> frames, frames_keyboard_mouse, frames_pad, pad0_connected;
    std::atomic<std::uint64_t> keyboard_hotkeys, pad_hotkeys, hotkeys_off_game_thread, hotkey_state_mismatch;
    std::atomic<std::uint64_t> captures, refusals, releases;
    std::atomic<std::uint64_t> messages, off_window_thread, other_window;
    std::atomic<std::uint64_t> mouse_moves, mouse_buttons, key_messages, chars;
    std::atomic<std::uint64_t> mouse_moves_captured, chars_consumed;
} g;

// The input layer's state as last logged, checked every 30 frames.
std::uint32_t g_seen_state = 0xFFFFFFFFu, g_seen_devices = 0xFFFFFFFFu;

void say(al_log_level_t level, const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    g_api->log(g_self, level, text);
}

const char* text(const char* value) { return value ? value : ""; }

const char* state_name(std::uint32_t state) {
    switch (state) {
        case AL_INPUT_WAITING: return "waiting";
        case AL_INPUT_RUNNING: return "running";
        case AL_INPUT_UNAVAILABLE: return "unavailable";
        case AL_INPUT_STOPPED: return "stopped";
        default: return "unknown";
    }
}

al_input_status_t input_status() {
    al_input_status_t status{};
    status.size = sizeof(status);
    g_input->status(g_self, &status);
    return status;
}

std::uint64_t frame_index() {
    al_frame_status_t status{};
    status.size = sizeof(status);
    g_frame->status(g_self, &status);
    return status.frames;
}

bool on_game_thread() {
    al_frame_status_t status{};
    status.size = sizeof(status);
    g_frame->status(g_self, &status);
    return status.game_thread == GetCurrentThreadId();
}

// Moves from the current capture to `wanted`: release what is held, then
// capture the new set. If AL refuses it, the old set is taken again.
void apply(std::uint32_t wanted) {
    const auto old = g_mask.load();
    if (g_holding && g_input->release(g_self) == AL_OK) ++g.releases;
    g_holding = false;
    g_mask = 0;
    if (!wanted) return;
    if (g_input->capture(g_self, wanted) == AL_OK) {
        g_holding = true;
        g_mask = wanted;
        ++g.captures;
        return;
    }
    ++g.refusals;
    const auto status = input_status();
    say(AL_LOG_WARNING, "capture of devices 0x%x refused; input %s, devices 0x%x%s%s", wanted,
        state_name(status.state), status.devices, *text(status.reason) ? ": " : "", text(status.reason));
    if (old && g_input->capture(g_self, old) == AL_OK) {
        g_holding = true;
        g_mask = old;
    }
}

void report_toggle(const char* what) {
    const auto mask = g_mask.load();
    say(AL_LOG_INFO, "%s at frame %llu: now capturing keyboard/mouse %s, pad %s", what,
        static_cast<unsigned long long>(frame_index()), (mask & kKeyboardMouse) ? "yes" : "no",
        (mask & AL_INPUT_PAD) ? "yes" : "no");
}

void AL_CALL on_keyboard_hotkey(al_plugin_t*, std::uint32_t, void*) {
    ++g.keyboard_hotkeys;
    if (!on_game_thread()) ++g.hotkeys_off_game_thread;
    if (!g_input->key_down(g_self, VK_F7) || !g_input->key_down(g_self, VK_CONTROL)) ++g.hotkey_state_mismatch;
    apply(g_mask.load() ^ kKeyboardMouse);
    report_toggle("Ctrl+F7");
}

void AL_CALL on_pad_hotkey(al_plugin_t*, std::uint32_t, void*) {
    ++g.pad_hotkeys;
    if (!on_game_thread()) ++g.hotkeys_off_game_thread;
    al_pad_state_t pad{};
    pad.size = sizeof(pad);
    constexpr std::uint32_t kSticks = AL_PAD_LEFT_THUMB | AL_PAD_RIGHT_THUMB;
    if (g_input->pad_state(g_self, 0, &pad) != AL_OK || (pad.buttons & kSticks) != kSticks) ++g.hotkey_state_mismatch;
    apply(g_mask.load() ^ AL_INPUT_PAD);
    report_toggle("L3+R3");
}

std::uint32_t AL_CALL on_message(al_plugin_t*, void* hwnd, std::uint32_t message, std::uintptr_t, std::intptr_t,
                                 void*) {
    ++g.messages;
    const auto win_main = g_win_main_thread.load();
    if (win_main && GetCurrentThreadId() != win_main) ++g.off_window_thread;
    if (hwnd != g_input->window(g_self)) ++g.other_window;
    const auto captured = g_mask.load();
    if (message == WM_MOUSEMOVE) {
        ++g.mouse_moves;
        if (captured & AL_INPUT_MOUSE) ++g.mouse_moves_captured;
    } else if ((message >= WM_LBUTTONDOWN && message <= WM_MBUTTONDBLCLK) ||
               (message >= WM_XBUTTONDOWN && message <= WM_XBUTTONDBLCLK) || message == WM_MOUSEWHEEL) {
        ++g.mouse_buttons;
    } else if (message == WM_CHAR) {
        ++g.chars;
        if (captured & AL_INPUT_KEYBOARD) {
            ++g.chars_consumed;
            return 1;  // as a menu's text box: the game does not get it
        }
    } else if (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) {
        ++g.key_messages;
    }
    return 0;
}

void AL_CALL on_frame_end(al_plugin_t*, al_frame_event_t, const al_frame_info_t* frame, void*) {
    ++g.frames;
    const auto mask = g_mask.load();
    if (mask & kKeyboardMouse) ++g.frames_keyboard_mouse;
    if (mask & AL_INPUT_PAD) ++g.frames_pad;
    al_pad_state_t pad{};
    pad.size = sizeof(pad);
    if (g_input->pad_state(g_self, 0, &pad) == AL_OK && pad.connected) ++g.pad0_connected;
    if (frame->index % 30) return;
    const auto status = input_status();
    if (status.state == g_seen_state && status.devices == g_seen_devices) return;
    g_seen_state = status.state;
    g_seen_devices = status.devices;
    say(AL_LOG_INFO, "input %s at frame %llu: devices 0x%x, window %s%s%s", state_name(status.state),
        static_cast<unsigned long long>(frame->index), status.devices, status.window ? "known" : "not yet",
        *text(status.reason) ? ", reason: " : "", text(status.reason));
}

void report(const char* moment) {
    const auto status = input_status();
    say(AL_LOG_INFO, "%s: input %s, devices 0x%x, captured 0x%x, window %s%s%s", moment, state_name(status.state),
        status.devices, status.captured, status.window ? "known" : "none", *text(status.reason) ? ", reason: " : "",
        text(status.reason));
    say(AL_LOG_INFO,
        "%s: frames %llu (keyboard/mouse captured %llu, pad captured %llu, pad 0 connected %llu); hotkeys: "
        "Ctrl+F7 %llu, L3+R3 %llu, off the game thread %llu, state mismatches %llu; captures %llu, refused %llu, "
        "released %llu",
        moment, static_cast<unsigned long long>(g.frames.load()),
        static_cast<unsigned long long>(g.frames_keyboard_mouse.load()),
        static_cast<unsigned long long>(g.frames_pad.load()), static_cast<unsigned long long>(g.pad0_connected.load()),
        static_cast<unsigned long long>(g.keyboard_hotkeys.load()), static_cast<unsigned long long>(g.pad_hotkeys.load()),
        static_cast<unsigned long long>(g.hotkeys_off_game_thread.load()),
        static_cast<unsigned long long>(g.hotkey_state_mismatch.load()),
        static_cast<unsigned long long>(g.captures.load()), static_cast<unsigned long long>(g.refusals.load()),
        static_cast<unsigned long long>(g.releases.load()));
    say(AL_LOG_INFO,
        "%s: messages %llu (off the WinMain thread %llu, another window %llu): mouse moves %llu (%llu while the "
        "mouse was captured), mouse buttons %llu, key messages %llu, characters %llu (%llu kept from the game)",
        moment, static_cast<unsigned long long>(g.messages.load()),
        static_cast<unsigned long long>(g.off_window_thread.load()),
        static_cast<unsigned long long>(g.other_window.load()), static_cast<unsigned long long>(g.mouse_moves.load()),
        static_cast<unsigned long long>(g.mouse_moves_captured.load()),
        static_cast<unsigned long long>(g.mouse_buttons.load()), static_cast<unsigned long long>(g.key_messages.load()),
        static_cast<unsigned long long>(g.chars.load()), static_cast<unsigned long long>(g.chars_consumed.load()));
}

void AL_CALL on_phase(al_plugin_t*, al_phase_t phase, const void*) {
    if (phase == AL_PHASE_WIN_MAIN) {
        g_win_main_thread = GetCurrentThreadId();
    } else if (phase == AL_PHASE_EXITING) {
        report("exiting");
    } else if (phase == AL_PHASE_SHUTDOWN) {
        report("shutdown");
    }
}

const al_plugin_info_t kInfo = {
    sizeof(al_plugin_info_t),
    AL_API_VERSION,
    "input_check",
    "Astera Loader input check",
    "0.1.0",
    AL_PHASE_BIT(AL_PHASE_WIN_MAIN) | AL_PHASE_BIT(AL_PHASE_EXITING) | AL_PHASE_BIT(AL_PHASE_SHUTDOWN),
    0,
    nullptr,
    nullptr,
    nullptr,
    on_phase,
    nullptr,
};
}  // namespace

extern "C" AL_PLUGIN_EXPORT const al_plugin_info_t* AL_CALL al_plugin_info(void) {
    return &kInfo;
}

extern "C" AL_PLUGIN_EXPORT al_result_t AL_CALL al_plugin_init(const al_api_t* api, const al_init_t* init) {
    g_api = api;
    g_self = init->self;
    g_input = static_cast<const al_input_t*>(api->get_interface(g_self, AL_INPUT_INTERFACE, AL_INPUT_VERSION));
    g_frame = static_cast<const al_frame_t*>(api->get_interface(g_self, AL_FRAME_INTERFACE, AL_FRAME_VERSION));
    if (!g_input || !g_frame) {
        api->set_status(g_self, AL_STATUS_DISABLED, "needs al.input 1.0 and al.frame 1.0 (API 1.6)");
        return AL_ERROR;
    }
    const al_hotkey_t keyboard = {sizeof(al_hotkey_t), VK_F7, AL_MOD_CTRL, 0, "Toggle keyboard/mouse capture"};
    const al_pad_hotkey_t pad = {sizeof(al_pad_hotkey_t), 0, AL_PAD_LEFT_THUMB | AL_PAD_RIGHT_THUMB, 0,
                                 "Toggle pad capture"};
    const bool ok = g_input->add_hotkey(g_self, &keyboard, on_keyboard_hotkey, nullptr) &&
                    g_input->add_pad_hotkey(g_self, &pad, on_pad_hotkey, nullptr) &&
                    g_input->subscribe_messages(g_self, 0, on_message, nullptr) == AL_OK &&
                    g_frame->subscribe(g_self, AL_FRAME_END, on_frame_end, nullptr);
    if (!ok) {
        api->set_status(g_self, AL_STATUS_DISABLED, "could not register its hotkeys or callbacks; see its log");
        return AL_ERROR;
    }
    say(AL_LOG_INFO, "input_check 0.1.0: Ctrl+F7 toggles keyboard/mouse capture, L3+R3 on pad 0 toggles pad capture");
    return AL_OK;
}
