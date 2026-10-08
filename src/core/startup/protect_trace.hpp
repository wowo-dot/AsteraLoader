#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace cmfm::startup {
struct ProtectCall {
    std::int64_t begin_qpc = 0;
    std::int64_t end_qpc = 0;
    std::uintptr_t caller = 0;
    std::uintptr_t address = 0;
    std::size_t size = 0;
    std::uint32_t thread = 0;
    std::uint32_t protection = 0;
    std::uint32_t old_protection = 0;
    std::uint32_t last_error = 0;
    unsigned state = 0;
    bool success = false;
    bool old_available = false;
    bool from_stub = false;
    bool covers_text = false;
    bool executable_read = false;
};

// Single writer per ticket, one diagnostic reader. No allocation, waiting or
// log I/O on the intercepted call. Unpublished slots are never read.
class ProtectTrace {
public:
    static constexpr std::size_t limit = 256;
    static constexpr std::uint64_t ignored = UINT64_MAX;

    void start() noexcept { accepting_.store(true); }
    void stop() noexcept { accepting_.store(false); }
    std::uint64_t claim() noexcept {
        return accepting_.load() ? total_.fetch_add(1) : ignored;
    }
    void publish(std::uint64_t ticket, const ProtectCall& call) noexcept {
        if (ticket >= limit) return;
        auto& slot = slots_[static_cast<std::size_t>(ticket)];
        slot.call = call;
        slot.ready.store(true, std::memory_order_release);
    }
    bool read(std::size_t ticket, ProtectCall& call) const noexcept {
        if (ticket >= limit || !slots_[ticket].ready.load(std::memory_order_acquire)) return false;
        call = slots_[ticket].call;
        return true;
    }
    std::uint64_t total() const noexcept { return total_.load(); }

private:
    struct Slot { ProtectCall call; std::atomic<bool> ready{false}; };
    std::array<Slot, limit> slots_{};
    std::atomic<std::uint64_t> total_{0};
    std::atomic<bool> accepting_{false};
};
}  // namespace cmfm::startup
