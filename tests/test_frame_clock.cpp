// "al.frame" core side on its own: callback and task order, frame indices,
// tasks posted from many threads, unsubscribing, faults, the last-frame
// drain, SHUTDOWN, the unavailable state, and that a frame allocates nothing.
// The test's main thread stands in for the game's frame thread.
#include "core/frame/clock.hpp"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<std::uint64_t> g_allocations{0};
}  // namespace

// Counts every allocation in this process, to show a frame makes none.
void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {
int failures = 0;
void check(bool condition, const std::string& label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        ++failures;
    }
}

struct FakeClient {
    std::atomic<bool> stopped{false};
    std::vector<std::string> log;
    std::string disabled;
    al::frame::Client client;
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

FakeClient* fake(al_plugin_t* self) {
    return reinterpret_cast<FakeClient*>(self);
}

std::vector<std::string> g_trace;

void AL_CALL record(al_plugin_t* self, al_frame_event_t event, const al_frame_info_t* frame, void* user) {
    g_trace.push_back(fake(self)->client.name + (event == AL_FRAME_BEGIN ? " begin " : " end ") +
                      static_cast<const char*>(user) + " #" + std::to_string(frame->index));
}

void AL_CALL task(al_plugin_t* self, void* user) {
    g_trace.push_back(fake(self)->client.name + " task " + static_cast<const char*>(user));
}

void AL_CALL throws(al_plugin_t*, al_frame_event_t, const al_frame_info_t*, void*) {
    throw std::runtime_error("plugin bug");
}

void AL_CALL faults(al_plugin_t*, void*) {
    volatile int* nowhere = nullptr;
    *nowhere = 1;
}

al::frame::Clock* g_clock = nullptr;
FakeClient* g_poster = nullptr;
void AL_CALL posts_again(al_plugin_t* self, void*) {
    g_trace.push_back(fake(self)->client.name + " task posts again");
    g_clock->post(g_poster->client, task, const_cast<char*>("follow-up"));
}

void AL_CALL count(al_plugin_t*, void* user) {
    static_cast<std::atomic<int>*>(user)->fetch_add(1, std::memory_order_relaxed);
}

void AL_CALL nothing(al_plugin_t*, al_frame_event_t, const al_frame_info_t*, void*) {}

void frame(al::frame::Clock& clock) {
    clock.begin_frame();
    clock.end_frame();
}

bool contains(const std::vector<std::string>& lines, const std::string& part) {
    for (const auto& line : lines) {
        if (line.find(part) != std::string::npos) return true;
    }
    return false;
}

void order_and_tasks() {
    al::frame::Clock clock;
    g_clock = &clock;
    FakeClient alpha("alpha", 0), beta("beta", 1);
    std::string problem;
    check(!clock.subscribe(alpha.client, AL_FRAME_BEGIN, nullptr, nullptr, problem) && problem == "no callback",
          "no callback refused");
    check(!clock.subscribe(alpha.client, 7, record, nullptr, problem) && problem == "unknown frame event 7",
          "unknown event refused");

    // Beta subscribes first, but alpha loads first; within alpha, its order.
    const auto b1 = clock.subscribe(beta.client, AL_FRAME_BEGIN, record, const_cast<char*>("b1"), problem);
    const auto a1 = clock.subscribe(alpha.client, AL_FRAME_BEGIN, record, const_cast<char*>("a1"), problem);
    const auto a2 = clock.subscribe(alpha.client, AL_FRAME_BEGIN, record, const_cast<char*>("a2"), problem);
    clock.subscribe(beta.client, AL_FRAME_END, record, const_cast<char*>("b-end"), problem);
    check(b1 && a1 && a2 && a1 != a2, "handles");

    al_frame_status_t status{sizeof(status)};
    clock.status(status);
    check(status.state == AL_FRAME_WAITING && status.frames == 0 && status.game_thread == 0, "waiting before a frame");

    // Posted before the first frame, from two plugins: they run first, in posting order.
    check(clock.post(beta.client, task, const_cast<char*>("one")).empty(), "post while waiting");
    check(clock.post(alpha.client, task, const_cast<char*>("two")).empty(), "post from another plugin");
    g_trace.clear();
    frame(clock);
    check(g_trace == std::vector<std::string>{"beta task one", "alpha task two", "alpha begin a1 #0", "alpha begin a2 #0",
                                              "beta begin b1 #0", "beta end b-end #0"},
          "tasks, then BEGIN in load order, then END");
    clock.status(status);
    check(status.state == AL_FRAME_RUNNING && status.frames == 1 && status.game_thread == GetCurrentThreadId(),
          "running on the game thread");

    // A task posting another: it runs next frame. Unsubscribing takes effect next frame.
    g_poster = &alpha;
    clock.post(alpha.client, posts_again, nullptr);
    check(clock.unsubscribe(beta.client, a1) == "the handle belongs to another plugin", "only the owner unsubscribes");
    check(clock.unsubscribe(alpha.client, 999) == "unknown handle", "unknown handle");
    check(clock.unsubscribe(alpha.client, a1).empty(), "unsubscribed");
    g_trace.clear();
    frame(clock);
    check(g_trace == std::vector<std::string>{"alpha task posts again", "alpha begin a2 #1", "beta begin b1 #1",
                                              "beta end b-end #1"},
          "unsubscribed callback gone, follow-up not yet run");
    g_trace.clear();
    frame(clock);
    check(!g_trace.empty() && g_trace.front() == "alpha task follow-up", "follow-up runs the next frame");

    // Faults: a throwing callback and a faulting task disable their plugin; the frame goes on.
    FakeClient gamma("gamma", 2), delta("delta", 3);
    clock.subscribe(gamma.client, AL_FRAME_BEGIN, throws, nullptr, problem);
    clock.post(delta.client, faults, nullptr);
    g_trace.clear();
    frame(clock);
    check(gamma.disabled.find("its frame begin callback raised exception") == 0, "throwing callback disables its plugin");
    check(delta.disabled.find("its game-thread task raised exception 3221225477") == 0, "faulting task disables its plugin");
    check(contains(g_trace, "beta end b-end #3"), "the frame went on");
    g_trace.clear();
    frame(clock);
    check(!contains(g_trace, "gamma"), "a disabled plugin is skipped");

    // The last frame: queued tasks run on the game thread, then nothing more.
    clock.post(beta.client, task, const_cast<char*>("last"));
    g_trace.clear();
    clock.last_frame();
    check(g_trace == std::vector<std::string>{"beta task last"}, "last frame runs the queue");
    check(clock.post(beta.client, task, const_cast<char*>("late")) == "the game's frames have stopped",
          "post after the last frame refused");
    g_trace.clear();
    frame(clock);
    clock.status(status);
    check(g_trace.empty() && status.state == AL_FRAME_STOPPED, "no frames after the last one");

    const auto lines = clock.summary();
    check(contains(lines, "frame_state=stopped frames=5") && contains(lines, "frame_event plugin=alpha event=begin calls=1") &&
              contains(lines, "unsubscribed") && contains(lines, "frame_tasks plugin=beta posted=2 run=2 dropped=0") &&
              contains(lines, "frame_tasks plugin=alpha posted=3 run=3 dropped=0"),
          "summary");
    if (failures) {
        for (const auto& line : lines) std::cerr << line << '\n';
    }
}

void shutdown_and_unavailable() {
    al::frame::Clock clock;
    FakeClient alpha("alpha", 0);
    clock.post(alpha.client, task, const_cast<char*>("never"));
    clock.post(alpha.client, task, const_cast<char*>("never either"));
    clock.close();
    check(!alpha.log.empty() && alpha.log.back() ==
                                    "WARN al.frame: 2 game-thread task(s) never ran: the game's frames ended before them",
          "tasks still queued at SHUTDOWN are dropped and the plugin told");
    check(contains(clock.summary(), "frame_tasks plugin=alpha posted=2 run=0 dropped=2"), "dropped counted");

    al::frame::Clock other;
    std::string problem;
    other.set_unavailable("this game version is not supported");
    check(other.subscribe(alpha.client, AL_FRAME_BEGIN, record, nullptr, problem) != 0, "subscribe still succeeds");
    check(other.post(alpha.client, task, nullptr) == "frames are unavailable: this game version is not supported",
          "post refused with the reason");
    al_frame_status_t status{sizeof(status)};
    other.status(status);
    check(status.state == AL_FRAME_UNAVAILABLE && std::string(status.reason) == "this game version is not supported",
          "status says why");
    // A plugin built against a smaller struct gets only its part.
    al_frame_status_t small{};
    small.size = 8;
    small.frames = 77;
    other.status(small);
    check(small.size == 8 && small.state == AL_FRAME_UNAVAILABLE && small.frames == 77, "status up to the caller's size");
}

void many_posting_threads() {
    al::frame::Clock clock;
    FakeClient alpha("alpha", 0);
    std::atomic<int> ran{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> posters;
    constexpr int kThreads = 4, kPosts = 2000;
    for (int t = 0; t < kThreads; ++t) {
        posters.emplace_back([&] {
            while (!go.load()) std::this_thread::yield();
            for (int k = 0; k < kPosts; ++k) clock.post(alpha.client, count, &ran);
        });
    }
    go = true;
    int frames = 0;
    while (ran.load() < kThreads * kPosts && frames < 1000000) {
        frame(clock);
        ++frames;
    }
    for (auto& poster : posters) poster.join();
    frame(clock);
    check(ran.load() == kThreads * kPosts, "every task from four threads ran exactly once");
}

void no_allocation_per_frame() {
    al::frame::Clock clock;
    FakeClient alpha("alpha", 0);
    std::string problem;
    clock.subscribe(alpha.client, AL_FRAME_BEGIN, nothing, nullptr, problem);
    clock.subscribe(alpha.client, AL_FRAME_END, nothing, nullptr, problem);
    std::atomic<int> ran{0};
    for (int k = 0; k < 10; ++k) {  // warm up: the queue's storage settles
        clock.post(alpha.client, count, &ran);
        frame(clock);
    }
    const auto before = g_allocations.load();
    for (int k = 0; k < 1000; ++k) frame(clock);  // callbacks, nothing queued
    for (int k = 0; k < 1000; ++k) {               // one task per frame
        clock.post(alpha.client, count, &ran);
        frame(clock);
    }
    // Read the count before building the label, which allocates itself.
    const auto made = g_allocations.load() - before;
    check(made == 0, "frames and steady posting allocate nothing (" + std::to_string(made) + " allocations)");
}
}  // namespace

int main() {
    order_and_tasks();
    shutdown_and_unavailable();
    many_posting_threads();
    no_allocation_per_frame();
    if (failures) {
        std::cerr << failures << " frame clock check(s) failed\n";
        return 1;
    }
    std::cout << "al.frame orders callbacks and tasks, survives faults, and allocates nothing per frame\n";
    return 0;
}
