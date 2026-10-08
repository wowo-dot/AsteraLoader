#include "core/plugins/load_order.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace cmfm::plugins {
namespace {
std::string lower(std::string value) {
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

// Tarjan's strongly connected components: every cycle in the "initialise
// before" graph is one component with more than one plugin.
class Components {
public:
    Components(const std::vector<std::set<std::size_t>>& next, const std::vector<bool>& active)
        : next_(next), active_(active), index_(next.size(), -1), low_(next.size(), 0),
          on_stack_(next.size(), false) {
        for (std::size_t v = 0; v < next.size(); ++v) {
            if (active_[v] && index_[v] < 0) visit(v);
        }
    }
    const std::vector<std::vector<std::size_t>>& all() const { return components_; }

private:
    void visit(std::size_t v) {
        index_[v] = low_[v] = counter_++;
        stack_.push_back(v);
        on_stack_[v] = true;
        for (const auto w : next_[v]) {
            if (!active_[w]) continue;
            if (index_[w] < 0) {
                visit(w);
                low_[v] = std::min(low_[v], low_[w]);
            } else if (on_stack_[w]) {
                low_[v] = std::min(low_[v], index_[w]);
            }
        }
        if (low_[v] != index_[v]) return;
        std::vector<std::size_t> component;
        std::size_t w = 0;
        do {
            w = stack_.back();
            stack_.pop_back();
            on_stack_[w] = false;
            component.push_back(w);
        } while (w != v);
        components_.push_back(std::move(component));
    }

    const std::vector<std::set<std::size_t>>& next_;
    const std::vector<bool>& active_;
    std::vector<int> index_, low_;
    std::vector<bool> on_stack_;
    std::vector<std::size_t> stack_;
    int counter_ = 0;
    std::vector<std::vector<std::size_t>> components_;
};
}  // namespace

std::vector<LoadOrder::Refusal> check_dependencies(const std::vector<Manifest>& plugins,
                                                   const std::vector<Unavailable>& unavailable) {
    std::map<std::string, std::size_t> by_name;
    for (std::size_t i = 0; i < plugins.size(); ++i) by_name.emplace(plugins[i].name, i);
    std::map<std::string, std::string> missing;  // name -> why it will not run
    for (const auto& entry : unavailable) missing.emplace(lower(entry.name), entry.reason);

    std::vector<std::string> refusals(plugins.size());
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = 0; i < plugins.size(); ++i) {
            if (!refusals[i].empty()) continue;
            for (const auto& dependency : plugins[i].dependencies) {
                const auto name = lower(dependency.name);
                if (name == plugins[i].name) continue;
                const auto wanted = dependency.min_version.empty() ? std::string() : " " + dependency.min_version;
                std::string reason;
                const auto found = by_name.find(name);
                if (found == by_name.end()) {
                    reason = missing.count(name) ? "it needs " + name + ", which will not run"
                                                 : "it needs " + name + wanted + ", which is not installed";
                } else if (!refusals[found->second].empty()) {
                    reason = "it needs " + name + ", which will not run";
                } else if (!dependency.min_version.empty()) {
                    const auto& installed = plugins[found->second].version;
                    const auto have = parse_version(installed);
                    if (!have) {
                        reason = "it needs " + name + wanted + ", but " + name + "'s version \"" + installed +
                                 "\" cannot be compared";
                    } else if (compare_versions(*have, *parse_version(dependency.min_version)) < 0) {
                        reason = "it needs " + name + wanted + " or newer; " + installed + " is installed";
                    }
                }
                if (!reason.empty()) {
                    refusals[i] = std::move(reason);
                    changed = true;
                    break;
                }
            }
        }
    }
    std::vector<LoadOrder::Refusal> result;
    for (std::size_t i = 0; i < plugins.size(); ++i) {
        if (!refusals[i].empty()) result.push_back({i, std::move(refusals[i])});
    }
    return result;
}

LoadOrder resolve_load_order(const std::vector<Manifest>& plugins) {
    LoadOrder result;
    const auto count = plugins.size();
    std::vector<bool> active(count, true);
    std::map<std::string, std::size_t> by_name;
    for (std::size_t i = 0; i < count; ++i) {
        if (!by_name.emplace(plugins[i].name, i).second) {
            active[i] = false;
            result.refused.push_back({i, "another plugin has the same name"});
        }
    }

    // next[a] holds b when a must initialise before b.
    std::vector<std::set<std::size_t>> next(count);
    const auto find = [&](const std::string& name) -> std::size_t {
        const auto found = by_name.find(lower(name));
        return found == by_name.end() || !active[found->second] ? count : found->second;
    };
    for (std::size_t i = 0; i < count; ++i) {
        if (!active[i]) continue;
        for (const auto& name : plugins[i].load_after) {
            const auto j = find(name);
            if (j != count && j != i) next[j].insert(i);
        }
        for (const auto& dependency : plugins[i].dependencies) {
            const auto j = find(dependency.name);
            if (j != count && j != i) next[j].insert(i);
        }
        for (const auto& name : plugins[i].load_before) {
            const auto j = find(name);
            if (j != count && j != i) next[i].insert(j);
        }
    }

    const Components components(next, active);
    for (const auto& component : components.all()) {
        if (component.size() < 2) continue;
        for (const auto member : component) {
            std::vector<std::string> others;
            for (const auto other : component) {
                if (other != member) others.push_back(plugins[other].name);
            }
            std::sort(others.begin(), others.end());
            std::string reason = "its load order forms a cycle with ";
            for (std::size_t k = 0; k < others.size(); ++k) reason += (k ? ", " : "") + others[k];
            result.refused.push_back({member, std::move(reason)});
        }
        for (const auto member : component) active[member] = false;
    }

    // Kahn's algorithm; among plugins that are free to go, lower `order`
    // first, then name. Names are unique here, so the result is total.
    std::vector<std::size_t> incoming(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        if (!active[i]) continue;
        for (const auto j : next[i]) {
            if (active[j]) ++incoming[j];
        }
    }
    const auto before = [&plugins](std::size_t a, std::size_t b) {
        if (plugins[a].order != plugins[b].order) return plugins[a].order < plugins[b].order;
        return plugins[a].name < plugins[b].name;
    };
    std::set<std::size_t, decltype(before)> ready(before);
    for (std::size_t i = 0; i < count; ++i) {
        if (active[i] && incoming[i] == 0) ready.insert(i);
    }
    while (!ready.empty()) {
        const auto i = *ready.begin();
        ready.erase(ready.begin());
        result.sequence.push_back(i);
        for (const auto j : next[i]) {
            if (active[j] && --incoming[j] == 0) ready.insert(j);
        }
    }

    std::sort(result.refused.begin(), result.refused.end(),
              [](const LoadOrder::Refusal& a, const LoadOrder::Refusal& b) { return a.index < b.index; });
    return result;
}

}  // namespace cmfm::plugins
