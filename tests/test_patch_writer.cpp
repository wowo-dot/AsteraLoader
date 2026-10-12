// "al.patch"'s native writer in this process: aligned data in one atomic
// step, longer data and code with the other threads paused, page protection
// restored, bytes that differ left alone, a busy thread running code while
// it changes, and call stubs (within reach, never writable).
#include "core/image/claims.hpp"
#include "core/patch/registry.hpp"
#include "core/patch/writer.hpp"
#include "core/plugins/services.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace patch = al::patch;

namespace {
int failures = 0;
void check(bool condition, const std::string& label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        ++failures;
    }
}

// Expected: 81 01 2A 2A 2A 00, 81 41 04 5D 5D 5D 00, C3.
__declspec(noinline) void game_spin(int* p) {
    p[0] += 0x2A2A2A;
    p[1] += 0x5D5D5D;
}
__declspec(noinline) int game_callee(int x) {
    return x * 7 + 0x3E3E;
}
__declspec(noinline) int game_caller(int x) {
    return game_callee(x + 0x2222) * 3 + 0x55;
}
void (*volatile g_spin)(int*) = &game_spin;
int (*volatile g_caller)(int) = &game_caller;

extern const std::uint64_t g_rdata[4];
const std::uint64_t g_rdata[4] = {0x7AB1E00000000001ull, 0x7AB1E00000000002ull, 0x7AB1E00000000003ull,
                                  0x7AB1E00000000004ull};

void* g_callee_original = nullptr;
int plugin_callee(int x) {
    return reinterpret_cast<int (*)(int)>(g_callee_original)(x) + 2000000;
}

DWORD protection(const void* at) {
    MEMORY_BASIC_INFORMATION memory{};
    VirtualQuery(at, &memory, sizeof(memory));
    return memory.Protect;
}
std::uint64_t load64(const volatile std::uint64_t* at) {
    return *at;
}
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
}  // namespace

int main() {
    patch::NativeWriter writer;
    std::string problem;
    std::array<std::uint8_t, 64> found{};
    using Result = patch::Writer::Result;

    // Aligned read-only data: one atomic step, protection back.
    auto* rdata = reinterpret_cast<std::uint8_t*>(const_cast<std::uint64_t*>(g_rdata));
    const auto rdata_protection = protection(rdata);
    std::uint64_t first = 0, swapped = 0x7AB1E000000000FFull, wrong = 0;
    std::memcpy(&first, rdata, 8);
    check(writer.write(rdata, reinterpret_cast<const std::uint8_t*>(&first),
                       reinterpret_cast<const std::uint8_t*>(&swapped), 8, false, found.data(), problem) ==
                  Result::written &&
              load64(&g_rdata[0]) == swapped && protection(rdata) == rdata_protection,
          "8 aligned bytes in read-only data, protection restored: " + problem);
    check(writer.write(rdata, reinterpret_cast<const std::uint8_t*>(&wrong),
                       reinterpret_cast<const std::uint8_t*>(&first), 8, false, found.data(), problem) ==
                  Result::differs &&
              std::memcmp(found.data(), &swapped, 8) == 0 && load64(&g_rdata[0]) == swapped,
          "bytes that differ: nothing written, the bytes found returned");
    for (const std::size_t size : {1u, 2u, 4u}) {
        std::array<std::uint8_t, 8> was{}, now{};
        std::memcpy(was.data(), rdata + 8, size);
        for (std::size_t k = 0; k < size; ++k) now[k] = static_cast<std::uint8_t>(was[k] ^ 0x5A);
        check(writer.write(rdata + 8, was.data(), now.data(), size, false, found.data(), problem) == Result::written &&
                  std::memcmp(rdata + 8, now.data(), size) == 0 &&
                  writer.write(rdata + 8, now.data(), was.data(), size, false, found.data(), problem) ==
                      Result::written,
              std::to_string(size) + " aligned bytes, and back");
    }
    // Unaligned: with the other threads paused.
    std::array<std::uint8_t, 6> six{}, sixed{1, 2, 3, 4, 5, 6};
    std::memcpy(six.data(), rdata + 17, 6);
    check(writer.write(rdata + 17, six.data(), sixed.data(), 6, false, found.data(), problem) == Result::written &&
              std::memcmp(rdata + 17, sixed.data(), 6) == 0 && protection(rdata + 17) == rdata_protection,
          "6 unaligned bytes: " + problem);
    check(writer.write(rdata + 17, six.data(), sixed.data(), 6, false, found.data(), problem) == Result::differs &&
              std::memcmp(found.data(), sixed.data(), 6) == 0,
          "6 unaligned bytes that differ");

    // Across two read-only pages.
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    auto* pages = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 2 * info.dwPageSize, MEM_COMMIT | MEM_RESERVE,
                                                          PAGE_READWRITE));
    std::memset(pages, 0x11, 2 * info.dwPageSize);
    DWORD old = 0;
    VirtualProtect(pages, 2 * info.dwPageSize, PAGE_READONLY, &old);
    auto* across = pages + info.dwPageSize - 3;
    const std::array<std::uint8_t, 8> elevens{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11},
        eights{8, 8, 8, 8, 8, 8, 8, 8};
    check(writer.write(across, elevens.data(), eights.data(), 8, false, found.data(), problem) == Result::written &&
              std::memcmp(across, eights.data(), 8) == 0 && protection(pages) == PAGE_READONLY &&
              protection(pages + info.dwPageSize) == PAGE_READONLY,
          "across two pages, both read-only again: " + problem);
    // Reserved, not committed: refused with the reason.
    auto* reserved = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, info.dwPageSize, MEM_RESERVE, PAGE_NOACCESS));
    check(writer.write(reserved, elevens.data(), eights.data(), 8, false, found.data(), problem) == Result::failed &&
              problem.find("VirtualProtect failed, Windows error ") == 0,
          "a page that cannot be made writable: " + problem);

    // Code with a busy thread running it.
    auto* spin = reinterpret_cast<std::uint8_t*>(&game_spin);
    const std::array<std::uint8_t, 13> spin_bytes{0x81, 0x01, 0x2A, 0x2A, 0x2A, 0x00, 0x81,
                                                  0x41, 0x04, 0x5D, 0x5D, 0x5D, 0x00};
    if (std::memcmp(spin, spin_bytes.data(), spin_bytes.size()) != 0 || spin[13] != 0xC3) {
        std::cerr << "FAIL: game_spin compiled differently than this test expects\n";
        return 1;
    }
    const auto code_protection = protection(spin);
    std::array<std::uint8_t, 13> nops{};
    nops.fill(0x90);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> calls{0};
    int counters[2] = {0, 0};
    std::thread busy([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            g_spin(counters);
            calls.fetch_add(1, std::memory_order_relaxed);
        }
    });
    while (calls.load() < 1000) Sleep(0);
    int switched = 0;
    for (int round = 0; round < 200; ++round) {
        const bool on = round % 2 == 0;
        if (writer.write(spin, on ? spin_bytes.data() : nops.data(), on ? nops.data() : spin_bytes.data(), 13, true,
                         found.data(), problem) == Result::written) {
            ++switched;
        } else {
            std::cerr << "round " << round << ": " << problem << '\n';
        }
    }
    check(switched == 200, "200 code writes under a busy thread: " + std::to_string(switched));
    // The game's bytes are back after an even number of rounds. NOPs: the
    // thread keeps calling, the adds stop.
    const volatile int* counted = &counters[1];
    writer.write(spin, spin_bytes.data(), nops.data(), 13, true, found.data(), problem);
    Sleep(5);
    const int frozen = *counted;
    const auto calls_then = calls.load();
    Sleep(20);
    check(*counted == frozen && calls.load() > calls_then, "the NOPed adds no longer run, the thread does");
    writer.write(spin, nops.data(), spin_bytes.data(), 13, true, found.data(), problem);
    Sleep(20);
    check(*counted != frozen, "the adds run again");
    stop = true;
    busy.join();
    check(protection(spin) == code_protection, "code protection restored");

    // A call sent to a plugin function through a stub.
    auto* site = call_to(reinterpret_cast<void*>(&game_caller), reinterpret_cast<void*>(&game_callee));
    check(site != nullptr, "game_caller calls game_callee");
    const auto* exe = GetModuleHandleW(nullptr);
    al::plugins::Scanner scanner(exe, L"");
    al::image::Claims claims;
    patch::Registry registry(
        exe, [&](const al::Pattern& pattern, bool code) {
            return scanner.find(pattern, code ? AL_SCAN_CODE : AL_SCAN_IMAGE).matches;
        },
        claims);
    registry.set_writer(&writer);
    al::graphics::Client client;
    client.name = "alpha";
    std::array<std::uint8_t, 5> call_bytes{};
    if (site) std::memcpy(call_bytes.data(), site, 5);
    const int before = g_caller(4);
    al_patch_t call{};
    call.size = sizeof(call);
    call.kind = AL_PATCH_CALL;
    call.name = "caller: callee";
    call.length = 5;
    call.address = site;
    call.expected = call_bytes.data();
    call.function = reinterpret_cast<void*>(&plugin_callee);
    call.original = &g_callee_original;
    const auto handle = site ? registry.apply(client, call, problem) : 0;
    check(handle && g_caller(4) == (game_callee(4 + 0x2222) + 2000000) * 3 + 0x55,
          "the plugin's function runs and calls the original: " + problem);
    if (handle) {
        std::int32_t rel = 0;
        std::memcpy(&rel, site + 1, sizeof(rel));
        const auto* stub = site + 5 + rel;
        check(stub != reinterpret_cast<const std::uint8_t*>(&plugin_callee) && protection(stub) == PAGE_EXECUTE_READ,
              "a stub, read-and-execute only");
        check(registry.set_enabled(client, handle, false).empty() && g_caller(4) == before &&
                  std::memcmp(site, call_bytes.data(), 5) == 0,
              "switched off");
        check(registry.set_enabled(client, handle, true).empty() && g_caller(4) != before &&
                  std::memcmp(site + 1, &rel, 4) == 0,
              "switched on, the same stub");
        void* second = nullptr;
        check(writer.stub(site, reinterpret_cast<void*>(&plugin_callee), &second).empty() &&
                  second == stub + info.dwPageSize,
              "the next stub takes the next page of the same reservation");
    }

    if (failures) {
        std::cerr << failures << " native writer check(s) failed\n";
        return 1;
    }
    std::cout << "the native writer swaps data atomically, writes code under a busy thread and places stubs\n";
    return 0;
}
