// "al.patch" core side with a stand-in writer: checks and refusals, the
// claims shared with "al.hooks", pattern checks against the original bytes,
// pending changes, switching off and on, someone else's change, calls sent
// elsewhere, and SHUTDOWN. Functions and tables in this exe stand in for the
// game's code and data. The stand-in writes plainly (this test runs on one
// thread); the native writer has its own tests.
#include "core/game_hooks/registry.hpp"
#include "core/image/claims.hpp"
#include "core/image/footprint.hpp"
#include "core/image/layout.hpp"
#include "core/patch/registry.hpp"
#include "core/plugins/services.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace hooks = al::game_hooks;
namespace image = al::image;
namespace patch = al::patch;

namespace {
int failures = 0;
void check(bool condition, const std::string& label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        ++failures;
    }
}
bool has(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

// Stand-in game code; the constants make each function's bytes unique.
// Expected: 81 01 3C 3C 3C 00 (add [rcx], imm32), 81 41 04 4B 4B 4B 00, C3.
__declspec(noinline) void game_bump(int* p) {
    p[0] += 0x3C3C3C;
    p[1] += 0x4B4B4B;
}
// Expected: B8 CD AB 34 12 (mov eax, imm32), C3.
__declspec(noinline) int game_constant() {
    return 0x1234ABCD;
}
__declspec(noinline) int game_callee(int x) {
    return x * 3 + 0x2D2D;
}
__declspec(noinline) int game_caller(int x) {
    return game_callee(x + 0x1111) * 5 + 0x77;
}
__declspec(noinline) int game_quest(int a) {
    return (a ^ 0x13579BDF) * 31337;
}
// Called through these, so the compiler knows nothing about the calls.
void (*volatile g_bump)(int*) = &game_bump;
int (*volatile g_constant)() = &game_constant;
int (*volatile g_caller)(int) = &game_caller;

// Stand-in game data: read-only pointers (CMFM's Start menu case) and a
// writable table.
extern const std::uint64_t g_rdata[2];
const std::uint64_t g_rdata[2] = {0x5EC7A11E0001F00Dull, 0x5EC7A11E0002F00Dull};
alignas(16) std::uint8_t g_table[32] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x17, 0x28, 0x39, 0x4A, 0x5B,
                                        0x6C, 0x7D, 0x8E, 0x9F, 0xA0, 0xB1, 0xC2, 0xD3, 0xE4, 0xF5, 0x06,
                                        0x18, 0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x90, 0xA2};
// A section named as Steam's DRM stub's.
#pragma section(".bind", read)
__declspec(allocate(".bind")) extern const std::uint8_t g_drm[16] = {0xD5, 0xA1, 0x3E, 0x77, 0x42, 0x19, 0xC8, 0x0B,
                                                                     0x64, 0xF2, 0x8D, 0x31, 0x5A, 0xE6, 0x2F, 0x93};

std::uint64_t load64(const volatile std::uint64_t* at) {
    return *at;
}
std::uint8_t load8(const volatile std::uint8_t* at) {
    return *at;
}

// A plugin's function for the call change.
void* g_callee_original = nullptr;
int plugin_callee(int x) {
    return reinterpret_cast<int (*)(int)>(g_callee_original)(x) + 1000000;
}

// Writes bytes in this exe, whatever the page's protection.
bool poke(void* at, const void* bytes, std::size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(at, bytes, size);
    VirtualProtect(at, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return true;
}

class StandInWriter : public patch::Writer {
public:
    std::vector<bool> code;  // each write's `code`
    std::string refuse;      // fails every write with this
    std::string refuse_stubs;
    int stubs = 0;

    Result write(std::uint8_t* at, const std::uint8_t* expect, const std::uint8_t* bytes, std::size_t size,
                 bool is_code, std::uint8_t* found, std::string& problem) override {
        code.push_back(is_code);
        if (!refuse.empty()) {
            problem = refuse;
            return Result::failed;
        }
        if (std::memcmp(at, expect, size) != 0) {
            std::memcpy(found, at, size);
            return Result::differs;
        }
        if (!poke(at, bytes, size)) {
            problem = "VirtualProtect failed";
            return Result::failed;
        }
        return Result::written;
    }
    // Functions in this exe are within a rel32's reach of each other.
    std::string stub(const void*, void* function, void** stub) override {
        if (!refuse_stubs.empty()) return refuse_stubs;
        ++stubs;
        *stub = function;
        return {};
    }
};

// A hook layer that writes a jump at the entry, as MinHook would, so the
// claims' original bytes matter.
class StandInInstaller : public hooks::Installer {
public:
    std::string create(void* target, void* const*, void** original) override {
        const std::uint8_t jump[5] = {0xE9, 0, 0, 0, 0};
        if (!poke(target, jump, sizeof(jump))) return "VirtualProtect failed";
        *original = target;
        return {};
    }
    std::string enable(void*) override { return {}; }
};

struct FakeClient {
    std::atomic<bool> stopped{false};
    std::vector<std::string> log;
    hooks::Client client;
    FakeClient(const char* name, std::size_t load_index) {
        client.self = reinterpret_cast<al_plugin_t*>(this);
        client.name = name;
        client.load_index = load_index;
        client.stopped = &stopped;
        client.log = [this](std::string_view level, std::string_view text) {
            log.push_back(std::string(level) + " " + std::string(text));
        };
    }
};

std::string hex(const void* at, std::size_t count) {
    return patch::hex_bytes(static_cast<const std::uint8_t*>(at), count, count);
}

bool starts_with(const void* at, std::initializer_list<std::uint8_t> bytes) {
    return std::memcmp(at, std::data(bytes), bytes.size()) == 0;
}

// The E8 in `function`'s first bytes that calls `callee`.
std::uint8_t* call_to(void* function, const void* callee) {
    auto* bytes = static_cast<std::uint8_t*>(function);
    for (std::size_t k = 0; k < 48; ++k) {
        if (bytes[k] != 0xE8) continue;
        std::int32_t rel = 0;
        std::memcpy(&rel, bytes + k + 1, sizeof(rel));
        if (bytes + k + 5 + rel == callee) return bytes + k;
    }
    return nullptr;
}

al_patch_t patch_of(std::uint32_t kind, const char* name, const void* address, std::uint32_t length,
                    const void* expected, const void* replacement, const char* pattern = nullptr,
                    std::int32_t offset = 0) {
    al_patch_t change{};
    change.size = sizeof(al_patch_t);
    change.kind = kind;
    change.name = name;
    change.pattern = pattern;
    change.offset = offset;
    change.length = length;
    change.address = const_cast<void*>(address);
    change.expected = expected;
    change.replacement = replacement;
    return change;
}

al_hook_t hook_of(const char* name, const char* pattern, void* address, void* detour, void** next) {
    return {56, 0, name, pattern, 0, 0, address, detour, next};
}

// One summary line for the change named `name`.
std::string line_of(const std::vector<std::string>& lines, std::string_view name) {
    for (const auto& line : lines) {
        if (has(line, " name=" + std::string(name) + " ")) return line;
    }
    return {};
}
}  // namespace

int main() {
    const auto* exe = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    auto* bump = reinterpret_cast<std::uint8_t*>(&game_bump);
    auto* constant = reinterpret_cast<std::uint8_t*>(&game_constant);
    auto* call_site = call_to(reinterpret_cast<void*>(&game_caller), reinterpret_cast<void*>(&game_callee));
    const bool shapes = starts_with(bump, {0x81, 0x01, 0x3C, 0x3C, 0x3C, 0x00, 0x81, 0x41, 0x04, 0x4B, 0x4B, 0x4B,
                                           0x00, 0xC3}) &&
                        starts_with(constant, {0xB8, 0xCD, 0xAB, 0x34, 0x12, 0xC3}) && call_site;
    if (!shapes) {
        std::cerr << "FAIL: the stand-in game code compiled differently than this test expects\n"
                  << "game_bump: " << hex(bump, 16) << "\ngame_constant: " << hex(constant, 8)
                  << "\ngame_caller: " << hex(reinterpret_cast<const void*>(&game_caller), 32) << '\n';
        return 1;
    }
    // The game's bytes, before any change.
    const std::string bump_pattern = hex(bump, 14);
    const std::string constant_pattern = hex(constant, 6);
    const std::string quest_pattern = hex(reinterpret_cast<const void*>(&game_quest), 16);
    const std::string rdata_pattern = hex(g_rdata, 16);

    al::plugins::Scanner scanner(exe, L"");
    image::Claims claims;
    hooks::Registry hook_registry(
        exe, [&](const al::Pattern& pattern) { return scanner.find(pattern, AL_SCAN_CODE).matches; }, &claims);
    patch::Registry registry(
        exe,
        [&](const al::Pattern& pattern, bool code) {
            return scanner.find(pattern, code ? AL_SCAN_CODE : AL_SCAN_IMAGE).matches;
        },
        claims);
    FakeClient alpha("alpha", 0), beta("beta", 1), gamma("gamma", 2);
    std::string problem;
    const std::array<std::uint8_t, 8> nops{0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

    // Refusals, each with its reason.
    std::array<std::uint8_t, 16> current{};
    std::memcpy(current.data(), constant, 6);
    auto bad = patch_of(AL_PATCH_CODE, "small", constant, 1, current.data(), nops.data());
    bad.size = 64;
    check(!registry.apply(alpha.client, bad, problem) && has(problem, "smaller than al.patch 1.0"), "a struct smaller than 1.0's");
    bad = patch_of(7, "kind", constant, 1, current.data(), nops.data());
    check(!registry.apply(alpha.client, bad, problem) && problem == "unknown kind 7", "unknown kind");
    bad = patch_of(AL_PATCH_CODE, "empty", constant, 0, current.data(), nops.data());
    check(!registry.apply(alpha.client, bad, problem) && has(problem, "must be 1 to 64, not 0"), "length 0");
    bad = patch_of(AL_PATCH_CODE, "long", constant, 65, current.data(), nops.data());
    check(!registry.apply(alpha.client, bad, problem) && has(problem, "not 65"), "length 65");
    bad = patch_of(AL_PATCH_CALL, "call 4", call_site, 4, call_site, nullptr);
    bad.function = reinterpret_cast<void*>(&plugin_callee);
    check(!registry.apply(alpha.client, bad, problem) && has(problem, "5 bytes long, not 4"), "a call is 5 bytes");
    bad = patch_of(AL_PATCH_CODE, "nothing", constant, 1, nullptr, nops.data());
    check(!registry.apply(alpha.client, bad, problem) && problem == "no expected or no replacement bytes", "no expected bytes");
    bad = patch_of(AL_PATCH_CALL, "mov", constant, 5, current.data(), nullptr);
    bad.function = reinterpret_cast<void*>(&plugin_callee);
    check(!registry.apply(alpha.client, bad, problem) && has(problem, "start with B8"), "a call needs E8 or E9");
    bad = patch_of(AL_PATCH_CODE, "nowhere", nullptr, 1, current.data(), nops.data());
    check(!registry.apply(alpha.client, bad, problem) && problem == "no address and no pattern", "no address, no pattern");
    bad = patch_of(AL_PATCH_CODE, "unreadable", nullptr, 1, current.data(), nops.data(), "ZZ QQ");
    check(!registry.apply(alpha.client, bad, problem) && problem == "the pattern is unreadable", "unreadable pattern");
    bad = patch_of(AL_PATCH_CODE, "differs", constant, 5, nops.data(), nops.data());
    check(!registry.apply(alpha.client, bad, problem) && has(problem, "are B8 CD AB 34 12, not the expected 90 90"),
          "expected bytes differ: " + problem);
    bad = patch_of(AL_PATCH_CODE, "padding", nullptr, 1, nops.data(), nops.data(), "CC CC CC CC CC CC CC CC");
    check(!registry.apply(alpha.client, bad, problem) && has(problem, "times in the game's code, not once"),
          "a pattern must match once");
    bad = patch_of(AL_PATCH_CODE, "elsewhere", constant, 5, current.data(), nops.data(), "B8 11 22 33 44");
    check(!registry.apply(alpha.client, bad, problem) && problem == "the pattern does not match at the address",
          "pattern and address disagree");

    // Places AL does not change.
    const auto refused_at = [&](std::uint32_t kind, const void* at, std::size_t length, std::string_view why,
                                const std::string& label) {
        std::array<std::uint8_t, 16> there{};
        std::memcpy(there.data(), at, length);
        auto change = patch_of(kind, "place", at, static_cast<std::uint32_t>(length), there.data(), there.data());
        check(!registry.apply(alpha.client, change, problem) && has(problem, why), label + ": " + problem);
    };
    std::vector<std::uint8_t> heap(16, 0x42);
    refused_at(AL_PATCH_DATA, heap.data(), 8, "outside the game exe", "heap memory");
    refused_at(AL_PATCH_DATA, exe, 2, "the exe's headers", "the headers");
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(exe + reinterpret_cast<const IMAGE_DOS_HEADER*>(exe)->e_lfanew);
    const auto& pdata = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const auto& iat = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
    check(pdata.Size && iat.Size, "this exe has exception data and an import address table");
    refused_at(AL_PATCH_DATA, exe + pdata.VirtualAddress, 8, "a data directory", "exception data");
    refused_at(AL_PATCH_DATA, exe + iat.VirtualAddress, 8, "a data directory", "the import address table");
    refused_at(AL_PATCH_DATA, g_drm, 8, "the DRM stub's section", "the .bind section");
    refused_at(AL_PATCH_CODE, g_rdata, 8, "a code or call change goes in the game's code", "code kind in data");
    refused_at(AL_PATCH_DATA, constant, 5, "a data change goes in the game's data", "data kind in code");
    check(alpha.log.empty(), "the registry leaves logging to the host");

    // Before the writer (PRE_MAIN): checked and claimed, not written.
    std::array<std::uint8_t, 7> bump_add{};
    std::memcpy(bump_add.data(), bump + 6, bump_add.size());
    const auto nop_add = patch_of(AL_PATCH_CODE, "bump: skip p[1]", nullptr, 7, bump_add.data(), nops.data(),
                                  bump_pattern.c_str(), 6);
    const auto handle_nop = registry.apply(alpha.client, nop_add, problem);
    const std::uint64_t swapped = 0x5EC7A11E0003F00Dull;
    std::uint64_t first = 0;
    std::memcpy(&first, &g_rdata[0], sizeof(first));
    const auto swap = patch_of(AL_PATCH_DATA, "Start menu: System tab items", &g_rdata[0], 8, &first, &swapped);
    const auto handle_swap = registry.apply(alpha.client, swap, problem);
    std::array<std::uint8_t, 4> table_head{};
    std::memcpy(table_head.data(), g_table, table_head.size());
    const std::array<std::uint8_t, 4> zeros{};
    const auto off_early = patch_of(AL_PATCH_DATA, "table head", g_table, 4, table_head.data(), zeros.data());
    const auto handle_off = registry.apply(beta.client, off_early, problem);
    std::array<std::uint8_t, 4> table_tail{};
    std::memcpy(table_tail.data(), g_table + 28, table_tail.size());
    const auto moved = patch_of(AL_PATCH_DATA, "table tail", g_table + 28, 4, table_tail.data(), zeros.data());
    const auto handle_moved = registry.apply(beta.client, moved, problem);
    check(handle_nop && handle_swap && handle_off && handle_moved, "made before the writer: " + problem);
    check(registry.state(alpha.client, handle_nop) == AL_PATCH_PENDING &&
              registry.state(alpha.client, handle_swap) == AL_PATCH_PENDING,
          "pending before the writer");
    int values[2] = {1, 2};
    g_bump(values);
    check(values[0] == 1 + 0x3C3C3C && values[1] == 2 + 0x4B4B4B && load64(&g_rdata[0]) == first,
          "nothing written before the writer");
    check(registry.set_enabled(beta.client, handle_off, false).empty(), "a pending change can be switched off");
    const std::uint8_t poked = 0x66;
    poke(g_table + 29, &poked, 1);  // behind AL's back, before the writer

    // The writer is ready: the pending changes are written.
    StandInWriter writer;
    registry.set_writer(&writer);
    check(registry.state(alpha.client, handle_nop) == AL_PATCH_ACTIVE &&
              registry.state(alpha.client, handle_swap) == AL_PATCH_ACTIVE,
          "written when the writer is ready");
    check(registry.state(beta.client, handle_off) == AL_PATCH_DISABLED && load8(g_table) == table_head[0],
          "a change switched off while pending is not written");
    check(registry.state(beta.client, handle_moved) == AL_PATCH_FAILED && load8(g_table + 29) == poked,
          "a pending change whose bytes changed fails and is left alone");
    check(!beta.log.empty() && beta.log.back().find("WARN al.patch: table tail failed: ") == 0 &&
              has(beta.log.back(), "something outside Astera Loader changed them"),
          "the plugin is told why");
    values[0] = 1;
    values[1] = 2;
    g_bump(values);
    check(values[0] == 1 + 0x3C3C3C && values[1] == 2, "CODE: the NOPed add no longer runs");
    check(load64(&g_rdata[0]) == swapped, "DATA: the read-only pointer is swapped");
    check(writer.code == std::vector<bool>{true, false, false}, "code and data writes are told apart");

    // Off and on.
    check(registry.set_enabled(alpha.client, handle_nop, false).empty() &&
              registry.state(alpha.client, handle_nop) == AL_PATCH_DISABLED,
          "CODE switched off");
    values[1] = 2;
    g_bump(values);
    check(values[1] == 2 + 0x4B4B4B, "the add runs again");
    check(registry.set_enabled(alpha.client, handle_nop, true).empty(), "CODE switched on");
    values[1] = 2;
    g_bump(values);
    check(values[1] == 2, "the add is skipped again");
    check(registry.set_enabled(beta.client, handle_nop, false) == "the handle belongs to another plugin" &&
              registry.state(beta.client, handle_nop) == AL_PATCH_FAILED,
          "only the owner switches or sees it");
    check(registry.set_enabled(alpha.client, 999, false) == "unknown handle", "unknown handle");
    check(has(registry.set_enabled(beta.client, handle_moved, true), "the change failed: the bytes at"),
          "a failed change stays failed");

    // One owner per byte.
    const std::uint32_t half = 0x0BADF00D;
    auto overlap = patch_of(AL_PATCH_DATA, "half", reinterpret_cast<const std::uint8_t*>(&g_rdata[0]) + 4, 4,
                            reinterpret_cast<const std::uint8_t*>(&g_rdata[0]) + 4, &half);
    check(!registry.apply(beta.client, overlap, problem) &&
              problem == "the bytes overlap the plugin alpha's change \"Start menu: System tab items\"",
          "two plugins on one range: " + problem);
    // A hook next to a patch, found by a pattern over the patched bytes.
    void* next_bump = nullptr;
    auto hook_bump = hook_of("Bump", bump_pattern.c_str(), nullptr, reinterpret_cast<void*>(&game_quest), &next_bump);
    const auto hook_handle = hook_registry.hook(gamma.client, hook_bump, problem);
    check(hook_handle != 0, "a hook's pattern over a patched range matches the original bytes: " + problem);
    const auto footprint = image::hook_footprint(bump, 32);
    check(footprint.begin == reinterpret_cast<std::uintptr_t>(bump) &&
              footprint.end == reinterpret_cast<std::uintptr_t>(bump) + 6,
          "a hook takes the first whole instruction");
    // A hook over a patch.
    std::uint8_t constant_bytes[4]{};
    std::memcpy(constant_bytes, constant + 1, 4);
    const std::uint32_t other_constant = 0x0FEDCBA9;
    const auto value = patch_of(AL_PATCH_CODE, "constant", constant + 1, 4, constant_bytes, &other_constant);
    const auto handle_value = registry.apply(alpha.client, value, problem);
    check(handle_value && g_constant() == 0x0FEDCBA9, "CODE: a same-length change: " + problem);
    void* next_constant = nullptr;
    auto hook_constant = hook_of("Constant", constant_pattern.c_str(), constant, reinterpret_cast<void*>(&game_quest), &next_constant);
    check(!hook_registry.hook(beta.client, hook_constant, problem) &&
              problem == "the function's first bytes overlap the plugin alpha's change \"constant\"",
          "a hook over a patch is refused: " + problem);
    // A patch on a hook's copied bytes.
    void* next_quest = nullptr;
    auto hook_quest = hook_of("Quest", quest_pattern.c_str(), reinterpret_cast<void*>(&game_quest),
                              reinterpret_cast<void*>(&game_quest), &next_quest);
    check(hook_registry.hook(gamma.client, hook_quest, problem) != 0, "hook on Quest: " + problem);
    auto* quest = reinterpret_cast<std::uint8_t*>(&game_quest);
    auto inside = patch_of(AL_PATCH_CODE, "inside", quest + 2, 1, quest + 2, quest + 2);
    check(!registry.apply(beta.client, inside, problem) &&
              problem == "the bytes overlap the hook on Quest (plugin gamma)",
          "a patch on a hook's bytes is refused, naming the plugin: " + problem);

    // A call sent elsewhere.
    std::array<std::uint8_t, 5> call_bytes{};
    std::memcpy(call_bytes.data(), call_site, 5);
    const int before = g_caller(1);
    auto call = patch_of(AL_PATCH_CALL, "caller: callee", call_site, 5, call_bytes.data(), nullptr);
    call.function = reinterpret_cast<void*>(&plugin_callee);
    call.original = &g_callee_original;
    writer.refuse_stubs = "no free page within 2 GB";
    check(!registry.apply(alpha.client, call, problem) && problem == "no stub within reach: no free page within 2 GB" &&
              g_callee_original == reinterpret_cast<void*>(&game_callee) && g_caller(1) == before,
          "no stub: refused, nothing written");
    writer.refuse_stubs.clear();
    g_callee_original = nullptr;
    const auto handle_call = registry.apply(alpha.client, call, problem);
    check(handle_call && g_callee_original == reinterpret_cast<void*>(&game_callee) &&
              g_caller(1) == (game_callee(1 + 0x1111) + 1000000) * 5 + 0x77,
          "CALL: the plugin's function runs and calls the original: " + problem);
    registry.set_enabled(alpha.client, handle_call, false);
    check(g_caller(1) == before && std::memcmp(call_site, call_bytes.data(), 5) == 0, "CALL switched off");
    registry.set_enabled(alpha.client, handle_call, true);
    check(g_caller(1) != before && writer.stubs == 1, "CALL switched on, the stub reused");

    // The writer fails: nothing claimed is left behind.
    writer.refuse = "the write failed for the test";
    const std::array<std::uint8_t, 6> six{1, 2, 3, 4, 5, 6};
    const auto unaligned = patch_of(AL_PATCH_DATA, "unaligned", g_table + 9, 6, g_table + 9, six.data());
    check(!registry.apply(beta.client, unaligned, problem) && problem == "the write failed for the test",
          "a failed write is refused");
    writer.refuse.clear();
    const auto handle_unaligned = registry.apply(beta.client, unaligned, problem);
    check(handle_unaligned && std::memcmp(g_table + 9, six.data(), 6) == 0, "the same bytes again once it works");

    // Someone else's change.
    const std::uint8_t outside = 0x77;
    poke(g_table + 11, &outside, 1);
    const auto why = registry.set_enabled(beta.client, handle_unaligned, false);
    check(has(why, "are 01 02 77 04 05 06, not the replacement 01 02 03 04 05 06; something outside Astera Loader"),
          "switching off finds someone else's change: " + why);
    check(registry.state(beta.client, handle_unaligned) == AL_PATCH_FAILED && load8(g_table + 11) == outside,
          "FAILED, and the bytes are left alone");

    // A pattern over a hook's entry and a patch sees the game's bytes.
    StandInInstaller installer;
    hook_registry.set_installer(&installer);
    check(bump[0] == 0xE9 && hook_registry.state(gamma.client, hook_handle) == AL_HOOK_ACTIVE, "the hook is in");
    const std::uint8_t ret = 0xC3;
    const auto at_ret = patch_of(AL_PATCH_CODE, "bump: ret", nullptr, 1, &ret, &ret, bump_pattern.c_str(), 13);
    check(registry.apply(beta.client, at_ret, problem) != 0, "a pattern over a hook and a patch: " + problem);
    // A patch on its read-only data found by pattern, next to another.
    std::uint64_t second = 0;
    std::memcpy(&second, &g_rdata[1], sizeof(second));
    const std::uint64_t swapped_second = 0x5EC7A11E0004F00Dull;
    const auto by_pattern = patch_of(AL_PATCH_DATA, "Start menu: second", nullptr, 8, &second, &swapped_second,
                                     rdata_pattern.c_str(), 8);
    const auto handle_second = registry.apply(beta.client, by_pattern, problem);
    check(handle_second && load64(&g_rdata[1]) == swapped_second,
          "DATA by a pattern over another plugin's change: " + problem);

    // SHUTDOWN: DATA put back, code and calls stay.
    registry.finish();
    check(load64(&g_rdata[0]) == first && load64(&g_rdata[1]) == second, "DATA put back at SHUTDOWN");
    // (game_bump has the stand-in hook's jump now and is not called again.)
    check(g_constant() == 0x0FEDCBA9 && g_caller(1) != before && bump[6] == 0x90, "code and call changes stay");
    const auto lines = registry.summary();
    check(has(line_of(lines, "Start menu: System tab items"), "kind=data length=8 owner=alpha state=put_back_at_shutdown"),
          "summary: put back");
    check(has(line_of(lines, "caller: callee"), "kind=call length=5 owner=alpha state=active"), "summary: call");
    check(has(line_of(lines, "table head"), "owner=beta state=off"), "summary: off");
    check(has(line_of(lines, "unaligned"), "state=failed reason=the bytes at exe+0x"), "summary: failed");
    const auto rva = reinterpret_cast<std::uintptr_t>(&g_rdata[0]) - reinterpret_cast<std::uintptr_t>(exe);
    char target[40]{};
    std::snprintf(target, sizeof(target), "patch target=exe+0x%llx ", static_cast<unsigned long long>(rva));
    check(line_of(lines, "Start menu: System tab items").find(target) == 0, "summary: the RVA");

    if (failures) {
        for (const auto& line : lines) std::cerr << line << '\n';
        std::cerr << failures << " al.patch check(s) failed\n";
        return 1;
    }
    std::cout << "al.patch checks, claims, writes, switches and puts back changes\n";
    return 0;
}
