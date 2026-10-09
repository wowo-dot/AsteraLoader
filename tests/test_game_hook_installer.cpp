#include "core/game_hooks/installer.hpp"
#include <windows.h>
#include <MinHook.h>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using namespace cmfm::game_hooks;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
using Eight = double (*)(double, double, double, double, double, double, double, double);
void* original{};
bool valid_arguments{}, valid_error{};
volatile unsigned salt = 23;
__declspec(noinline) double target(double a, double b, double c, double d, double e, double f, double g, double h) {
    valid_arguments = a == 1.25 && b == -2.5 && c == 3.75 && d == -4.125 && e == 5.5 && f == -6.75 && g == 7.125 && h == -8.5;
    valid_error = GetLastError() == 0x1234;
    return a + b + c + d + e + f + g + h + salt;
}
double detour(double a, double b, double c, double d, double e, double f, double g, double h) {
    return std::bit_cast<Eight>(original)(a, b, c, d, e, f, g, h) + 1;
}
double other_detour(double a, double b, double c, double d, double e, double f, double g, double h) {
    return std::bit_cast<Eight>(original)(a, b, c, d, e, f, g, h) + 2;
}
double call(Eight function) {
    SetLastError(0x1234); return function(1.25, -2.5, 3.75, -4.125, 5.5, -6.75, 7.125, -8.5);
}
__declspec(noinline) int late_target(int x) { return x * 7919 + static_cast<int>(salt); }
void *next_a{}, *next_b{};
int late_a(int x) { return std::bit_cast<decltype(&late_target)>(next_a)(x) + 1; }
int late_b(int x) { return std::bit_cast<decltype(&late_target)>(next_b)(x) + 2; }
// Original synthetic code in an executable image section: five padding
// bytes, then xor eax,eax; ret, then non-padding. MinHook must patch above.
#pragma section(".hooktest", read, execute)
__declspec(allocate(".hooktest")) const unsigned char hot_code[]{
    0x90, 0x90, 0x90, 0x90, 0x90, 0x31, 0xc0, 0xc3, 0xb8, 0x12, 0x34, 0x56, 0x78, 0xc3,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
using Hot = int (*)();
void *hot_next_a{}, *hot_next_b{};
int hot_a() { return std::bit_cast<Hot>(hot_next_a)() + 1; }
int hot_b() { return std::bit_cast<Hot>(hot_next_b)() + 2; }
std::string signature(void* function) {
    std::string result;
    for (unsigned i = 0; i < 16; ++i) {
        char text[4]{}; std::snprintf(text, sizeof(text), "%02X", static_cast<unsigned char*>(function)[i]);
        if (i) result += ' '; result += text;
    }
    return result;
}
}
int main() {
    try {
        require(MH_Initialize() == MH_OK, "MinHook initialization");
        NativeInstaller installer({});
        void* head = nullptr;
        const auto function = std::bit_cast<void*>(&target);
        std::array<unsigned char, 16> before{};
        std::memcpy(before.data(), function, before.size());
        const auto baseline = call(&target);
        require(installer.create(function, &head, &original).empty() && original &&
            !std::memcmp(before.data(), function, before.size()) && call(&target) == baseline,
            "create did not leave the hook inactive");
        require(!installer.enable(function).empty() && !std::memcmp(before.data(), function, before.size()), "null chain head enabled");
        InterlockedExchangePointer(&head, std::bit_cast<void*>(&detour));
        require(installer.enable(function).empty() && call(&target) == baseline + 1 && valid_arguments && valid_error,
            "enabled entry changed XMM/stack arguments or last error");
        InterlockedExchangePointer(&head, std::bit_cast<void*>(&other_detour));
        cmfm::graphics::begin_internal();
        require(call(&target) == baseline + 2 && valid_arguments, "head change failed or d3d12 internal bypassed game hook");
        cmfm::graphics::end_internal();
        const auto sentinel = reinterpret_cast<void*>(0x1234);
        void* duplicate_original = sentinel;
        require(!installer.create(function, &head, &duplicate_original).empty() && duplicate_original == sentinel,
            "duplicate create changed original storage");
        require(!installer.enable(std::bit_cast<void*>(&late_target)).empty(), "unknown target enabled");
        require(!installer.create(function, nullptr, &duplicate_original).empty(), "missing head accepted");
        const auto unsupported = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        require(unsupported != nullptr, "unsupported fixture allocation");
        std::memset(unsupported, 0x90, 32);
        unsupported[0] = 0xe2; unsupported[1] = 0x10; // LOOP outside the relocated prefix: unsupported by MinHook.
        unsupported[18] = 0xc3;
        DWORD old{}; require(VirtualProtect(unsupported, 4096, PAGE_EXECUTE_READ, &old) != FALSE, "fixture protection");
        const auto problem = installer.create(unsupported, &head, &duplicate_original);
        require(problem.find("cannot relocate") != std::string::npos && duplicate_original == sentinel,
            "unrelocatable instruction accepted or missing reason");
        VirtualFree(unsupported, 0, MEM_RELEASE);

        // A native patch, then an address-based late join using its original
        // signature. This complements the fake game's pattern-based join.
        auto* registry = new Registry(GetModuleHandleW(nullptr), {}); // Head storage is process-lifetime.
        registry->set_installer(&installer);
        Client a, b; a.name = "address_a"; b.name = "address_b"; b.load_index = 1;
        const auto late = std::bit_cast<void*>(&late_target);
        const auto bytes = signature(late);
        const auto expected = late_target(7);
        std::string why;
        const cmfm_hook_t first{sizeof(first), 0, "LateAddress", bytes.c_str(), 0, 0, late, std::bit_cast<void*>(&late_a), &next_a};
        require(registry->hook(a, first, why) != 0 && why.empty() && late_target(7) == expected + 1, "first native registry hook");
        std::memcpy(before.data(), late, before.size());
        const cmfm_hook_t second{sizeof(second), 0, "LateAddress", bytes.c_str(), 0, 0, late, std::bit_cast<void*>(&late_b), &next_b};
        require(registry->hook(b, second, why) != 0 && why.empty() && late_target(7) == expected + 3 &&
            !std::memcmp(before.data(), late, before.size()), "late address join rejected or repatched");
        Client wrong; wrong.name = "wrong";
        void* wrong_next = sentinel;
        const cmfm_hook_t bad{sizeof(bad), 0, "Wrong", "90 90 90 90 90 90", 0, 0, late, std::bit_cast<void*>(&late_b), &wrong_next};
        require(!registry->hook(wrong, bad, why) && why.find("do not match") != std::string::npos && wrong_next == sentinel,
            "late join ignored a signature mismatch");
        auto hot = const_cast<unsigned char*>(hot_code + 5);
        const auto hot_bytes = signature(hot);
        const cmfm_hook_t hot_first{sizeof(hot_first), 0, "HotPatch", hot_bytes.c_str(), 0, 0, hot, std::bit_cast<void*>(&hot_a), &hot_next_a};
        const auto hot_handle = registry->hook(a, hot_first, why);
        if (!hot_handle) std::cerr << why << '\n';
        const volatile unsigned char* hot_live = hot;
        require(hot_handle && std::bit_cast<Hot>(hot)() == 1 && hot_live[0] == 0xeb && hot_live[-5] == 0xe9,
            "MinHook hotpatch fixture did not activate");
        const cmfm_hook_t hot_second{sizeof(hot_second), 0, "HotPatch", hot_bytes.c_str(), 0, 0, hot, std::bit_cast<void*>(&hot_b), &hot_next_b};
        require(registry->hook(b, hot_second, why) && std::bit_cast<Hot>(hot)() == 3, "hotpatch footprint rejected a late join");
        std::cout << "Game-hook inactive creation, ABI, live head, relocation refusals and late address join passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
