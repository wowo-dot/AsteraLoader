// API 1.1 through the plugin host: published interfaces, built-in services,
// dependencies, data folders, timing and a plugin built for API 1.0.
// argv[1] holds the built fixtures, argv[2] is a work folder.
#include "work_dir.hpp"
#include "common/log_file.hpp"
#include "common/sha256.hpp"
#include "common/windows.hpp"
#include "core/plugins/host.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool condition, const std::string& label) {
    if (!condition) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// For "consumer" to find through al.scanner in this exe's image.
std::uint8_t g_planted[12] = {0xC1, 0x05, 0xE0, 0x01, 0xAB, 0xCD, 0xEF, 0x77, 0x31, 0x5E, 0x22, 0x10};

const al::plugins::PluginReport* find(const std::vector<al::plugins::PluginReport>& report, const wchar_t* name) {
    for (const auto& line : report) {
        if (line.name == name) return &line;
    }
    return nullptr;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const fs::path fixtures = argv[1];
    const auto work = al_test::fresh_work_dir(argv[2]);
    const auto root = work / L"plugins";
    for (const auto name : {L"provider", L"consumer", L"needs_missing", L"needs_newer", L"failing", L"needs_failing",
                            L"legacy10", L"slow"}) {
        fs::create_directories(root / name);
        fs::copy_file(fixtures / (std::wstring(name) + L".dll"), root / name / (std::wstring(name) + L".dll"));
    }
    fs::create_directories(work / L"game");
    std::ofstream(work / L"game" / L"graphics_option.ini") << "DirectX12Enable=Off\r\n";
    const auto record = work / L"record.txt";
    SetEnvironmentVariableW(L"AL_TEST_PLUGIN_RECORD", record.c_str());
    SetEnvironmentVariableA("AL_TEST_SCAN_PATTERN", "C1 05 E0 01 AB CD EF ?? 31 5E 22 10");
    const auto hash = al::sha256_file(al::module_path());

    al::plugins::HostOptions options;
    options.plugins_root = root.wstring();
    // As in the game: AL's log folder comes from a resolved path with the
    // long-path prefix; plugins must still get plain paths.
    // (Windows takes such paths literally, so the separators must be backslashes.)
    options.logs_folder = L"\\\\?\\" + (work / L"logs").make_preferred().wstring();
    options.exe_sha256 = hash;
    options.game_folder = (work / L"game").wstring();
    al::plugins::Host host;
    host.load_all(options);
    const auto report = host.report();

    char planted[32]{};
    std::snprintf(planted, sizeof(planted), "%p", static_cast<const void*>(g_planted));
    const auto expected = "failing init\nlegacy10 init\n"
                          "provider published\nprovider taken\nprovider foreign refused\nprovider case refused\n"
                          "consumer math 5\nconsumer mismatches null\n"
                          "consumer game " + al::to_utf8(hash) + " graphics 1\n"
                          "consumer scan 1 " + planted + "\nconsumer invalid refused\nconsumer data written\n"
                          "slow init\n";
    const auto seen = read_file(record);
    check(seen == expected, "start order and what the plugins saw");
    if (seen != expected) std::cerr << "--- seen\n" << seen << "--- expected\n" << expected;

    const auto status = [&](const wchar_t* name, al_status_t wanted, bool initialised, const std::string& reason) {
        const auto line = find(report, name);
        const bool ok = line && line->status == wanted && line->initialised == initialised &&
                        line->reason.find(reason) != std::string::npos;
        check(ok, al::to_utf8(name) + " status");
        if (line && !ok) std::cerr << "  reason: " << line->reason << '\n';
    };
    status(L"provider", AL_STATUS_ACTIVE, true, "");
    status(L"consumer", AL_STATUS_ACTIVE, true, "");
    status(L"legacy10", AL_STATUS_ACTIVE, true, "");  // API 1.0 struct, its dependency list never read
    status(L"needs_missing", AL_STATUS_DISABLED, false, "it needs ghost, which is not installed");
    status(L"needs_newer", AL_STATUS_DISABLED, false, "it needs provider 9.0 or newer; 0.3.1 is installed");
    status(L"needs_failing", AL_STATUS_DISABLED, false, "it needs failing, which did not start");
    check(!GetModuleHandleW((root / L"needs_failing" / L"needs_failing.dll").c_str()), "refused dependant unloaded");

    const auto slow = find(report, L"slow");
    check(slow && slow->init_us >= 25000, "init time measured");
    for (const auto& line : report) {
        if (line.initialised) check(line.load_us > 0, al::to_utf8(line.name) + " load time measured");
    }

    check(fs::exists(work / L"data" / L"consumer" / L"consumer.txt"), "data folder beside the logs");
    check(!fs::exists(work / L"data" / L"legacy10"), "no data folder for a plugin built for API 1.0");
    const auto cache = read_file(work / L"cache" / (L"scan." + hash + L".txt"));
    check(cache.rfind("1 c105e001abcdef00315e2210/ffffffffffffff00ffffffff ", 0) == 0, "scan cached per exe hash");
    const auto consumer_log = read_file(work / L"logs" / L"plugins" / L"consumer.log");
    check(consumer_log.find(" in image: 1 match(es), searched in ") != std::string::npos, "scan logged");
    const auto provider_log = read_file(work / L"logs" / L"plugins" / L"provider.log");
    check(provider_log.find(" INFO published interface provider.math 1.2") != std::string::npos &&
          provider_log.find("cannot publish interface other.math 1.2: the name must start with \"provider.\"") !=
              std::string::npos, "publishing logged");

    if (failures) {
        std::cerr << failures << " service check(s) failed\n";
        return 1;
    }
    std::cout << "Interfaces, services, dependencies, data folders and timing checked through the host\n";
    return 0;
}
