#include "core/game_hooks/registry.hpp"

#include "common/memory.hpp"
#include "core/plugins/abi_sizes.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace al::game_hooks {
namespace {
constexpr std::size_t kMinimumPattern = 5;  // an inline hook overwrites at least five bytes

// Plugins read their next pointer while the chain is relinked: one store
// per pointer, never a torn one.
void publish(void** slot, void* value) noexcept {
    InterlockedExchangePointer(slot, value);
}

const std::uint8_t* shifted(const void* address, std::int64_t delta) {
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    if ((delta >= 0 && static_cast<std::uint64_t>(delta) > UINTPTR_MAX - at) ||
        (delta < 0 && static_cast<std::uint64_t>(-delta) > at)) return nullptr;
    return reinterpret_cast<const std::uint8_t*>(at + delta);
}
}  // namespace

struct Registry::Layer {
    std::uint32_t handle = 0;
    const Client* client = nullptr;
    std::int32_t order = 0;
    void* detour = nullptr;
    void** next = nullptr;
    std::string name;
    bool enabled = true;
};

struct Registry::Chain {
    void* target = nullptr;
    void* head = nullptr;      // the hook's entry jumps through this
    void* original = nullptr;  // runs the target's original code
    al_hook_state_t state = AL_HOOK_PENDING;
    std::string problem;
    std::vector<Layer> layers;  // call order
    // MinHook 1.3.4 changes five entry bytes, or five above + two at entry
    // for its hotpatch form. Retain that entire footprint for late joins.
    const std::uint8_t* saved_at = nullptr;
    std::size_t saved_size = 0;
    std::array<std::uint8_t, 10> before{}, patched{};
};

Registry::Registry(const void* image, Finder find)
    : image_(static_cast<const std::uint8_t*>(image)), find_(std::move(find)) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image_);
    if (!image_ || !readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image_ + dos->e_lfanew);
    if (!readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) return;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    if (!readable(section, sizeof(IMAGE_SECTION_HEADER) * nt->FileHeader.NumberOfSections)) return;
    for (WORD k = 0; k < nt->FileHeader.NumberOfSections; ++k, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const auto begin = reinterpret_cast<std::uintptr_t>(image_) + section->VirtualAddress;
        code_.emplace_back(begin, begin + section->Misc.VirtualSize);
    }
}

Registry::~Registry() = default;

bool Registry::in_code(const void* address, std::size_t size) const {
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    return std::any_of(code_.begin(), code_.end(), [&](const auto& range) {
        return begin >= range.first && begin + size <= range.second && begin + size >= begin;
    });
}

bool Registry::matches_at(const Pattern& pattern, const std::uint8_t* at) const {
    if (!in_code(at, pattern.bytes.size()) || !readable(at, pattern.bytes.size())) return false;
    std::vector<std::uint8_t> bytes(at, at + pattern.bytes.size());
    const auto begin = reinterpret_cast<std::uintptr_t>(at);
    for (const auto& chain : chains_) {
        if (chain->state != AL_HOOK_ACTIVE || !chain->saved_size) continue;
        const auto saved = reinterpret_cast<std::uintptr_t>(chain->saved_at);
        const auto first = (std::max)(begin, saved);
        const auto last = (std::min)(begin + bytes.size(), saved + chain->saved_size);
        if (first >= last) continue;
        // Only undo our known patch for validation. Later modifications are
        // not silently treated as the previously verified original bytes.
        if (!readable(chain->saved_at, chain->saved_size) ||
            std::memcmp(chain->saved_at, chain->patched.data(), chain->saved_size)) return false;
        std::memcpy(bytes.data() + first - begin, chain->before.data() + first - saved, last - first);
    }
    for (std::size_t k = 0; k < bytes.size(); ++k)
        if ((bytes[k] & pattern.mask[k]) != (pattern.bytes[k] & pattern.mask[k])) return false;
    return true;
}

std::uint32_t Registry::hook(Client& client, const al_hook_t& hook, std::string& problem) {
    problem.clear();
    if (hook.size < plugins::kHookSize10) {
        problem = "the hook struct is smaller than al.hooks 1.0's al_hook_t";
        return 0;
    }
    if (!hook.detour || !hook.next) {
        problem = "no detour or no next";
        return 0;
    }
    const auto parsed = hook.pattern ? parse_pattern(hook.pattern) : std::nullopt;
    if (!parsed || parsed->bytes.size() < kMinimumPattern) {
        problem = "the pattern is missing, unreadable or shorter than 5 bytes";
        return 0;
    }
    const auto& pattern = *parsed;
    // Resolution, signature checks and patching share the same lock, so a
    // concurrent registration never scans a half-written AL entry patch.
    std::lock_guard lock(mutex_);
    const std::uint8_t* target = nullptr;
    if (hook.address) {
        target = static_cast<const std::uint8_t*>(hook.address);
        const auto* start = shifted(target, -static_cast<std::int64_t>(hook.offset));
        if (!in_code(start, pattern.bytes.size()) || !matches_at(pattern, start)) {
            problem = "the bytes at the address do not match the pattern, or lie outside the game's code";
            return 0;
        }
    } else {
        auto found = find_ ? find_(pattern) : std::vector<const void*>{};
        found.erase(std::remove_if(found.begin(), found.end(), [&](const void* at) {
            return !matches_at(pattern, static_cast<const std::uint8_t*>(at));
        }), found.end());
        for (const auto& chain : chains_) {
            const auto at = shifted(chain->target, -static_cast<std::int64_t>(hook.offset));
            if (matches_at(pattern, at) && std::find(found.begin(), found.end(), at) == found.end()) found.push_back(at);
        }
        if (found.size() != 1) {
            problem = "the pattern matches " + std::to_string(found.size()) + " times in the game's code, not once";
            return 0;
        }
        target = shifted(found[0], hook.offset);
    }
    if (!in_code(target, kMinimumPattern)) {
        problem = "the target lies outside the game's code";
        return 0;
    }

    auto found = std::find_if(chains_.begin(), chains_.end(),
                              [&](const auto& chain) { return chain->target == target; });
    Chain* chain = found == chains_.end() ? nullptr : found->get();
    if (chain) {
        if (std::any_of(chain->layers.begin(), chain->layers.end(),
                        [&](const Layer& layer) { return layer.client == &client; })) {
            problem = "this plugin already hooks that function";
            return 0;
        }
        if (chain->state == AL_HOOK_FAILED) {
            problem = "that function could not be hooked: " + chain->problem;
            return 0;
        }
    } else {
        chains_.push_back(std::make_unique<Chain>());
        chain = chains_.back().get();
        chain->target = const_cast<std::uint8_t*>(target);
    }

    Layer layer;
    layer.handle = next_handle_++;
    layer.client = &client;
    layer.order = hook.order;
    layer.detour = hook.detour;
    layer.next = hook.next;
    layer.name = hook.name ? std::string(hook.name, strnlen(hook.name, 128)) : std::string();
    publish(layer.next, nullptr);
    const auto position = std::upper_bound(chain->layers.begin(), chain->layers.end(), layer,
                                           [](const Layer& a, const Layer& b) {
                                               return a.order != b.order ? a.order < b.order
                                                                         : a.client->load_index < b.client->load_index;
                                           });
    const auto handle = layer.handle;
    chain->layers.insert(position, std::move(layer));

    if (chain->state == AL_HOOK_ACTIVE) {
        link(*chain);
    } else if (installer_) {
        install(*chain);
        if (chain->state == AL_HOOK_FAILED) problem = "that function could not be hooked: " + chain->problem;
    }
    return problem.empty() ? handle : 0;
}

// Points every enabled detour at the next one, the last at the original,
// then the entry at the first. Written from the end, so each detour that
// becomes reachable already has its next; a detour taken out keeps its old
// next, so calls already inside it still finish.
void Registry::link(Chain& chain) {
    void* following = chain.original;
    for (auto layer = chain.layers.rbegin(); layer != chain.layers.rend(); ++layer) {
        if (!layer->enabled) continue;
        publish(layer->next, following);
        following = layer->detour;
    }
    publish(&chain.head, following);
}

void Registry::install(Chain& chain) {
    chain.saved_at = shifted(chain.target, -5);
    chain.saved_size = 10;
    if (!in_code(chain.saved_at, chain.saved_size)) { chain.saved_at = static_cast<const std::uint8_t*>(chain.target); chain.saved_size = 5; }
    if (readable(chain.saved_at, chain.saved_size)) std::memcpy(chain.before.data(), chain.saved_at, chain.saved_size);
    else chain.saved_size = 0;
    auto problem = installer_->create(chain.target, &chain.head, &chain.original);
    if (problem.empty() && !chain.original) problem = "the hook layer returned no original";
    if (problem.empty()) {
        link(chain);
        problem = installer_->enable(chain.target);
    }
    if (problem.empty()) {
        if (chain.saved_size) {
            std::memcpy(chain.patched.data(), chain.saved_at, chain.saved_size);
            if (chain.saved_size == 10) {
                if (std::memcmp(chain.before.data(), chain.patched.data(), 5)) {
                    chain.saved_size = 7; // MinHook's jump above + short entry jump.
                } else {
                    // Retain only our actual patch footprint. The bytes
                    // preceding this function might belong to another chain.
                    std::memmove(chain.before.data(), chain.before.data() + 5, 5);
                    std::memmove(chain.patched.data(), chain.patched.data() + 5, 5);
                    chain.saved_at += 5;
                    chain.saved_size = 5;
                }
            }
        }
        chain.state = AL_HOOK_ACTIVE;
        return;
    }
    chain.state = AL_HOOK_FAILED;
    chain.problem = problem;
    for (const auto& layer : chain.layers) {
        publish(layer.next, nullptr);
        if (layer.client->log) {
            layer.client->log("WARN", "al.hooks: " + (layer.name.empty() ? std::string("a hook") : layer.name) +
                                          " failed: " + problem);
        }
    }
}

void Registry::set_installer(Installer* installer) {
    std::lock_guard lock(mutex_);
    installer_ = installer;
    if (!installer_) return;
    for (auto& chain : chains_) {
        if (chain->state == AL_HOOK_PENDING) install(*chain);
    }
}

std::string Registry::set_enabled(const Client& client, std::uint32_t handle, bool enabled) {
    std::lock_guard lock(mutex_);
    for (auto& chain : chains_) {
        for (auto& layer : chain->layers) {
            if (layer.handle != handle) continue;
            if (layer.client != &client) return "the handle belongs to another plugin";
            if (chain->state == AL_HOOK_FAILED) return "the hook failed";
            layer.enabled = enabled;
            if (chain->state == AL_HOOK_ACTIVE) link(*chain);
            return {};
        }
    }
    return "unknown handle";
}

al_hook_state_t Registry::state(const Client& client, std::uint32_t handle) const {
    std::lock_guard lock(mutex_);
    for (const auto& chain : chains_) {
        for (const auto& layer : chain->layers) {
            if (layer.handle != handle || layer.client != &client) continue;
            if (chain->state != AL_HOOK_ACTIVE) return chain->state;
            return layer.enabled ? AL_HOOK_ACTIVE : AL_HOOK_DISABLED;
        }
    }
    return AL_HOOK_FAILED;
}

std::vector<std::string> Registry::summary() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> lines;
    for (const auto& chain : chains_) {
        char rva[32]{};
        std::snprintf(rva, sizeof(rva), "exe+0x%llx",
                      static_cast<unsigned long long>(static_cast<const std::uint8_t*>(chain->target) - image_));
        std::string name;
        for (const auto& layer : chain->layers) {
            if (!layer.name.empty()) {
                name = layer.name;
                break;
            }
        }
        std::string line = "game_hook target=" + std::string(rva) + " name=" + (name.empty() ? "-" : name) + " state=" +
                           (chain->state == AL_HOOK_ACTIVE    ? "active"
                            : chain->state == AL_HOOK_PENDING ? "pending"
                                                                : "failed") +
                           " layers=";
        for (std::size_t k = 0; k < chain->layers.size(); ++k) {
            const auto& layer = chain->layers[k];
            line += (k ? "," : "") + layer.client->name + (layer.enabled ? "" : "(off)");
        }
        if (chain->state == AL_HOOK_FAILED) line += " reason=" + chain->problem;
        lines.push_back(std::move(line));
    }
    return lines;
}

}  // namespace al::game_hooks
