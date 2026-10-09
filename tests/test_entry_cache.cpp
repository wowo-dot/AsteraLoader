#include "core/startup/entry_cache.hpp"
#include "common/windows.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    namespace fs = std::filesystem;
    using namespace al::startup;
    // Windows reuses PIDs; an earlier invocation's cache is intentionally
    // retained for diagnostics. Reserve a fresh directory for this test.
    const auto root = fs::path(argv[1]);
    fs::create_directories(root);
    fs::path work;
    for (unsigned attempt = 0;; ++attempt) {
        work = root / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(attempt));
        if (fs::create_directory(work)) break;
    }
    fs::create_directories(work / L"logs");
    const std::wstring hash(64, L'a'), other_hash(64, L'b');
    const auto file = crt_cache_file((work / L"logs").wstring(), hash);
    if (file != (work / L"cache" / (L"scan." + hash + L".txt")).wstring() ||
        file == crt_cache_file((work / L"logs").wstring(), other_hash) ||
        !crt_cache_file((work / L"logs").wstring(), L"../bad").empty()) return 2;
    if (read_crt_cache(file) || !append_crt_cache(file, 64) || read_crt_cache(file) != 64) {
        std::cerr << "Fresh cache append/read failed\n"; return 3;
    }
    // Share with the plugin scanner; malformed/new-format/incomplete records
    // cannot erase the last valid entry, and each matcher version is separate.
    std::ofstream(file, std::ios::app | std::ios::binary) << "0 pattern/key 123\nstartup crt-entry-2 99\n"
        "startup crt-entry-1 -1\nstartup crt-entry-1 0\nstartup crt-entry-1 fffffffff\n"
        "startup crt-entry-1 40,50\nstartup crt-entry-1 80\r\nstartup crt-entry-1 ff";
    if (read_crt_cache(file) != 128) return 4;
    if (!append_crt_cache(file, 256) || read_crt_cache(file) != 256) return 7;
    const auto text = std::string("startup crt-entry-1 20\nstartup crt-entry-1 xx\nstartup crt-entry-1 30\n");
    if (cached_crt_rva(text) != 48 || cached_crt_rva("startup crt-entry-0 40\n") ||
        cached_crt_rva("startup crt-entry-1 40")) return 5;
    // A directory cannot become a cache file; missing hash never writes.
    if (append_crt_cache(work.wstring(), 64) || append_crt_cache(L"", 64) || append_crt_cache(file, 0)) return 6;
    std::cout << "CRT cache parsing, hash/version separation, shared-file records and I/O failure checks passed\n";
    return 0;
}
