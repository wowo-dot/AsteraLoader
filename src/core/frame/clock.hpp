#pragma once

// "al.frame" (API 1.5), core side: the game thread's frame callbacks and its
// task queue. The frame layer calls begin_frame and end_frame around each of
// the game's frames on its frame thread; nothing here hooks anything.
//
// The game thread reads the subscriber lists without a lock: each change
// publishes a new list and keeps the old one until the process exits. The
// task queue is two vectors swapped under a lock, so their storage is reused
// and a frame allocates nothing.

#include "al/al_plugin.h"
#include "core/graphics/hub.hpp"  // Client

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace al::frame {

using Client = graphics::Client;

class Clock {
public:
    Clock();
    ~Clock();
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;

    // "al.frame". A handle, or 0 with the reason in `problem`.
    std::uint32_t subscribe(Client& client, al_frame_event_t event, al_frame_fn callback, void* user,
                            std::string& problem);
    // "" or why not.
    std::string unsubscribe(const Client& client, std::uint32_t handle);
    std::string post(Client& client, al_task_fn task, void* user);
    // Fills `out` up to out.size.
    void status(al_frame_status_t& out) const;
    al_frame_state_t state() const noexcept { return state_.load(std::memory_order_acquire); }

    // The frame layer, on the game's frame thread.
    void begin_frame() noexcept;
    void end_frame() noexcept;
    // The game left its frame loop after this frame: runs the queued tasks,
    // then nothing more is called.
    void last_frame() noexcept;

    // The frame layer could not be installed; nothing will be called.
    void set_unavailable(std::string reason);
    // SHUTDOWN: drops the tasks still queued and tells their plugins.
    void close();
    // One line per subscription and per plugin that posted, for the core log.
    std::vector<std::string> summary() const;

private:
    struct Stats;
    struct Subscription;
    struct List;
    struct Task {
        Client* client;
        al_task_fn task;
        void* user;
        Stats* stats;
    };

    Stats& stats_for(Client& client);  // under mutex_
    void publish(al_frame_event_t event);  // under mutex_
    void run(al_frame_event_t event, std::int64_t start) noexcept;
    void run_tasks() noexcept;
    void run_swapped() noexcept;  // runs and clears running_

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Subscription>> subscriptions_;  // every one made, until exit
    std::vector<std::unique_ptr<List>> lists_kept_;              // every list published, until exit
    std::array<std::atomic<const List*>, 2> lists_{};            // per event
    std::vector<std::unique_ptr<Stats>> stats_;                  // per plugin that posted
    std::vector<Task> queue_;    // under mutex_
    std::vector<Task> running_;  // the game thread's, swapped with queue_
    std::atomic<std::uint32_t> queued_{0};
    std::atomic<al_frame_state_t> state_{AL_FRAME_WAITING};
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint32_t> game_thread_{0};
    std::string reason_;  // written once, before UNAVAILABLE is published
    std::uint32_t next_handle_ = 1;
};

}  // namespace al::frame
