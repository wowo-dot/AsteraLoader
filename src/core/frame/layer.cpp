#include "core/frame/layer.hpp"
#include "core/frame/clock.hpp"
#include "core/input/service.hpp"
#include "core/plugins/host.hpp"
#include <windows.h>
#include <bit>

namespace al::frame {
namespace {
Layer* active{};
thread_local bool in_frame = false;
using Update = std::uintptr_t (*)(void*, void*, void*, void*);
using Exit = void (*)(void*);
struct FrameScope {
    FrameScope() { in_frame = true; }
    ~FrameScope() { in_frame = false; }
};
// Two reads, no allocation or VirtualQuery on the per-frame path. A null
// main object is allowed; an invalid published pointer makes frames unavailable.
bool exit_flag(const Sites& sites, bool& set) noexcept {
    __try {
        const auto main = *reinterpret_cast<const void* const volatile*>(sites.main_object);
        set = false;
        if (!main) return true;
        const auto at = reinterpret_cast<std::uintptr_t>(main);
        if (at > UINTPTR_MAX - sites.exit_offset) return false;
        set = *reinterpret_cast<const volatile unsigned char*>(at + sites.exit_offset) != 0;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
Layer::Layer(plugins::Host& host, Log log, Warning warning)
    : host_(host), log_(std::move(log)), warning_(std::move(warning)) {
    active = this;
    client_.name = "AsteraLoader";
    client_.log = [this](std::string_view level, std::string_view text) { note(std::string(level) + " " + std::string(text)); };
}
void Layer::note(std::string_view text) noexcept { try { if (log_) log_(text); } catch (...) {} }
void Layer::unavailable(std::string reason, bool warn) noexcept {
    armed_.store(false, std::memory_order_release);
    try {
        auto& registry = host_.game_hooks();
        if (update_handle_) registry.set_enabled(client_, update_handle_, false);
        if (exit_handle_) registry.set_enabled(client_, exit_handle_, false);
        if (title_handle_) registry.set_enabled(client_, title_handle_, false);
        ready_armed_.store(false, std::memory_order_release);
        ready_problem_ = "the frame layer is unavailable: " + reason;
        host_.frame_clock().set_unavailable(reason);
        if(host_.input_service().wanted()) host_.input_service().unavailable("frame layer: "+reason);
        note("frame_layer_unavailable reason=" + reason);
        if (warn && warning_) warning_(reason);
    } catch (...) {}
}
void Layer::prepare(Sites sites, bool warn_unavailable) {
    sites_ = std::move(sites);
    if (!sites_.problem.empty()) { unavailable(sites_.problem, warn_unavailable); return; }
    if (!sites_.update.target || !sites_.exiting.target || !sites_.main_object) { unavailable("incomplete frame sites"); return; }
    auto& registry = host_.game_hooks();
    std::string problem;
    const al_hook_t update_hook{sizeof(update_hook), -500, "Main:Update", sites_.update.pattern.c_str(), sites_.update.offset, 0,
        sites_.update.target, std::bit_cast<void*>(&update), &update_next_};
    update_handle_ = registry.hook(client_, update_hook, problem);
    if (!update_handle_) { unavailable("Main:Update: " + problem); return; }
    const al_hook_t exit_hook{sizeof(exit_hook), -500, "Game:Exiting", sites_.exiting.pattern.c_str(), sites_.exiting.offset, 0,
        sites_.exiting.target, std::bit_cast<void*>(&exiting), &exit_next_};
    exit_handle_ = registry.hook(client_, exit_hook, problem);
    if (!exit_handle_) { unavailable("Game:Exiting: " + problem); return; }
    // GAME_READY's site is optional: without it, frames and EXITING still work.
    if (!sites_.title.target) { ready_unavailable("no title site for this executable"); return; }
    const al_hook_t title_hook{sizeof(title_hook), -500, "Area:TitleStart", sites_.title.pattern.c_str(), sites_.title.offset, 0,
        sites_.title.target, std::bit_cast<void*>(&title), &title_next_};
    title_handle_ = registry.hook(client_, title_hook, problem);
    if (!title_handle_) ready_unavailable("Area:TitleStart: " + problem);
}
void Layer::ready_unavailable(std::string reason) noexcept {
    ready_armed_.store(false, std::memory_order_release);
    try {
        if (title_handle_) host_.game_hooks().set_enabled(client_, title_handle_, false);
        ready_problem_ = reason;
        note("game_ready_unavailable reason=" + reason);
    } catch (...) {}
}
void Layer::installed() {
    if (host_.frame_clock().state() == AL_FRAME_UNAVAILABLE) return;
    if (!update_handle_ || !exit_handle_ || host_.game_hooks().state(client_, update_handle_) != AL_HOOK_ACTIVE ||
        host_.game_hooks().state(client_, exit_handle_) != AL_HOOK_ACTIVE) {
        unavailable("the frame or exit hook could not be activated"); return;
    }
    armed_.store(true, std::memory_order_release);
    note("frame_layer_armed order=-500; EXITING precedes the game exit flag");
    if (!title_handle_) return;
    if (host_.game_hooks().state(client_, title_handle_) != AL_HOOK_ACTIVE) { ready_unavailable("the title hook could not be activated"); return; }
    ready_problem_.clear();
    ready_armed_.store(true, std::memory_order_release);
    note("game_ready_armed site=Area:TitleStart");
}
void Layer::win_main(std::uint32_t thread) noexcept { win_main_.store(thread, std::memory_order_release); }
void Layer::notify(std::string_view source) noexcept {
    try {
        if (host_.raise_exiting()) note("phase_exiting source=" + std::string(source) + " thread_id=" + std::to_string(GetCurrentThreadId()));
    } catch (...) { unavailable("EXITING dispatch failed"); }
}
std::uintptr_t Layer::update(void* main, void* a2, void* a3, void* a4) {
    auto& layer = *active;
    const auto next = std::bit_cast<Update>(layer.update_next_);
    if (!layer.armed_.load(std::memory_order_acquire) || in_frame) return next(main, a2, a3, a4);
    auto& clock = layer.host_.frame_clock();
    const auto state = clock.state();
    if (state != AL_FRAME_WAITING && state != AL_FRAME_RUNNING) return next(main, a2, a3, a4);
    const auto thread = GetCurrentThreadId();
    auto expected = 0u;
    layer.frame_thread_.compare_exchange_strong(expected, thread, std::memory_order_relaxed);
    if (layer.frame_thread_.load(std::memory_order_relaxed) != thread) return next(main, a2, a3, a4);
    FrameScope scope;
    const auto incoming_error = GetLastError();
    layer.host_.input_service().begin_frame(thread);
    clock.begin_frame();
    SetLastError(incoming_error);
    const auto result = next(main, a2, a3, a4);
    const auto returned_error = GetLastError();
    layer.host_.input_service().end_frame();
    clock.end_frame();
    if (!layer.ready_done_ && layer.title_started_.load(std::memory_order_acquire)) layer.ready();
    bool set = false;
    if (!exit_flag(layer.sites_, set)) layer.unavailable("the game's frame-loop exit flag is unreadable");
    else if (set) { layer.notify("frame_backup"); clock.last_frame(); }
    SetLastError(returned_error);
    return result;
}
// The title area started during this frame: GAME_READY, once, between frames.
void Layer::ready() noexcept {
    ready_done_ = true;
    try {
        al_frame_status_t status{}; status.size = sizeof(status); host_.frame_clock().status(status);
        const al_phase_game_ready_t payload{sizeof(payload), AL_GAME_READY_TITLE, status.frames ? status.frames - 1 : 0};
        if (!host_.raise_game_ready(payload)) return;
        ready_fired_ = true;
        note("phase_game_ready source=title frame=" + std::to_string(payload.frame) + " thread_id=" + std::to_string(GetCurrentThreadId()));
    } catch (...) {}
}
// aTitle's start runs once the title area has loaded, and again after a
// return to the title; only the first time matters.
std::uintptr_t Layer::title(void* area, void* a2, void* a3, void* a4) {
    auto& layer = *active;
    const auto result = std::bit_cast<Update>(layer.title_next_)(area, a2, a3, a4);
    layer.title_starts_.fetch_add(1, std::memory_order_relaxed);
    if (layer.ready_armed_.load(std::memory_order_acquire)) layer.title_started_.store(true, std::memory_order_release);
    return result;
}
std::string Layer::ready_summary() const {
    return "game_ready fired=" + std::to_string(ready_fired_ ? 1 : 0) + " title_starts=" +
        std::to_string(title_starts_.load(std::memory_order_relaxed)) + (ready_problem_.empty() ? "" : " reason=" + ready_problem_);
}
void Layer::exiting(void* main) {
    auto& layer = *active;
    const auto incoming_error = GetLastError();
    if (layer.armed_.load(std::memory_order_acquire) && GetCurrentThreadId() == layer.win_main_.load(std::memory_order_acquire)) {
        al_frame_status_t status{}; status.size = sizeof(status); layer.host_.frame_clock().status(status);
        if (status.frames && status.state != AL_FRAME_UNAVAILABLE) layer.notify("exit_site");
    }
    SetLastError(incoming_error);
    std::bit_cast<Exit>(layer.exit_next_)(main);
}
}  // namespace al::frame
