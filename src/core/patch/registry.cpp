#include "core/patch/registry.hpp"

#include "common/memory.hpp"
#include "core/crash/handler.hpp"
#include "core/plugins/abi_sizes.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace al::patch {
namespace {

const char* kind_name(std::uint32_t kind) {
    return kind == AL_PATCH_CODE ? "code" : kind == AL_PATCH_DATA ? "data" : "call";
}

const std::uint8_t* shifted(const void* address, std::int64_t delta) {
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    if ((delta >= 0 && static_cast<std::uint64_t>(delta) > UINTPTR_MAX - at) ||
        (delta < 0 && static_cast<std::uint64_t>(-delta) > at)) {
        return nullptr;
    }
    return reinterpret_cast<const std::uint8_t*>(at + delta);
}

std::string where(const image::Layout& layout, const void* address) {
    char text[40]{};
    std::snprintf(text, sizeof(text), "exe+0x%llx", static_cast<unsigned long long>(layout.rva(address)));
    return text;
}
}  // namespace

std::string hex_bytes(const std::uint8_t* bytes, std::size_t size, std::size_t most) {
    std::string text;
    for (std::size_t k = 0; k < size && k < most; ++k) {
        char hex[4]{};
        std::snprintf(hex, sizeof(hex), "%02X", bytes[k]);
        text += (k ? " " : "") + std::string(hex);
    }
    if (size > most) text += " ...";
    return text;
}

struct Registry::Change {
    std::uint32_t handle = 0;
    const Client* client = nullptr;
    std::uint32_t kind = AL_PATCH_CODE;
    std::string name;
    std::uint8_t* target = nullptr;
    std::size_t length = 0;
    std::array<std::uint8_t, AL_PATCH_MAX_LENGTH> expected{}, replacement{};
    void* function = nullptr;  // AL_PATCH_CALL
    void* stub = nullptr;      // AL_PATCH_CALL, once placed
    al_patch_state_t state = AL_PATCH_PENDING;
    bool wanted = true;        // a pending change's last set_enabled
    bool put_back = false;     // put back by finish()
    std::string problem;
    std::uint64_t claim = 0;
    bool listed = false;  // in the crash handler's table
};

Registry::Registry(const void* image, Finder find, image::Claims& claims)
    : layout_(image), find_(std::move(find)), claims_(claims) {}

Registry::~Registry() = default;

bool Registry::matches_at(const Pattern& pattern, const std::uint8_t* at, image::Area area) const {
    if (!at || layout_.area(at, pattern.bytes.size()) != area || !readable(at, pattern.bytes.size())) return false;
    std::vector<std::uint8_t> bytes(at, at + pattern.bytes.size());
    claims_.overlay(reinterpret_cast<std::uintptr_t>(at), bytes);
    for (std::size_t k = 0; k < bytes.size(); ++k) {
        if ((bytes[k] & pattern.mask[k]) != (pattern.bytes[k] & pattern.mask[k])) return false;
    }
    return true;
}

std::uint32_t Registry::apply(Client& client, const al_patch_t& patch, std::string& problem) {
    problem.clear();
    if (patch.size < plugins::kPatchSize10) {
        problem = "the patch struct is smaller than al.patch 1.0's al_patch_t";
        return 0;
    }
    if (patch.kind > AL_PATCH_CALL) {
        problem = "unknown kind " + std::to_string(patch.kind);
        return 0;
    }
    const bool call = patch.kind == AL_PATCH_CALL;
    if (call ? patch.length != 5 : (patch.length == 0 || patch.length > AL_PATCH_MAX_LENGTH)) {
        problem = call ? "a call change is 5 bytes long, not " + std::to_string(patch.length)
                       : "the length must be 1 to " + std::to_string(AL_PATCH_MAX_LENGTH) + ", not " +
                             std::to_string(patch.length);
        return 0;
    }
    if (!patch.expected || (!call && !patch.replacement) || (call && !patch.function)) {
        problem = call ? "no expected bytes or no function" : "no expected or no replacement bytes";
        return 0;
    }
    const auto* expected = static_cast<const std::uint8_t*>(patch.expected);
    if (call && expected[0] != 0xE8 && expected[0] != 0xE9) {
        problem = "a call change needs E8 (call) or E9 (jmp) and a rel32; the expected bytes start with " +
                  hex_bytes(expected, 1);
        return 0;
    }
    if (!patch.address && !patch.pattern) {
        problem = "no address and no pattern";
        return 0;
    }
    std::optional<Pattern> parsed;
    if (patch.pattern) {
        parsed = parse_pattern(patch.pattern);
        if (!parsed) {
            problem = "the pattern is unreadable";
            return 0;
        }
    }
    const auto area = patch.kind == AL_PATCH_DATA ? image::Area::data : image::Area::code;
    const auto length = static_cast<std::size_t>(patch.length);

    std::lock_guard lock(mutex_);
    std::lock_guard writes(claims_.writes());
    const std::uint8_t* target = nullptr;
    if (patch.address) {
        target = static_cast<const std::uint8_t*>(patch.address);
        if (parsed && !matches_at(*parsed, shifted(target, -static_cast<std::int64_t>(patch.offset)), area)) {
            problem = "the pattern does not match at the address";
            return 0;
        }
    } else {
        auto found = find_ ? find_(*parsed, area == image::Area::code) : std::vector<const void*>{};
        found.erase(std::remove_if(found.begin(), found.end(), [&](const void* at) {
                        return !matches_at(*parsed, static_cast<const std::uint8_t*>(at), area);
                    }),
                    found.end());
        // Matches over bytes another change or a hook holds: the search of
        // the current bytes cannot see them.
        for (const auto start : claims_.windows(parsed->bytes.size())) {
            const auto* at = reinterpret_cast<const std::uint8_t*>(start);
            if (matches_at(*parsed, at, area) && std::find(found.begin(), found.end(), at) == found.end()) {
                found.push_back(at);
            }
        }
        if (found.size() != 1) {
            problem = "the pattern matches " + std::to_string(found.size()) + " times in " +
                      image::area_name(area) + ", not once";
            return 0;
        }
        target = shifted(found[0], patch.offset);
    }
    const auto actual = layout_.area(target, length);
    if (actual != area) {
        problem = std::string("the bytes lie in ") + image::area_name(actual) + "; " +
                  (area == image::Area::data ? "a data change goes in the game's data"
                                             : "a code or call change goes in the game's code");
        return 0;
    }
    if (!readable(target, length)) {
        problem = "the bytes are not readable";
        return 0;
    }
    if (std::memcmp(target, expected, length) != 0) {
        problem = "the bytes at " + where(layout_, target) + " are " + hex_bytes(target, length) +
                  ", not the expected " + hex_bytes(expected, length);
        return 0;
    }

    auto change = std::make_unique<Change>();
    change->handle = next_handle_++;
    change->client = &client;
    change->kind = patch.kind;
    change->name = patch.name ? std::string(patch.name, strnlen(patch.name, 128)) : std::string();
    change->target = const_cast<std::uint8_t*>(target);
    change->length = length;
    std::memcpy(change->expected.data(), expected, length);
    if (!call) std::memcpy(change->replacement.data(), patch.replacement, length);
    change->function = call ? patch.function : nullptr;

    image::Claim claim;
    claim.begin = reinterpret_cast<std::uintptr_t>(target);
    claim.end = claim.begin + length;
    claim.holder = "the plugin " + client.name + "'s change" +
                   (change->name.empty() ? std::string() : " \"" + change->name + "\"");
    claim.original.assign(expected, expected + length);
    if (const auto other = claims_.add(claim)) {
        problem = "the bytes overlap " + other->holder;
        return 0;
    }
    change->claim = claim.id;

    // The old target is the plugin's before the new call can run.
    if (call && patch.original) {
        std::int32_t rel = 0;
        std::memcpy(&rel, expected + 1, sizeof(rel));
        *patch.original = const_cast<std::uint8_t*>(target + 5 + rel);
    }
    auto& added = *changes_.emplace_back(std::move(change));
    if (writer_) {
        const auto outcome = write(added, true, problem);
        if (outcome != Outcome::done) {
            // Refused like the checks above: no handle, nothing kept.
            fail(added, problem);
            changes_.pop_back();
            return 0;
        }
    }
    return added.handle;
}

Registry::Outcome Registry::write(Change& change, bool enable, std::string& problem) {
    std::lock_guard writes(claims_.writes());
    if (enable && change.kind == AL_PATCH_CALL) {
        if (!change.stub) {
            const auto stub_problem = writer_->stub(change.target, change.function, &change.stub);
            if (!stub_problem.empty() || !change.stub) {
                problem = "no stub within reach: " + (stub_problem.empty() ? std::string("none returned") : stub_problem);
                return Outcome::failed;
            }
        }
        const auto distance = reinterpret_cast<std::intptr_t>(change.stub) -
                              reinterpret_cast<std::intptr_t>(change.target + 5);
        if (distance < INT32_MIN || distance > INT32_MAX) {
            problem = "the stub is out of a rel32's reach";
            return Outcome::failed;
        }
        const auto rel = static_cast<std::int32_t>(distance);
        change.replacement[0] = change.expected[0];
        std::memcpy(change.replacement.data() + 1, &rel, sizeof(rel));
    }
    const auto* from = enable ? change.expected.data() : change.replacement.data();
    const auto* to = enable ? change.replacement.data() : change.expected.data();
    std::array<std::uint8_t, AL_PATCH_MAX_LENGTH> found{};
    std::string why;
    const auto result =
        writer_->write(change.target, from, to, change.length, change.kind != AL_PATCH_DATA, found.data(), why);
    if (result == Writer::Result::differs) {
        problem = "the bytes at " + where(layout_, change.target) + " are " + hex_bytes(found.data(), change.length) +
                  ", not the " + (enable ? "expected " : "replacement ") + hex_bytes(from, change.length) +
                  "; something outside Astera Loader changed them";
        return Outcome::differs;
    }
    if (result == Writer::Result::failed) {
        problem = why.empty() ? std::string("the write failed") : why;
        return Outcome::failed;
    }
    change.state = enable ? AL_PATCH_ACTIVE : AL_PATCH_DISABLED;
    if (enable && !change.listed) {
        crash::note_change(change.target, change.length, change.client->name.c_str(),
                           change.name.empty() ? kind_name(change.kind) : change.name.c_str(),
                           change.kind != AL_PATCH_DATA);
        change.listed = true;
    } else if (!enable && change.listed) {
        crash::end_change(change.target);
        change.listed = false;
    }
    return Outcome::done;
}

void Registry::fail(Change& change, const std::string& problem) {
    change.state = AL_PATCH_FAILED;
    change.problem = problem;
    if (change.claim) {
        claims_.remove(change.claim);
        change.claim = 0;
    }
    if (change.listed) {
        crash::end_change(change.target);
        change.listed = false;
    }
}

std::string Registry::set_enabled(const Client& client, std::uint32_t handle, bool enabled) {
    std::lock_guard lock(mutex_);
    for (auto& change : changes_) {
        if (change->handle != handle) continue;
        if (change->client != &client) return "the handle belongs to another plugin";
        if (change->state == AL_PATCH_FAILED) return "the change failed: " + change->problem;
        if (change->state == AL_PATCH_PENDING) {
            change->wanted = enabled;
            return {};
        }
        if ((change->state == AL_PATCH_ACTIVE) == enabled) return {};
        std::string problem;
        const auto outcome = write(*change, enabled, problem);
        if (outcome == Outcome::differs) fail(*change, problem);
        return outcome == Outcome::done ? std::string() : problem;
    }
    return "unknown handle";
}

al_patch_state_t Registry::state(const Client& client, std::uint32_t handle) const {
    std::lock_guard lock(mutex_);
    for (const auto& change : changes_) {
        if (change->handle == handle && change->client == &client) return change->state;
    }
    return AL_PATCH_FAILED;
}

void Registry::set_writer(Writer* writer) {
    std::lock_guard lock(mutex_);
    writer_ = writer;
    if (!writer_) return;
    for (auto& change : changes_) {
        if (change->state != AL_PATCH_PENDING) continue;
        if (!change->wanted) {
            change->state = AL_PATCH_DISABLED;
            continue;
        }
        std::string problem;
        if (write(*change, true, problem) != Outcome::done) {
            fail(*change, problem);
            if (change->client->log) {
                change->client->log("WARN", "al.patch: " + (change->name.empty() ? std::string("a change") : change->name) +
                                                " failed: " + problem);
            }
        }
    }
}

void Registry::finish() {
    std::lock_guard lock(mutex_);
    if (!writer_) return;
    for (auto& change : changes_) {
        if (change->kind != AL_PATCH_DATA || change->state != AL_PATCH_ACTIVE) continue;
        std::string problem;
        const auto outcome = write(*change, false, problem);
        if (outcome == Outcome::done) {
            change->put_back = true;
        } else if (outcome == Outcome::differs) {
            fail(*change, problem);
        }
    }
}

std::vector<std::string> Registry::summary() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> lines;
    for (const auto& change : changes_) {
        const char* state = change->state == AL_PATCH_ACTIVE    ? "active"
                            : change->state == AL_PATCH_PENDING ? "pending"
                            : change->state == AL_PATCH_DISABLED ? (change->put_back ? "put_back_at_shutdown" : "off")
                                                                 : "failed";
        std::string line = "patch target=" + where(layout_, change->target) +
                           " name=" + (change->name.empty() ? std::string("-") : change->name) +
                           " kind=" + kind_name(change->kind) + " length=" + std::to_string(change->length) +
                           " owner=" + change->client->name + " state=" + state;
        if (change->state == AL_PATCH_FAILED) line += " reason=" + change->problem;
        lines.push_back(std::move(line));
    }
    return lines;
}

}  // namespace al::patch
