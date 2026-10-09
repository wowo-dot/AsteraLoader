// "cmfm.hooks" core side without patching: target checks, chain order, late
// joins, switching detours off and on, pending hooks before the hook layer
// exists, and failures. Functions in this exe stand in for game code; a
// stand-in installer records each hook's entry pointer, and the test calls
// through it as the game's call would.
#include "core/game_hooks/registry.hpp"
#include "core/plugins/services.hpp"

#include <windows.h>

#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace hooks = cmfm::game_hooks;

namespace {
int failures = 0;
void check(bool condition, const std::string& label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        ++failures;
    }
}

std::vector<std::string> g_trace;

// Stand-in game functions; their constants make their bytes unique.
__declspec(noinline) int game_damage(int a, int b) {
    return a * 7919 + b * 104729 + 0x5A17C3;
}
__declspec(noinline) int game_quest(int a) {
    return (a ^ 0x13579BDF) * 31337;
}
__declspec(noinline) int game_other(int a) {
    return (a + 0x2468ACE) * 65521;
}
int g_data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

using DamageFn = int (*)(int, int);
using QuestFn = int (*)(int);

// What the hook layer's trampolines would do: run the original code.
int original_damage(int a, int b) {
    g_trace.push_back("original");
    return game_damage(a, b);
}
int original_quest(int a) {
    g_trace.push_back("original quest");
    return game_quest(a);
}

void* g_next_a = nullptr;
void* g_next_b = nullptr;
void* g_next_c = nullptr;
void* g_next_quest = nullptr;
void* g_next_unused = nullptr;
int detour_a(int a, int b) {
    g_trace.push_back("a");
    return reinterpret_cast<DamageFn>(g_next_a)(a, b);
}
int detour_b(int a, int b) {
    g_trace.push_back("b");
    return reinterpret_cast<DamageFn>(g_next_b)(a, b) + 1;  // b changes the result
}
int detour_c(int a, int b) {
    g_trace.push_back("c");
    return reinterpret_cast<DamageFn>(g_next_c)(a, b);
}
int detour_quest(int a) {
    g_trace.push_back("quest");
    return reinterpret_cast<QuestFn>(g_next_quest)(a);
}

class StandInInstaller : public hooks::Installer {
public:
    std::map<void*, void* const*> heads;
    std::vector<std::string> calls;
    std::string refuse;

    std::string create(void* target, void* const* head, void** original) override {
        calls.push_back("create");
        if (!refuse.empty()) return refuse;
        heads[target] = head;
        *original = target == reinterpret_cast<void*>(&game_damage) ? reinterpret_cast<void*>(&original_damage)
                                                                     : reinterpret_cast<void*>(&original_quest);
        return {};
    }
    std::string enable(void*) override {
        calls.push_back("enable");
        return {};
    }
};

struct FakeClient {
    std::atomic<bool> stopped{false};
    std::vector<std::string> log;
    hooks::Client client;
    FakeClient(const char* name, std::size_t load_index) {
        client.self = reinterpret_cast<cmfm_plugin_t*>(this);
        client.name = name;
        client.load_index = load_index;
        client.stopped = &stopped;
        client.log = [this](std::string_view level, std::string_view text) {
            log.push_back(std::string(level) + " " + std::string(text));
        };
    }
};

// The first `count` bytes of a function as a pattern in hex.
std::string bytes_of(const void* function, std::size_t count) {
    std::string text;
    const auto* bytes = static_cast<const unsigned char*>(function);
    for (std::size_t k = 0; k < count; ++k) {
        char hex[4]{};
        std::snprintf(hex, sizeof(hex), "%02X", bytes[k]);
        text += (k ? " " : "") + std::string(hex);
    }
    return text;
}

// Literals and the named signature strings below outlive each registry call.
// A const std::string& here would convert literals to temporary strings and
// return a dangling c_str() before the hook was registered.
cmfm_hook_t hook_of(const char* name, std::string_view pattern, void* address, void* detour, void** next,
                    std::int32_t order = 0) {
    return {sizeof(cmfm_hook_t), order, name, pattern.data(), 0, 0, address, detour, next};
}

int call_damage(StandInInstaller& installer, int a, int b) {
    g_trace.clear();
    const auto head = installer.heads[reinterpret_cast<void*>(&game_damage)];
    return reinterpret_cast<DamageFn>(*head)(a, b);
}
}  // namespace

int main() {
    cmfm::plugins::Scanner scanner(GetModuleHandleW(nullptr), L"");
    hooks::Registry registry(GetModuleHandleW(nullptr), [&](const cmfm::Pattern& pattern) {
        return scanner.find(pattern, CMFM_SCAN_CODE).matches;
    });
    FakeClient a("alpha", 0), b("beta", 1), c("gamma", 2);
    const auto damage_bytes = bytes_of(reinterpret_cast<const void*>(&game_damage), 16);
    const auto damage = reinterpret_cast<void*>(&game_damage);
    std::string problem;

    // Checks on the target before anything is registered.
    auto bad = hook_of("short", "8B C1", damage, reinterpret_cast<void*>(&detour_a), &g_next_unused);
    check(!registry.hook(a.client, bad, problem) && problem.find("shorter than 5 bytes") != std::string::npos,
          "short pattern refused");
    bad = hook_of("wrong", "90 90 90 90 90 90", damage, reinterpret_cast<void*>(&detour_a), &g_next_unused);
    check(!registry.hook(a.client, bad, problem) && problem.find("do not match") != std::string::npos,
          "bytes that differ refused");
    const auto data_bytes = bytes_of(g_data, 8);
    bad = hook_of("data", data_bytes, g_data, reinterpret_cast<void*>(&detour_a), &g_next_unused);
    check(!registry.hook(a.client, bad, problem) && problem.find("outside the game's code") != std::string::npos,
          "data is not code");
    bad = hook_of("padding", "CC CC CC CC CC CC CC CC", nullptr, reinterpret_cast<void*>(&detour_a), &g_next_unused);
    check(!registry.hook(a.client, bad, problem) && problem.find("not once") != std::string::npos,
          "a pattern must match once");
    bad = hook_of("no next", damage_bytes, damage, reinterpret_cast<void*>(&detour_a), nullptr);
    check(!registry.hook(a.client, bad, problem) && problem == "no detour or no next", "no next refused");
    bad = hook_of("small", damage_bytes, damage, reinterpret_cast<void*>(&detour_a), &g_next_unused);
    bad.size = sizeof(cmfm_hook_t) - 8;
    check(!registry.hook(a.client, bad, problem), "a struct smaller than 1.0's refused");

    // Before the hook layer: pending, next still null.
    g_next_a = g_next_b = reinterpret_cast<void*>(1);
    const auto hook_a = hook_of("Damage", damage_bytes, damage, reinterpret_cast<void*>(&detour_a), &g_next_a, 0);
    const auto handle_a = registry.hook(a.client, hook_a, problem);
    const auto hook_b = hook_of(nullptr, damage_bytes, nullptr, reinterpret_cast<void*>(&detour_b), &g_next_b, -5);
    const auto handle_b = registry.hook(b.client, hook_b, problem);
    check(handle_a && handle_b && handle_a != handle_b, "handles");
    check(registry.state(a.client, handle_a) == CMFM_HOOK_PENDING && g_next_a == nullptr && g_next_b == nullptr,
          "pending before the hook layer, next cleared");
    check(registry.hook(a.client, hook_a, problem) == 0 && problem == "this plugin already hooks that function",
          "the same function twice refused");

    StandInInstaller installer;
    registry.set_installer(&installer);
    check(installer.calls == std::vector<std::string>{"create", "enable"}, "one hook per function");
    check(registry.state(a.client, handle_a) == CMFM_HOOK_ACTIVE, "active once the hook layer is in");
    const auto expected = game_damage(2, 3);
    check(call_damage(installer, 2, 3) == expected + 1 &&
              g_trace == std::vector<std::string>{"b", "a", "original"}, "order: beta (order -5), alpha, original");

    // A late detour joins a running chain; alpha and gamma tie on order.
    const auto hook_c = hook_of("Damage", damage_bytes, damage, reinterpret_cast<void*>(&detour_c), &g_next_c, 0);
    const auto handle_c = registry.hook(c.client, hook_c, problem);
    check(handle_c && registry.state(c.client, handle_c) == CMFM_HOOK_ACTIVE && installer.calls.size() == 2,
          "late join without a second hook");
    call_damage(installer, 1, 1);
    check(g_trace == std::vector<std::string>{"b", "a", "c", "original"}, "ties in load order");

    // Off and on again.
    check(registry.set_enabled(b.client, handle_b, false).empty() &&
              registry.state(b.client, handle_b) == CMFM_HOOK_DISABLED, "switched off");
    check(call_damage(installer, 2, 3) == expected && g_trace == std::vector<std::string>{"a", "c", "original"},
          "the entry skips a detour that is off");
    check(registry.set_enabled(a.client, handle_b, true) == "the handle belongs to another plugin",
          "only the owner switches it");
    registry.set_enabled(b.client, handle_b, true);
    call_damage(installer, 1, 1);
    check(g_trace == std::vector<std::string>{"b", "a", "c", "original"}, "back in its place");
    check(registry.set_enabled(a.client, 999, false) == "unknown handle", "unknown handle");

    // A pattern search; the hook goes in at once.
    const auto quest_bytes = bytes_of(reinterpret_cast<const void*>(&game_quest), 16);
    const auto hook_q = hook_of("Quest", quest_bytes, nullptr, reinterpret_cast<void*>(&detour_quest), &g_next_quest);
    const auto handle_q = registry.hook(a.client, hook_q, problem);
    const auto quest_head = installer.heads[reinterpret_cast<void*>(&game_quest)];
    g_trace.clear();
    check(handle_q && quest_head && reinterpret_cast<QuestFn>(*quest_head)(5) == game_quest(5) &&
              g_trace == std::vector<std::string>{"quest", "original quest"}, "found by pattern, active at once");

    // The hook layer refuses a function.
    installer.refuse = "the target is already patched by something CMFM cannot relocate";
    const auto other_bytes = bytes_of(reinterpret_cast<const void*>(&game_other), 16);
    void* next_other = reinterpret_cast<void*>(1);
    const auto hook_o = hook_of("Other", other_bytes, reinterpret_cast<void*>(&game_other),
                                reinterpret_cast<void*>(&detour_quest), &next_other);
    check(!registry.hook(c.client, hook_o, problem) && problem.find("could not be hooked: the target is already") !=
                                                            std::string::npos && next_other == nullptr,
          "refused by the hook layer");
    check(!c.log.empty() && c.log.back().find("WARN cmfm.hooks: Other failed:") == 0, "the plugin is told");
    void* next_late = nullptr;
    const auto hook_late = hook_of("Other", other_bytes, reinterpret_cast<void*>(&game_other),
                                   reinterpret_cast<void*>(&detour_quest), &next_late);
    check(!registry.hook(a.client, hook_late, problem) && problem.find("that function could not be hooked") == 0,
          "a failed function stays failed");

    const auto lines = registry.summary();
    bool chain_line = false, failed_line = false;
    for (const auto& line : lines) {
        if (line.find("name=Damage state=active layers=beta,alpha,gamma") != std::string::npos) chain_line = true;
        if (line.find("name=Other state=failed layers=gamma reason=the target is already patched") != std::string::npos) {
            failed_line = true;
        }
    }
    check(chain_line && failed_line, "summary");
    if (failures) {
        for (const auto& line : lines) std::cerr << line << '\n';
        std::cerr << failures << " cmfm.hooks check(s) failed\n";
        return 1;
    }
    std::cout << "cmfm.hooks checks targets, orders and relinks chains, and reports failures\n";
    return 0;
}
