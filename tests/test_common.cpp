#include "common/launch_plan.hpp"
#include "common/log_file.hpp"
#include "common/paths.hpp"
#include "common/sha256.hpp"
#include "common/startup.hpp"

#include <windows.h>
#include <shellapi.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char* label) {
    if (!condition) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
}
std::wstring hash(const std::string& input) {
    cmfm::Sha256 sha;
    sha.update(input.data(), input.size());
    return cmfm::Sha256::hex(sha.finish());
}

void quoting() {
    const std::vector<std::wstring> cases{L"", L"plain", L"with spaces", L"a\tb", L"a\"b",
        L"C:\\a b\\", L"\\", L"\\\\\"tail", L"\"", L"日本語", L"line\nend"};
    for (const auto& argument : cases) {
        const auto command = L"test.exe " + cmfm::quote_argument(argument);
        int count = 0;
        auto parsed = CommandLineToArgvW(command.c_str(), &count);
        check(parsed && count == 2 && parsed[1] == argument, "argument round trip");
        if (parsed) LocalFree(parsed);
    }
    // Exhaustively exercise short combinations of separators, quotes and slash runs.
    constexpr std::array<wchar_t, 4> alphabet{L'a', L' ', L'\\', L'"'};
    for (unsigned value = 0; value < 4096; ++value) {
        unsigned encoded = value;
        std::wstring input;
        for (int i = 0; i < 6; ++i) { input += alphabet[encoded % 4]; encoded /= 4; }
        const auto command = L"test.exe " + cmfm::quote_argument(input);
        int count = 0;
        auto parsed = CommandLineToArgvW(command.c_str(), &count);
        check(parsed && count == 2 && parsed[1] == input, "exhaustive argument round trip");
        if (parsed) LocalFree(parsed);
    }
}

void planning() {
    const std::wstring game = L"C:\\game dir\\MonsterHunterWorld.exe";
    std::vector<std::wstring> present{game, L"C:\\launcher\\MonsterHunterWorld.exe"};
    const auto exists = [&present](const std::wstring& path) {
        for (const auto& file : present) if (cmfm::iequals(file, path)) return true;
        return false;
    };
    cmfm::LaunchPlan plan;
    std::wstring error = L"stale";
    check(cmfm::plan_launch({game, L"a b", L"", L"tail\\"}, L"C:\\launcher", L"C:\\game dir",
                            exists, plan, error), "explicit Steam game path");
    check(plan.exe == game && plan.source == L"argument" && error.empty(), "argument has priority");
    check(plan.working_dir == L"C:\\game dir", "game working directory");
    int count = 0;
    auto argv = CommandLineToArgvW(plan.command_line.c_str(), &count);
    check(argv && count == 4 && std::wstring(argv[1]) == L"a b" && std::wstring(argv[2]).empty() &&
          std::wstring(argv[3]) == L"tail\\", "launch passes arguments unchanged");
    if (argv) LocalFree(argv);
    check(cmfm::plan_launch({L"-dx12"}, L"C:\\launcher", L"C:\\game dir", exists, plan, error) &&
          plan.source == L"launcher folder", "launcher directory precedes cwd");
    present = {game};
    check(cmfm::plan_launch({}, L"C:\\launcher", L"C:\\game dir", exists, plan, error) &&
          plan.source == L"current folder", "cwd fallback");
    check(cmfm::plan_launch({L"monsterhunterworld.EXE"}, L"C:\\launcher", L"C:\\game dir",
                            exists, plan, error) && plan.source == L"argument", "relative case-insensitive path");
    check(!cmfm::plan_launch({L"C:\\missing\\MonsterHunterWorld.exe"}, L"C:\\launcher",
                             L"C:\\game dir", exists, plan, error), "missing explicit exe never falls back");
    present.clear();
    check(!cmfm::plan_launch({}, L"C:\\launcher", L"C:\\game dir", exists, plan, error) &&
          !error.empty(), "missing game has an explanation");
    present = {L"C:\\MonsterHunterWorld.exe"};
    check(cmfm::plan_launch({}, L"C:\\", L"C:\\game dir", exists, plan, error) &&
          plan.working_dir == L"C:\\", "drive-root working directory");
    check(cmfm::plan_launch({L"\\MonsterHunterWorld.exe"}, L"C:\\launcher", L"C:\\game dir",
                            exists, plan, error) && plan.exe == present[0], "root-relative game path");
    check(!cmfm::plan_launch({L"C:MonsterHunterWorld.exe"}, L"C:\\launcher", L"C:\\game dir",
                             exists, plan, error), "ambiguous drive-relative path rejected");
}

void paths() {
    check(cmfm::iequals(L"CMFM.DLL", L"cmfm.dll"), "case-insensitive filename");
    check(cmfm::file_name_of(L"C:/mods/cmfm.dll") == L"cmfm.dll", "filename");
    check(cmfm::directory_of(L"C:\\x") == L"C:\\", "drive root kept absolute");
    check(cmfm::directory_of(L"\\\\?\\C:\\x") == L"\\\\?\\C:\\", "extended drive root kept absolute");
    check(cmfm::directory_of(L"/x") == L"/", "slash root preserved");
    check(cmfm::directory_of(L"x").empty(), "filename has no directory");
    check(cmfm::join_path(L"C:\\", L"x") == L"C:\\x", "join at root");
    check(cmfm::is_absolute(L"C:\\x") && cmfm::is_absolute(L"\\\\server\\share\\x") &&
          !cmfm::is_absolute(L"C:x") && !cmfm::is_absolute(L"mods\\x"), "absolute paths");
    check(cmfm::strip_extended_prefix(L"\\\\?\\C:\\x") == L"C:\\x", "extended drive path");
    check(cmfm::strip_extended_prefix(L"\\\\?\\UNC\\server\\share\\x") == L"\\\\server\\share\\x",
          "extended UNC path");
    check(cmfm::redact_profile(L"C:\\Users\\person\\mods", L"c:\\users\\PERSON\\") ==
          L"%USERPROFILE%\\mods", "profile redaction");
    check(cmfm::redact_profile(L"C:\\Users\\person2\\mods", L"C:\\Users\\person") ==
          L"C:\\Users\\person2\\mods", "profile boundary");
    check(cmfm::redact_profile(L"\\\\?\\C:\\Users\\person\\mods", L"C:/Users/person") ==
          L"%USERPROFILE%\\mods", "extended real path redaction");
}

void hashes() {
    check(hash("") == L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA empty");
    check(hash("abc") == L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA abc");
    check(hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          L"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "SHA padding boundary");
    check(hash(std::string(1000000, 'a')) ==
          L"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA million a");
    for (const auto size : {55, 56, 63, 64, 65, 119, 120, 128, 257}) {
        const std::string input(static_cast<std::size_t>(size), 'a');
        cmfm::Sha256 chunked;
        for (const char byte : input) chunked.update(&byte, 1);
        check(cmfm::Sha256::hex(chunked.finish()) == hash(input), "SHA chunking across block boundaries");
    }
}
}  // namespace

int main() {
    quoting(); planning(); paths(); hashes();
    check(cmfm::to_utf8(L"日本語") == "日本語", "UTF-8 log conversion");
    check(sizeof(cmfm::StartupRequest) == 16, "startup ABI size");
    if (!failures) std::cout << "All common tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
