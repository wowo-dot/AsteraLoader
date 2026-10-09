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
    if (!exit_handle_) unavailable("Game:Exiting: " + problem);
}
void Layer::installed() {
    if (host_.frame_clock().state() == AL_FRAME_UNAVAILABLE) return;
    if (!update_handle_ || !exit_handle_ || host_.game_hooks().state(client_, update_handle_) != AL_HOOK_ACTIVE ||
        host_.game_hooks().state(client_, exit_handle_) != AL_HOOK_ACTIVE) {
        unavailable("the frame or exit hook could not be activated"); return;
    }
    armed_.store(true, std::memory_order_release);
    note("frame_layer_armed order=-500; EXITING precedes the game exit flag");
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
    bool set = false;
    if (!exit_flag(layer.sites_, set)) layer.unavailable("the game's frame-loop exit flag is unreadable");
    else if (set) { layer.notify("frame_backup"); clock.last_frame(); }
    SetLastError(returned_error);
    return result;
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
