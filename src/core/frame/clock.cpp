#include "core/frame/clock.hpp"

#include "core/plugins/guard.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace al::frame {
namespace {
std::int64_t qpc() noexcept {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

std::int64_t ticks_per_us() noexcept {
    static const std::int64_t value = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart >= 1000000 ? frequency.QuadPart / 1000000 : 1;
    }();
    return value;
}

const char* event_name(al_frame_event_t event) noexcept {
    return event == AL_FRAME_BEGIN ? "begin" : "end";
}

const char* state_name(al_frame_state_t state) noexcept {
    switch (state) {
    case AL_FRAME_WAITING: return "waiting";
    case AL_FRAME_RUNNING: return "running";
    case AL_FRAME_STOPPED: return "stopped";
    default: return "unavailable";
    }
}

bool skipped(const Client& client) noexcept {
    return client.stopped && client.stopped->load(std::memory_order_relaxed);
}
}  // namespace

struct Clock::Stats {
    Client* client = nullptr;
    std::atomic<std::uint64_t> posted{0}, run{0}, dropped{0};
};

struct Clock::Subscription {
    Client* client = nullptr;
    al_frame_event_t event = AL_FRAME_BEGIN;
    al_frame_fn callback = nullptr;
    void* user = nullptr;
    std::uint32_t handle = 0;
    bool removed = false;  // under mutex_
    std::atomic<std::uint64_t> calls{0}, ticks{0}, max_ticks{0};
};

// Published lists never change; a new one replaces them.
struct Clock::List {
    std::vector<Subscription*> items;
};

Clock::Clock() {
    queue_.reserve(64);
    running_.reserve(64);
}

Clock::~Clock() = default;

std::uint32_t Clock::subscribe(Client& client, al_frame_event_t event, al_frame_fn callback, void* user,
                               std::string& problem) {
    problem.clear();
    if (!callback) {
        problem = "no callback";
        return 0;
    }
    if (event != AL_FRAME_BEGIN && event != AL_FRAME_END) {
        problem = "unknown frame event " + std::to_string(event);
        return 0;
    }
    std::lock_guard lock(mutex_);
    auto subscription = std::make_unique<Subscription>();
    subscription->client = &client;
    subscription->event = event;
    subscription->callback = callback;
    subscription->user = user;
    subscription->handle = next_handle_++;
    const auto handle = subscription->handle;
    subscriptions_.push_back(std::move(subscription));
    publish(event);
    return handle;
}

std::string Clock::unsubscribe(const Client& client, std::uint32_t handle) {
    std::lock_guard lock(mutex_);
    const auto found = std::find_if(subscriptions_.begin(), subscriptions_.end(),
                                    [&](const auto& subscription) { return subscription->handle == handle; });
    if (found == subscriptions_.end()) return "unknown handle";
    auto& subscription = **found;
    if (subscription.client->self != client.self) return "the handle belongs to another plugin";
    if (subscription.removed) return {};
    subscription.removed = true;
    publish(subscription.event);
    return {};
}

void Clock::publish(al_frame_event_t event) {
    auto list = std::make_unique<List>();
    for (const auto& subscription : subscriptions_) {
        if (subscription->event == event && !subscription->removed) list->items.push_back(subscription.get());
    }
    // Load order first; within a plugin, the order it subscribed in.
    std::stable_sort(list->items.begin(), list->items.end(), [](const Subscription* a, const Subscription* b) {
        return a->client->load_index < b->client->load_index;
    });
    lists_[event].store(list.get(), std::memory_order_release);
    lists_kept_.push_back(std::move(list));
}

Clock::Stats& Clock::stats_for(Client& client) {
    for (const auto& stats : stats_) {
        if (stats->client == &client) return *stats;
    }
    stats_.push_back(std::make_unique<Stats>());
    stats_.back()->client = &client;
    return *stats_.back();
}

std::string Clock::post(Client& client, al_task_fn task, void* user) {
    if (!task) return "no task";
    std::lock_guard lock(mutex_);
    const auto state = state_.load(std::memory_order_acquire);
    if (state == AL_FRAME_STOPPED) return "the game's frames have stopped";
    if (state == AL_FRAME_UNAVAILABLE) return "frames are unavailable: " + reason_;
    auto& stats = stats_for(client);
    queue_.push_back({&client, task, user, &stats});
    stats.posted.fetch_add(1, std::memory_order_relaxed);
    queued_.fetch_add(1, std::memory_order_release);
    return {};
}

void Clock::status(al_frame_status_t& out) const {
    al_frame_status_t full{};
    full.size = sizeof(full);
    full.state = state_.load(std::memory_order_acquire);
    full.frames = frames_.load(std::memory_order_relaxed);
    full.game_thread = game_thread_.load(std::memory_order_relaxed);
    full.reason = full.state == AL_FRAME_UNAVAILABLE ? reason_.c_str() : "";
    const auto size = (std::min)(static_cast<std::size_t>(out.size), sizeof(full));
    std::memcpy(&out, &full, size);
    out.size = static_cast<std::uint32_t>(size);
}

void Clock::begin_frame() noexcept {
    auto state = state_.load(std::memory_order_acquire);
    if (state == AL_FRAME_WAITING) {
        game_thread_.store(GetCurrentThreadId(), std::memory_order_relaxed);
        state_.compare_exchange_strong(state, AL_FRAME_RUNNING, std::memory_order_acq_rel);
        state = state_.load(std::memory_order_acquire);
    }
    if (state != AL_FRAME_RUNNING) return;
    const auto start = qpc();
    if (queued_.load(std::memory_order_acquire)) run_tasks();
    run(AL_FRAME_BEGIN, start);
    frames_.fetch_add(1, std::memory_order_relaxed);
}

void Clock::end_frame() noexcept {
    if (state_.load(std::memory_order_acquire) != AL_FRAME_RUNNING) return;
    run(AL_FRAME_END, qpc());
}

void Clock::run(al_frame_event_t event, std::int64_t start) noexcept {
    const auto* list = lists_[event].load(std::memory_order_acquire);
    if (!list || list->items.empty()) return;
    al_frame_info_t info{};
    info.size = sizeof(info);
    info.index = frames_.load(std::memory_order_relaxed) - (event == AL_FRAME_END ? 1 : 0);
    info.qpc = start;
    for (auto* subscription : list->items) {
        auto& client = *subscription->client;
        if (skipped(client)) continue;
        unsigned long fault = 0;
        const auto before = qpc();
        const bool returned =
            plugins::guarded_frame_event(subscription->callback, client.self, event, &info, subscription->user, fault);
        const auto ticks = static_cast<std::uint64_t>(qpc() - before);
        subscription->calls.fetch_add(1, std::memory_order_relaxed);
        subscription->ticks.fetch_add(ticks, std::memory_order_relaxed);
        if (ticks > subscription->max_ticks.load(std::memory_order_relaxed)) {
            subscription->max_ticks.store(ticks, std::memory_order_relaxed);  // only this thread writes it
        }
        if (!returned && client.disable) {
            try {
                client.disable(std::string("its frame ") + event_name(event) + " callback raised exception " +
                               std::to_string(fault));
            } catch (...) {
            }
        }
    }
}

void Clock::run_tasks() noexcept {
    {
        std::lock_guard lock(mutex_);
        running_.swap(queue_);
        queued_.store(0, std::memory_order_relaxed);
    }
    run_swapped();
}

void Clock::run_swapped() noexcept {
    for (const auto& task : running_) {
        auto& client = *task.client;
        if (skipped(client)) {
            task.stats->dropped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        unsigned long fault = 0;
        const bool returned = plugins::guarded_task(task.task, client.self, task.user, fault);
        task.stats->run.fetch_add(1, std::memory_order_relaxed);
        if (!returned && client.disable) {
            try {
                client.disable("its game-thread task raised exception " + std::to_string(fault));
            } catch (...) {
            }
        }
    }
    running_.clear();  // keeps the storage for the next swap
}

void Clock::last_frame() noexcept {
    // Stop taking tasks and take what is queued in one step, then run it on
    // the game thread while the game is intact. A task posted from now on,
    // also by these tasks, is refused.
    {
        std::lock_guard lock(mutex_);
        if (state_.load(std::memory_order_acquire) != AL_FRAME_RUNNING) return;
        state_.store(AL_FRAME_STOPPED, std::memory_order_release);
        running_.swap(queue_);
        queued_.store(0, std::memory_order_relaxed);
    }
    run_swapped();
}

void Clock::set_unavailable(std::string reason) {
    std::lock_guard lock(mutex_);
    const auto state = state_.load(std::memory_order_acquire);
    if (state == AL_FRAME_UNAVAILABLE || state == AL_FRAME_STOPPED) return;
    reason_ = std::move(reason);
    state_.store(AL_FRAME_UNAVAILABLE, std::memory_order_release);
}

void Clock::close() {
    std::vector<Task> dropped;
    {
        std::lock_guard lock(mutex_);
        dropped.swap(queue_);
        queued_.store(0, std::memory_order_relaxed);
        auto state = state_.load(std::memory_order_acquire);
        if (state == AL_FRAME_WAITING || state == AL_FRAME_RUNNING) {
            state_.store(AL_FRAME_STOPPED, std::memory_order_release);
        }
    }
    // One line per plugin: how many of its tasks never ran.
    std::vector<std::pair<Client*, std::size_t>> counts;
    for (const auto& task : dropped) {
        task.stats->dropped.fetch_add(1, std::memory_order_relaxed);
        const auto found = std::find_if(counts.begin(), counts.end(), [&](const auto& c) { return c.first == task.client; });
        if (found == counts.end()) counts.emplace_back(task.client, 1);
        else ++found->second;
    }
    for (const auto& [client, count] : counts) {
        if (client->log) {
            client->log("WARN", "al.frame: " + std::to_string(count) +
                                    " game-thread task(s) never ran: the game's frames ended before them");
        }
    }
}

std::vector<std::string> Clock::summary() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> lines;
    const auto state = state_.load(std::memory_order_acquire);
    std::string head = std::string("frame_state=") + state_name(state) +
                       " frames=" + std::to_string(frames_.load(std::memory_order_relaxed)) +
                       " game_thread=" + std::to_string(game_thread_.load(std::memory_order_relaxed));
    if (state == AL_FRAME_UNAVAILABLE) head += " reason=" + reason_;
    lines.push_back(std::move(head));
    const auto per_us = ticks_per_us();
    for (const auto& subscription : subscriptions_) {
        const auto calls = subscription->calls.load(std::memory_order_relaxed);
        const auto total = subscription->ticks.load(std::memory_order_relaxed) / per_us;
        char mean[32];
        std::snprintf(mean, sizeof(mean), "%.2f", calls ? static_cast<double>(total) / static_cast<double>(calls) : 0.0);
        lines.push_back("frame_event plugin=" + subscription->client->name + " event=" + event_name(subscription->event) +
                        " calls=" + std::to_string(calls) + " total_us=" + std::to_string(total) + " mean_us=" + mean +
                        " max_us=" + std::to_string(subscription->max_ticks.load(std::memory_order_relaxed) / per_us) +
                        (subscription->removed ? " unsubscribed" : ""));
    }
    for (const auto& stats : stats_) {
        lines.push_back("frame_tasks plugin=" + stats->client->name +
                        " posted=" + std::to_string(stats->posted.load(std::memory_order_relaxed)) +
                        " run=" + std::to_string(stats->run.load(std::memory_order_relaxed)) +
                        " dropped=" + std::to_string(stats->dropped.load(std::memory_order_relaxed)));
    }
    return lines;
}

}  // namespace al::frame
