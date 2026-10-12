#pragma once

// Who holds which bytes of the game exe: the one map shared by "al.hooks"
// (a chain's entry bytes) and "al.patch" (a change's bytes). Claims never
// overlap. Each keeps the game's original bytes where they differ now, so
// pattern checks can see the game as it was.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace al::image {

struct Claim {
    std::uint64_t id = 0;
    std::uintptr_t begin = 0, end = 0;  // [begin, end)
    bool hook = false;                  // an al.hooks chain; otherwise an al.patch change
    std::string holder;                 // for messages, e.g. "the plugin cmfm (Start menu: System tab items)"
    // The game's bytes over [begin, end) from before the change; empty while
    // they are still in place (a pending hook, say).
    std::vector<std::uint8_t> original;
};

class Claims {
public:
    // Adds `claim` unless it overlaps one already there; returns the one in
    // the way, or nullopt when added (and then claim.id is its id).
    std::optional<Claim> add(Claim& claim);
    // Widens or narrows a claim, and sets its original bytes.
    void update(std::uint64_t id, std::uintptr_t begin, std::uintptr_t end, std::vector<std::uint8_t> original);
    void set_holder(std::uint64_t id, std::string holder);
    void remove(std::uint64_t id);
    // A copy of the first claim overlapping [begin, end), other than `except`.
    std::optional<Claim> overlapping(std::uintptr_t begin, std::uintptr_t end, std::uint64_t except = 0) const;
    // Copies each claim's original bytes (where it has them) over `bytes`,
    // which were read at `at`. `hooks` false leaves hook claims out.
    void overlay(std::uintptr_t at, std::span<std::uint8_t> bytes, bool hooks = true, std::uint64_t except = 0) const;
    // Where a pattern of `size` bytes would start to cover a claimed byte:
    // candidates a search of the current bytes cannot find.
    std::vector<std::uintptr_t> windows(std::size_t size) const;
    // One writer at a time on the game's image, for every service: hold it
    // from the check of the bytes to the end of the write.
    std::recursive_mutex& writes() noexcept { return writes_; }

private:
    mutable std::mutex mutex_;
    std::recursive_mutex writes_;
    std::vector<Claim> claims_;
    std::uint64_t next_ = 1;
};

}  // namespace al::image
