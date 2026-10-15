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
    al::Sha256 sha;
    sha.update(input.data(), input.size());
    return al::Sha256::hex(sha.finish());
}

void quoting() {
    const std::vector<std::wstring> cases{L"", L"plain", L"with spaces", L"a\tb", L"a\"b",
        L"C:\\a b\\", L"\\", L"\\\\\"tail", L"\"", L"日本語", L"line\nend"};
    for (const auto& argument : cases) {
        const auto command = L"test.exe " + al::quote_argument(argument);
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
        const auto command = L"test.exe " + al::quote_argument(input);
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
        for (const auto& file : present) if (al::iequals(file, path)) return true;
        return false;
    };
    al::LaunchPlan plan;
    std::wstring error = L"stale";
    check(al::plan_launch({game, L"a b", L"", L"tail\\"}, L"C:\\launcher", L"C:\\game dir",
                            exists, plan, error), "explicit Steam game path");
    check(plan.exe == game && plan.source == L"argument" && error.empty(), "argument has priority");
    check(plan.working_dir == L"C:\\game dir", "game working directory");
    int count = 0;
    auto argv = CommandLineToArgvW(plan.command_line.c_str(), &count);
    check(argv && count == 4 && std::wstring(argv[1]) == L"a b" && std::wstring(argv[2]).empty() &&
          std::wstring(argv[3]) == L"tail\\", "launch passes arguments unchanged");
    if (argv) LocalFree(argv);
    check(al::plan_launch({L"-dx12"}, L"C:\\launcher", L"C:\\game dir", exists, plan, error) &&
          plan.source == L"launcher folder", "launcher directory precedes cwd");
    present = {game};
    check(al::plan_launch({}, L"C:\\launcher", L"C:\\game dir", exists, plan, error) &&
          plan.source == L"current folder", "cwd fallback");
    check(al::plan_launch({L"monsterhunterworld.EXE"}, L"C:\\launcher", L"C:\\game dir",
                            exists, plan, error) && plan.source == L"argument", "relative case-insensitive path");
    check(!al::plan_launch({L"C:\\missing\\MonsterHunterWorld.exe"}, L"C:\\launcher",
                             L"C:\\game dir", exists, plan, error), "missing explicit exe never falls back");
    present.clear();
    check(!al::plan_launch({}, L"C:\\launcher", L"C:\\game dir", exists, plan, error) &&
          !error.empty(), "missing game has an explanation");
    present = {L"C:\\MonsterHunterWorld.exe"};
    check(al::plan_launch({}, L"C:\\", L"C:\\game dir", exists, plan, error) &&
          plan.working_dir == L"C:\\", "drive-root working directory");
    check(al::plan_launch({L"\\MonsterHunterWorld.exe"}, L"C:\\launcher", L"C:\\game dir",
                            exists, plan, error) && plan.exe == present[0], "root-relative game path");
    check(!al::plan_launch({L"C:MonsterHunterWorld.exe"}, L"C:\\launcher", L"C:\\game dir",
                             exists, plan, error), "ambiguous drive-relative path rejected");
}

void steam() {
    int asked = 0;
    const auto running = [&asked] { ++asked; return true; };
    const auto stopped = [&asked] { ++asked; return false; };
    al::SteamPlan plan;
    std::wstring error = L"stale";
    check(al::plan_steam(L"582010", stopped, plan, error) && plan.app_id == L"582010" &&
          plan.source == L"inherited" && error.empty() && asked == 0,
          "an inherited app id is kept and Steam is not checked");
    check(al::plan_steam(L"4242", running, plan, error) && plan.app_id == L"4242",
          "an inherited app id is never replaced");
    check(al::plan_steam(L"", running, plan, error) && plan.app_id == L"582010" &&
          plan.source == L"launcher" && asked == 1, "started directly: MHW's app id");
    check(!al::plan_steam(L"", stopped, plan, error) && asked == 2 && plan.app_id.empty() &&
          error.find(L"Steam is not running") != std::wstring::npos,
          "started directly without Steam: an explanation, no app id");
}

void paths() {
    check(al::iequals(L"ASTERALOADER.DLL", L"AsteraLoader.dll"), "case-insensitive filename");
    check(al::file_name_of(L"C:/mods/AsteraLoader.dll") == L"AsteraLoader.dll", "filename");
    check(al::directory_of(L"C:\\x") == L"C:\\", "drive root kept absolute");
    check(al::directory_of(L"\\\\?\\C:\\x") == L"\\\\?\\C:\\", "extended drive root kept absolute");
    check(al::directory_of(L"/x") == L"/", "slash root preserved");
    check(al::directory_of(L"x").empty(), "filename has no directory");
    check(al::join_path(L"C:\\", L"x") == L"C:\\x", "join at root");
    check(al::is_absolute(L"C:\\x") && al::is_absolute(L"\\\\server\\share\\x") &&
          !al::is_absolute(L"C:x") && !al::is_absolute(L"mods\\x"), "absolute paths");
    check(al::strip_extended_prefix(L"\\\\?\\C:\\x") == L"C:\\x", "extended drive path");
    check(al::strip_extended_prefix(L"\\\\?\\UNC\\server\\share\\x") == L"\\\\server\\share\\x",
          "extended UNC path");
    check(al::redact_profile(L"C:\\Users\\person\\mods", L"c:\\users\\PERSON\\") ==
          L"%USERPROFILE%\\mods", "profile redaction");
    check(al::redact_profile(L"C:\\Users\\person2\\mods", L"C:\\Users\\person") ==
          L"C:\\Users\\person2\\mods", "profile boundary");
    check(al::redact_profile(L"\\\\?\\C:\\Users\\person\\mods", L"C:/Users/person") ==
          L"%USERPROFILE%\\mods", "extended real path redaction");
    check(al::redact_profile(L"\\Device\\HarddiskVolume3\\Users\\person\\mods",
                             L"\\Device\\HarddiskVolume3\\Users\\person") == L"%USERPROFILE%\\mods",
          "NT name of a mapped file (Windows)");
    check(al::redact_profile(L"\\??\\C:\\users\\person\\mods", L"\\??\\C:\\Users\\person") ==
          L"%USERPROFILE%\\mods", "NT name of a mapped file (Wine)");
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
        al::Sha256 chunked;
        for (const char byte : input) chunked.update(&byte, 1);
        check(al::Sha256::hex(chunked.finish()) == hash(input), "SHA chunking across block boundaries");
    }
}
}  // namespace

int main() {
    quoting(); planning(); steam(); paths(); hashes();
    check(al::to_utf8(L"日本語") == "日本語", "UTF-8 log conversion");
    check(sizeof(al::StartupRequest) == 16, "startup ABI size");
    if (!failures) std::cout << "All common tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
