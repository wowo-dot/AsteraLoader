#include "core/image/claims.hpp"

#include <algorithm>
#include <cstring>

namespace al::image {

std::optional<Claim> Claims::add(Claim& claim) {
    std::lock_guard lock(mutex_);
    for (const auto& other : claims_) {
        if (claim.begin < other.end && claim.end > other.begin) return other;
    }
    claim.id = next_++;
    claims_.push_back(claim);
    return std::nullopt;
}

void Claims::update(std::uint64_t id, std::uintptr_t begin, std::uintptr_t end, std::vector<std::uint8_t> original) {
    std::lock_guard lock(mutex_);
    for (auto& claim : claims_) {
        if (claim.id != id) continue;
        claim.begin = begin;
        claim.end = end;
        claim.original = original.size() == end - begin ? std::move(original) : std::vector<std::uint8_t>{};
        return;
    }
}

void Claims::set_holder(std::uint64_t id, std::string holder) {
    std::lock_guard lock(mutex_);
    for (auto& claim : claims_) {
        if (claim.id == id) claim.holder = std::move(holder);
    }
}

void Claims::remove(std::uint64_t id) {
    std::lock_guard lock(mutex_);
    claims_.erase(std::remove_if(claims_.begin(), claims_.end(), [&](const Claim& claim) { return claim.id == id; }),
                  claims_.end());
}

std::optional<Claim> Claims::overlapping(std::uintptr_t begin, std::uintptr_t end, std::uint64_t except) const {
    std::lock_guard lock(mutex_);
    for (const auto& claim : claims_) {
        if (claim.id != except && begin < claim.end && end > claim.begin) return claim;
    }
    return std::nullopt;
}

void Claims::overlay(std::uintptr_t at, std::span<std::uint8_t> bytes, bool hooks, std::uint64_t except) const {
    std::lock_guard lock(mutex_);
    const auto end = at + bytes.size();
    for (const auto& claim : claims_) {
        if (claim.id == except || (claim.hook && !hooks) || claim.original.empty()) continue;
        const auto first = (std::max)(at, claim.begin);
        const auto last = (std::min)(end, claim.end);
        if (first >= last) continue;
        std::memcpy(bytes.data() + (first - at), claim.original.data() + (first - claim.begin), last - first);
    }
}

std::vector<std::uintptr_t> Claims::windows(std::size_t size) const {
    std::lock_guard lock(mutex_);
    std::vector<std::uintptr_t> starts;
    if (!size) return starts;
    for (const auto& claim : claims_) {
        const auto first = claim.begin >= size - 1 ? claim.begin - (size - 1) : 0;
        for (auto at = first; at < claim.end; ++at) starts.push_back(at);
    }
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    return starts;
}

}  // namespace al::image
