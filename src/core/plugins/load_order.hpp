#pragma once

#include "core/plugins/manifest.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace cmfm::plugins {

struct LoadOrder {
    // Indexes into the input, in the order to initialise them.
    std::vector<std::size_t> sequence;
    // Plugins that cannot be ordered, with the reason.
    struct Refusal {
        std::size_t index;
        std::string reason;
    };
    std::vector<Refusal> refused;
};

// Orders plugins by their load_after / load_before names first, then by
// `order` (lower first), then by name, so the result never depends on folder
// listing order. Names of plugins that are not present, and a plugin naming
// itself, are ignored. Plugins whose constraints form a cycle are refused,
// and the rest are ordered without them. A repeated name is refused.
LoadOrder resolve_load_order(const std::vector<Manifest>& plugins);

}  // namespace cmfm::plugins
