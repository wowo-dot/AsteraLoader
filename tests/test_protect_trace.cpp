#include "core/startup/protect_trace.hpp"

#include <array>
#include <cstdio>
#include <thread>

int main() {
    using namespace al::startup;
    ProtectTrace trace;
    if (trace.claim() != ProtectTrace::ignored) return 1;
    trace.start();
    const auto pending = trace.claim();
    std::array<std::thread, 8> writers;
    for (auto& writer : writers) writer = std::thread([&] {
        for (unsigned i = 0; i < 100; ++i) {
            const auto ticket = trace.claim();
            ProtectCall call;
            call.caller = static_cast<std::uintptr_t>(ticket);
            call.size = 7;
            trace.publish(ticket, call);
        }
    });
    for (auto& writer : writers) writer.join();
    trace.stop();
    ProtectCall call;
    if (trace.total() != 801 || trace.claim() != ProtectTrace::ignored || trace.read(pending, call)) return 2;
    for (std::size_t i = 1; i < ProtectTrace::limit; ++i) {
        if (!trace.read(i, call) || call.caller != i || call.size != 7) return 3;
    }
    if (trace.read(ProtectTrace::limit, call)) return 4;
    call.caller = 123;
    trace.publish(pending, call);
    if (!trace.read(pending, call) || call.caller != 123) return 5;
    std::puts("Bounded concurrent trace and incomplete-slot handling passed");
    return 0;
}
