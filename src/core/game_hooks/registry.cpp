#include "core/game_hooks/registry.hpp"

#include "common/memory.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace cmfm::game_hooks {
namespace {
constexpr std::size_t kMinimumPattern = 5;  // an inline hook overwrites at least five bytes

// Plugins read their next pointer while the chain is relinked: one store
// per pointer, never a torn one.
void publish(void** slot, void* value) noexcept {
    InterlockedExchangePointer(slot, value);
}

bool matches_at(const Pattern& pattern, const std::uint8_t* at) {
    if (!readable(at, pattern.bytes.size())) return false;
    for (std::size_t k = 0; k < pattern.bytes.size(); ++k) {
        if ((at[k] & pattern.mask[k]) != (pattern.bytes[k] & pattern.mask[k])) return false;
    }
    return true;
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
    cmfm_hook_state_t state = CMFM_HOOK_PENDING;
    std::string problem;
    std::vector<Layer> layers;  // call order
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

std::uint32_t Registry::hook(Client& client, const cmfm_hook_t& hook, std::string& problem) {
    problem.clear();
    if (hook.size < sizeof(cmfm_hook_t)) {
        problem = "the hook struct is smaller than cmfm_hook_t";
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
    const std::uint8_t* target = nullptr;
    if (hook.address) {
        target = static_cast<const std::uint8_t*>(hook.address);
        const auto* start = target - hook.offset;
        if (!in_code(start, pattern.bytes.size()) || !matches_at(pattern, start)) {
            problem = "the bytes at the address do not match the pattern, or lie outside the game's code";
            return 0;
        }
    } else {
        const auto found = find_ ? find_(pattern) : std::vector<const void*>{};
        if (found.size() != 1) {
            problem = "the pattern matches " + std::to_string(found.size()) + " times in the game's code, not once";
            return 0;
        }
        target = static_cast<const std::uint8_t*>(found[0]) + hook.offset;
    }
    if (!in_code(target, kMinimumPattern)) {
        problem = "the target lies outside the game's code";
        return 0;
    }

    std::lock_guard lock(mutex_);
    auto found = std::find_if(chains_.begin(), chains_.end(),
                              [&](const auto& chain) { return chain->target == target; });
    Chain* chain = found == chains_.end() ? nullptr : found->get();
    if (chain) {
        if (std::any_of(chain->layers.begin(), chain->layers.end(),
                        [&](const Layer& layer) { return layer.client == &client; })) {
            problem = "this plugin already hooks that function";
            return 0;
        }
        if (chain->state == CMFM_HOOK_FAILED) {
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

    if (chain->state == CMFM_HOOK_ACTIVE) {
        link(*chain);
    } else if (installer_) {
        install(*chain);
        if (chain->state == CMFM_HOOK_FAILED) problem = "that function could not be hooked: " + chain->problem;
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
    auto problem = installer_->create(chain.target, &chain.head, &chain.original);
    if (problem.empty() && !chain.original) problem = "the hook layer returned no original";
    if (problem.empty()) {
        link(chain);
        problem = installer_->enable(chain.target);
    }
    if (problem.empty()) {
        chain.state = CMFM_HOOK_ACTIVE;
        return;
    }
    chain.state = CMFM_HOOK_FAILED;
    chain.problem = problem;
    for (const auto& layer : chain.layers) {
        publish(layer.next, nullptr);
        if (layer.client->log) {
            layer.client->log("WARN", "cmfm.hooks: " + (layer.name.empty() ? std::string("a hook") : layer.name) +
                                          " failed: " + problem);
        }
    }
}

void Registry::set_installer(Installer* installer) {
    std::lock_guard lock(mutex_);
    installer_ = installer;
    if (!installer_) return;
    for (auto& chain : chains_) {
        if (chain->state == CMFM_HOOK_PENDING) install(*chain);
    }
}

std::string Registry::set_enabled(const Client& client, std::uint32_t handle, bool enabled) {
    std::lock_guard lock(mutex_);
    for (auto& chain : chains_) {
        for (auto& layer : chain->layers) {
            if (layer.handle != handle) continue;
            if (layer.client != &client) return "the handle belongs to another plugin";
            if (chain->state == CMFM_HOOK_FAILED) return "the hook failed";
            layer.enabled = enabled;
            if (chain->state == CMFM_HOOK_ACTIVE) link(*chain);
            return {};
        }
    }
    return "unknown handle";
}

cmfm_hook_state_t Registry::state(const Client& client, std::uint32_t handle) const {
    std::lock_guard lock(mutex_);
    for (const auto& chain : chains_) {
        for (const auto& layer : chain->layers) {
            if (layer.handle != handle || layer.client != &client) continue;
            if (chain->state != CMFM_HOOK_ACTIVE) return chain->state;
            return layer.enabled ? CMFM_HOOK_ACTIVE : CMFM_HOOK_DISABLED;
        }
    }
    return CMFM_HOOK_FAILED;
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
                           (chain->state == CMFM_HOOK_ACTIVE    ? "active"
                            : chain->state == CMFM_HOOK_PENDING ? "pending"
                                                                : "failed") +
                           " layers=";
        for (std::size_t k = 0; k < chain->layers.size(); ++k) {
            const auto& layer = chain->layers[k];
            line += (k ? "," : "") + layer.client->name + (layer.enabled ? "" : "(off)");
        }
        if (chain->state == CMFM_HOOK_FAILED) line += " reason=" + chain->problem;
        lines.push_back(std::move(line));
    }
    return lines;
}

}  // namespace cmfm::game_hooks
