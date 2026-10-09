#include "core/frame/layer.hpp"
#include "core/frame/clock.hpp"
#include "core/plugins/host.hpp"
#include <windows.h>
#include <bit>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <atomic>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <thread>

std::atomic<std::uint64_t> allocations{0};
void* operator new(std::size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (auto memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
__declspec(noinline) std::uintptr_t update(void*, void*, void*, void*) { return GetTickCount64() ^ 0x123456789abcull; }
__declspec(noinline) void exit_site(void*) { SetLastError(0x5a); }
std::string signature(void* target) {
    std::string text;
    for (unsigned i = 0; i < 16; ++i) {
        char byte[4]{}; std::snprintf(byte, sizeof(byte), "%02X", static_cast<const unsigned char*>(target)[i]);
        if (i) text += ' '; text += byte;
    }
    return text;
}
struct Installer : al::game_hooks::Installer {
    void* const* head{};
    unsigned count{}, fail{};
    std::string create(void* target, void* const* entry, void** original) override {
        if (++count == fail) return "fixture relocation failure";
        if (target == std::bit_cast<void*>(&update)) head = entry;
        *original = target; return {};
    }
    std::string enable(void*) override { return {}; }
};
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 3) return 2;
        const std::wstring mode(argv[1]);
        if (mode == L"identity") {
            al_game_t identity{};
            identity.game_version = "421810";
            require(!al::frame::game_sites(&identity, GetModuleHandleW(nullptr)).problem.empty(), "missing SHA accepted");
            identity.exe_sha256 = "other";
            require(!al::frame::game_sites(&identity, GetModuleHandleW(nullptr)).problem.empty(), "unknown SHA accepted");
            identity.exe_sha256 = "c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea";
            identity.game_version = "421809";
            require(!al::frame::game_sites(&identity, GetModuleHandleW(nullptr)).problem.empty(), "other version accepted");
            identity.game_version = "421810";
            require(!al::frame::game_sites(&identity, GetModuleHandleW(nullptr)).problem.empty(), "truncated fake exe accepted as real sites");
            std::cout << "Frame site identity and image bounds refused unsafe addresses\n"; return 0;
        }
        namespace fs = std::filesystem;
        const auto work = fs::path(argv[2]) / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directories(work / L"plugins");
        // Like the production host, all callback/continuation storage stays
        // alive until process exit. The stand-in installer never patches code.
        auto* host = new al::plugins::Host;
        host->load_all({(work / L"plugins").wstring(), (work / L"logs").wstring(), L""});
        host->dispatch(AL_PHASE_PRE_MAIN, nullptr);
        unsigned warnings = 0;
        auto* layer = new al::frame::Layer(*host, {}, [&](std::string_view) { ++warnings; });
        unsigned char flag = 0;
        const void* object = &flag;
        al::frame::Sites sites{{std::bit_cast<void*>(&update), signature(std::bit_cast<void*>(&update)), 0},
            {std::bit_cast<void*>(&exit_site), signature(std::bit_cast<void*>(&exit_site)), 0}, &object, 0, {}};
        auto* installer = new Installer;
        if (mode == L"bad_signature") sites.exiting.pattern = "00 00 00 00 00 00";
        if (mode == L"first_failure") installer->fail = 1;
        if (mode == L"second_failure") installer->fail = 2;
        layer->prepare(std::move(sites), true);
        host->game_hooks().set_installer(installer);
        layer->installed();
        if (mode == L"cost") {
            require(host->frame_clock().state() == AL_FRAME_WAITING && installer->head && warnings == 0, "benchmark hooks not ready");
            std::uint64_t made = 0;
            double baseline_ns = 0, layer_ns = 0;
            std::thread worker([&] {
                const auto wrapped = std::bit_cast<decltype(&update)>(*installer->head);
                for (unsigned i = 0; i < 100; ++i) wrapped(nullptr, nullptr, nullptr, nullptr);
                LARGE_INTEGER frequency{}, a{}, b{}, c{}; QueryPerformanceFrequency(&frequency);
                const auto before = allocations.load();
                QueryPerformanceCounter(&a);
                for (unsigned i = 0; i < 100000; ++i) wrapped(nullptr, nullptr, nullptr, nullptr);
                QueryPerformanceCounter(&b);
                for (unsigned i = 0; i < 100000; ++i) update(nullptr, nullptr, nullptr, nullptr);
                QueryPerformanceCounter(&c);
                made = allocations.load() - before;
                layer_ns = static_cast<double>(b.QuadPart - a.QuadPart) * 1000000000.0 / static_cast<double>(frequency.QuadPart) / 100000.0;
                baseline_ns = static_cast<double>(c.QuadPart - b.QuadPart) * 1000000000.0 / static_cast<double>(frequency.QuadPart) / 100000.0;
            });
            worker.join();
            require(made == 0, "steady frame layer allocated");
            std::cout << "synthetic_frame_thread empty_layer_ns=" << layer_ns << " original_ns=" << baseline_ns
                << " allocations=" << made << " (excludes MinHook entry; not an MHW measurement)\n";
            return 0;
        }
        require(host->frame_clock().state() == AL_FRAME_UNAVAILABLE && warnings == 1, "failed pair not unavailable/visible exactly once");
        al::frame::Client client;
        require(!host->frame_clock().post(client, [](al_plugin_t*, void*) {}, nullptr).empty(), "unavailable queue accepted a task");
        for (const auto& line : host->game_hooks().summary()) {
            require(line.find("layers=AsteraLoader") == std::string::npos || line.find("(off)") != std::string::npos ||
                line.find("state=failed") != std::string::npos, "partial pair still had an enabled loader layer");
        }
        if (installer->head) require(*installer->head == std::bit_cast<void*>(&update), "partly installed frame layer was not bypassed");
        std::cout << "Frame pair failure is visible, unavailable and leaves the game's original reachable\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
