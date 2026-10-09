#include "frame_fixture.hpp"
#include "al/al_plugin.h"
#include <windows.h>
#include <bit>
#include <cstdio>
#include <string>

namespace {
const al_api_t* api{};
const al_frame_t* frame{};
al_plugin_t* self{};
FrameRecord record{};
void* next{};
std::uint64_t begins{}, ends{}, tasks{}, exit_calls{}, ready_calls{};
DWORD frame_thread{}, exit_thread{};
bool valid = true;
std::uintptr_t update(void* main, void* a2, void* a3, void* a4) {
#ifdef FRAME_REFLEX
    record(OuterBegin, 0);
#else
    record(InnerBegin, 0);
#endif
    const auto result = std::bit_cast<FrameUpdate>(next)(main, a2, a3, a4);
#ifdef FRAME_REFLEX
    record(OuterEnd, 0);
#else
    record(InnerEnd, 0);
#endif
    return result;
}
#ifndef FRAME_REFLEX
void AL_CALL task(al_plugin_t*, void* user) {
    ++tasks;
    if (!frame_thread) frame_thread = GetCurrentThreadId();
    valid = valid && GetCurrentThreadId() == frame_thread;
    const auto final = reinterpret_cast<std::uintptr_t>(user) == 2;
    record(final ? TaskFinal : TaskInitial, 0);
    if (final) valid = valid && frame->post(self, task, nullptr) == AL_ERROR;
    SetLastError(0xbad);
}
void AL_CALL event(al_plugin_t*, al_frame_event_t kind, const al_frame_info_t* info, void*) {
    if (!frame_thread) frame_thread = GetCurrentThreadId();
    valid = valid && frame_thread == GetCurrentThreadId() && info && info->size == sizeof(*info);
    if (kind == AL_FRAME_BEGIN) {
        valid = valid && info->index == begins;
        ++begins; record(Begin, info->index);
    } else {
        valid = valid && info->index == ends;
        ++ends; record(End, info->index);
        if (info->index == 2) {
            valid = valid && frame->post(self, task, reinterpret_cast<void*>(2)) == AL_OK;
            using Wait = BOOL (*)();
            const auto wait = std::bit_cast<Wait>(GetProcAddress(GetModuleHandleW(nullptr), "al_test_end_wait"));
            valid = valid && wait && wait();
        }
    }
    SetLastError(0xbad);
}
void AL_CALL phase(al_plugin_t*, al_phase_t id, const void* data) {
    if (id == AL_PHASE_GAME_READY) {
        const auto* ready = static_cast<const al_phase_game_ready_t*>(data);
        ++ready_calls;
        valid = valid && ready && ready->size == sizeof(*ready) && ready->source == AL_GAME_READY_TITLE &&
            ready->frame == 1 && ends == 2 && GetCurrentThreadId() == frame_thread;
        record(GameReady, ready ? ready->frame : 99);
    }
    if (id == AL_PHASE_EXITING) {
        ++exit_calls; exit_thread = GetCurrentThreadId(); record(Exiting, 0);
    }
    const auto ok = valid && begins == 3 && ends == 3 && tasks == 2 && exit_calls == 1 && ready_calls == 1;
    if (id == AL_PHASE_SHUTDOWN) api->log(self, ok ? AL_LOG_INFO : AL_LOG_ERROR, ok ? "shutdown_frame_valid=1" : "shutdown_frame_valid=0");
}
#else
void AL_CALL phase(al_plugin_t*, al_phase_t, const void*) {}
#endif
#ifdef FRAME_REFLEX
constexpr const char* name = "frame_reflex";
constexpr int order = -1000;
#else
constexpr const char* name = "frame_probe";
constexpr int order = 0;
#endif
const al_plugin_info_t info{sizeof(info), AL_API_VERSION, name, name, "0.0.1",
    AL_PHASE_BIT(AL_PHASE_EXITING) | AL_PHASE_BIT(AL_PHASE_SHUTDOWN)
#ifndef FRAME_REFLEX
        | AL_PHASE_BIT(AL_PHASE_GAME_READY)
#endif
    , 0, nullptr, nullptr, nullptr, phase, nullptr};
}
extern "C" AL_PLUGIN_EXPORT const al_plugin_info_t* AL_CALL al_plugin_info() { return &info; }
extern "C" AL_PLUGIN_EXPORT al_result_t AL_CALL al_plugin_init(const al_api_t* table, const al_init_t* init) {
    api = table; self = init->self;
    const auto module = GetModuleHandleW(nullptr);
    const auto target = std::bit_cast<void*>(GetProcAddress(module, "al_test_frame"));
    record = std::bit_cast<FrameRecord>(GetProcAddress(module, "al_test_frame_record"));
    const auto hooks = static_cast<const al_hooks_t*>(api->get_interface(self, AL_HOOKS_INTERFACE, AL_HOOKS_VERSION));
    if (!hooks || !target || !record) return AL_ERROR;
    std::string pattern;
    for (unsigned i = 0; i < 32; ++i) {
        char text[4]{}; std::snprintf(text, sizeof(text), "%02X", static_cast<const unsigned char*>(target)[i]);
        if (i) pattern += ' '; pattern += text;
    }
    const al_hook_t hook{sizeof(hook), order, "Main:Update", pattern.c_str(), 0, 0, target, std::bit_cast<void*>(&update), &next};
    if (!hooks->hook(self, &hook)) return AL_ERROR;
#ifndef FRAME_REFLEX
    frame = static_cast<const al_frame_t*>(api->get_interface(self, AL_FRAME_INTERFACE, AL_FRAME_VERSION));
    if (!frame || !frame->subscribe(self, AL_FRAME_BEGIN, event, nullptr) || !frame->subscribe(self, AL_FRAME_END, event, nullptr)) return AL_ERROR;
#endif
    return AL_OK;
}
#ifndef FRAME_REFLEX
extern "C" __declspec(dllexport) BOOL al_test_post_initial() {
    // Runs on a separate poster before the first frame, but records where
    // the task actually runs using the game-thread record export.
    return frame->post(self, task, reinterpret_cast<void*>(1)) == AL_OK;
}
extern "C" __declspec(dllexport) BOOL al_test_frame_validate(DWORD game_thread, DWORD expected_exit_thread) {
    al_frame_status_t status{}; status.size = sizeof(status); frame->status(self, &status);
    return valid && begins == 3 && ends == 3 && tasks == 2 && exit_calls == 1 && ready_calls == 1 && exit_thread == expected_exit_thread &&
        status.state == AL_FRAME_STOPPED && status.frames == 3 && status.game_thread == game_thread &&
        frame->post(self, task, nullptr) == AL_ERROR;
}
#endif
