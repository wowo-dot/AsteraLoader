// CMFM's built-in services on their own: "cmfm.game" and "cmfm.scanner".
// argv[1] is a work folder.
#include "common/pattern.hpp"
#include "common/sha256.hpp"
#include "common/windows.hpp"
#include "core/plugins/services.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool condition, const std::string& label) {
    if (!condition) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
}

// Planted in this exe's data twice; the scanner's image scope must find both
// and its code scope neither.
std::uint8_t g_planted[48] = {0x13, 0x37, 0xC0, 0xDE, 0x5A, 0x17, 0xF0, 0x0D, 0x99, 0x77, 0, 0, 0, 0, 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                              0x13, 0x37, 0xC0, 0xDE, 0x5A, 0x17, 0xF0, 0x0D, 0x99, 0x77, 0, 0, 0, 0, 0, 0};
constexpr char kPlanted[] = "13 37 C0 DE 5A ?? F0 0D 99 77";

// MHW's version code in miniature, in an executable section of this exe: an
// instruction whose RIP-relative operand points at `text`, then Stracker's
// pattern 0x18 bytes in. cmfm.game must find "svc-code" through it.
#pragma section(".cmfmx", read, execute)
struct VersionCode {
    std::uint8_t instruction[0x18];  // 48 8B 05 <disp32>: text sits at 7 + 0x21 = 0x28
    std::uint8_t pattern[9];
    std::uint8_t pad[7];
    const char* text;
};
static_assert(offsetof(VersionCode, text) == 0x28);
__declspec(allocate(".cmfmx")) const VersionCode g_version_code = {
    {0x48, 0x8B, 0x05, 0x21, 0, 0, 0}, {0x48, 0x8D, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x42, 0xF5}, {}, "svc-code"};

// Code with an unusual constant, so its bytes occur once in .text.
__declspec(noinline) int marker() {
    return 0x5A17C0DE;
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

void graphics() {
    using cmfm::plugins::graphics_from_ini;
    check(graphics_from_ini("[Graphics]\nDirectX12Enable=On\n") == CMFM_GRAPHICS_DX12, "On");
    check(graphics_from_ini("directx12enable = off\r\n") == CMFM_GRAPHICS_DX11, "off, spaced, lower case");
    check(graphics_from_ini("ScreenMode=Borderless\n") == CMFM_GRAPHICS_UNKNOWN, "missing key");
    check(graphics_from_ini("DirectX12Enable=Maybe") == CMFM_GRAPHICS_UNKNOWN, "unknown value");
}

void game(const fs::path& work) {
    const auto folder = work / L"game";
    fs::create_directories(folder);
    std::ofstream(folder / L"graphics_option.ini") << "[Graphics]\r\nDirectX12Enable=On\r\n";
    const auto hash = cmfm::sha256_file(cmfm::module_path());
    cmfm::plugins::GameService service(GetModuleHandleW(nullptr), hash, folder.wstring());
    const auto* table = service.table();
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const std::uint8_t*>(dos) + dos->e_lfanew);
    check(table->size == sizeof(cmfm_game_t) && table->version == CMFM_GAME_VERSION, "game table header");
    check(table->exe_sha256 && std::string(table->exe_sha256).size() == 64, "exe hash");
    check(table->game_version && std::string(table->game_version) == "svc-code",
          "version through Stracker's whole-image search (no scanner)");
    check(table->exe_path && cmfm::module_path() == table->exe_path, "exe path");
    check(table->game_folder && folder.wstring() == table->game_folder, "game folder");
    check(table->pe_timestamp == nt->FileHeader.TimeDateStamp, "PE time stamp");
    check(table->graphics == CMFM_GRAPHICS_DX12, "graphics from the ini");
    check(service.table() == table, "filled once");

    cmfm::plugins::GameService unknown(GetModuleHandleW(nullptr), L"", (work / L"nowhere").wstring());
    check(!unknown.table()->exe_sha256 && unknown.table()->graphics == CMFM_GRAPHICS_UNKNOWN, "no hash, no ini");

    // With the scanner: found in code sections, and the match is cached, so a
    // later launch does not search at all.
    const auto cache = work / L"cache" / L"scan.game.txt";
    cmfm::plugins::Scanner scanner(GetModuleHandleW(nullptr), cache.wstring());
    cmfm::plugins::GameService scanned(GetModuleHandleW(nullptr), hash, folder.wstring(), &scanner);
    check(scanned.table()->game_version && std::string(scanned.table()->game_version) == "svc-code",
          "version through the code-section scan");
    char line[96]{};
    std::snprintf(line, sizeof(line), "0 488d4c2430488d42f5/f8ffff3ffff8ffffff %zx\n",
                  static_cast<std::size_t>(g_version_code.pattern - reinterpret_cast<const std::uint8_t*>(dos)));
    check(read_file(cache) == line, "version code cached");
}

void scanner(const fs::path& work) {
    const auto cache = work / L"cache" / L"scan.test.txt";
    const auto image = GetModuleHandleW(nullptr);
    const auto planted = *cmfm::parse_pattern(kPlanted);

    cmfm::plugins::Scanner first(image, cache.wstring());
    auto result = first.find(planted, CMFM_SCAN_IMAGE);
    check(result.matches.size() == 2 && result.matches[0] == g_planted && result.matches[1] == g_planted + 32 &&
          !result.cached, "image scope finds both planted copies");
    check(first.find(planted, CMFM_SCAN_CODE).matches.empty(), "code scope does not search data");

    std::uint8_t code[6]{};
    std::memcpy(code, reinterpret_cast<const void*>(&marker), sizeof(code));
    char text[64]{};
    std::snprintf(text, sizeof(text), "%02X %02X %02X %02X %02X %02X", code[0], code[1], code[2], code[3], code[4],
                  code[5]);
    const auto code_result = first.find(*cmfm::parse_pattern(text), CMFM_SCAN_CODE);
    check(code_result.matches.size() == 1 && code_result.matches[0] == reinterpret_cast<const void*>(&marker),
          std::string("code scope finds marker() by ") + text);

    // A second scanner reads the cache instead of searching.
    const auto lines = read_file(cache);
    check(std::count(lines.begin(), lines.end(), '\n') == 2, "two results cached");
    cmfm::plugins::Scanner second(image, cache.wstring());
    result = second.find(planted, CMFM_SCAN_IMAGE);
    check(result.cached && result.matches.size() == 2 && result.matches[0] == g_planted, "cache hit");

    // A cached match that no longer matches is searched again and replaced.
    g_planted[32] = 0x14;
    cmfm::plugins::Scanner third(image, cache.wstring());
    result = third.find(planted, CMFM_SCAN_IMAGE);
    check(!result.cached && result.matches.size() == 1 && result.matches[0] == g_planted, "stale cache searched again");
    std::ofstream(cache, std::ios::app) << "garbage\n0 only-two\n0 key zz\n1 488b/ffff 1,\n";
    cmfm::plugins::Scanner fourth(image, cache.wstring());
    result = fourth.find(planted, CMFM_SCAN_IMAGE);
    check(result.cached && result.matches.size() == 1, "newest entry wins, bad lines ignored");

    // Patterns without a match are not cached.
    const auto before = read_file(cache);
    check(fourth.find(*cmfm::parse_pattern("DE AD BE EF 0B AD F0 0D 13 37"), CMFM_SCAN_IMAGE).matches.empty() &&
          read_file(cache) == before, "no match, nothing cached");

    cmfm::plugins::Scanner uncached(image, L"");
    check(uncached.find(planted, CMFM_SCAN_IMAGE).matches.size() == 1, "works without a cache file");
}

void rip() {
    std::uint8_t instruction[16] = {0x48, 0x8B, 0x05, 0x10, 0, 0, 0};
    check(cmfm::plugins::rip_target(instruction, 3, 7) == instruction + 7 + 0x10, "forward");
    const std::int32_t back = -0x20;
    std::memcpy(instruction + 3, &back, 4);
    check(cmfm::plugins::rip_target(instruction, 3, 7) == instruction + 7 - 0x20, "backward");
    check(!cmfm::plugins::rip_target(nullptr, 3, 7), "null instruction");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const auto work = fs::path(argv[1]) / std::to_wstring(GetCurrentProcessId());
    fs::create_directories(work);
    graphics();
    game(work);
    scanner(work);
    rip();
    if (failures) {
        std::cerr << failures << " service check(s) failed\n";
        return 1;
    }
    std::cout << "Game identity, scanner, cache and RIP-relative checks passed\n";
    return 0;
}
