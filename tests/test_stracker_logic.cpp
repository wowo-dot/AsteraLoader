#include "stracker_logic.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char* label) {
    if (!condition) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
}

using cmfm::stracker::Config;

bool parse(std::string_view text, Config& config, std::vector<std::string>& problems) {
    problems.clear();
    return cmfm::stracker::parse_config(text, config, problems);
}

void config() {
    Config c;
    std::vector<std::string> problems;
    // The user's real file and Stracker's shipped one.
    check(parse(R"({"logfile": true, "logcmd": true, "logLevel": "ERROR", "outputEveryPath": false,
                    "enablePluginLoader": true})", c, problems) &&
          c.logfile && c.logcmd && c.log_level == 3 && c.enable_plugin_loader && problems.empty(), "user config");
    // SPL adds its own object; it is skipped.
    check(parse("\xEF\xBB\xBF{\"SPL\": {\"PreloadDLLs\": [\"dinput8.dll\", \"dxgi.dll\"], \"n\": [1, -2.5e3, null, "
                "{\"x\": \"\\u00e9\\ud83d\\ude00\"}]}, \"logLevel\": \"DEBUG\", \"enablePluginLoader\": false}",
                c, problems) &&
          c.log_level == 0 && !c.enable_plugin_loader && !c.logfile && problems.empty(), "SPL object skipped, BOM");
    check(parse("{}", c, problems) && !c.logfile && !c.logcmd && c.log_level == 3 && c.enable_plugin_loader,
          "defaults: ERROR, plugin loader on");
    check(parse(R"({"logLevel": "INFO", "logLevel": "WARNING"})", c, problems) && c.log_level == 2,
          "last repeated key wins");
    check(parse(R"({"logfile": 1, "logLevel": "VERBOSE", "enablePluginLoader": "yes"})", c, problems) &&
          !c.logfile && c.log_level == 3 && c.enable_plugin_loader && problems.size() == 3,
          "wrong types and unknown level keep defaults, with problems");

    for (const char* bad : {"", "[]", "\"text\"", "{", "{\"a\": }", "{\"a\": tru}", "{\"a\": 01}", "{\"a\": 1.}",
                            "{\"a\": \"\\x\"}", "{\"a\": \"\\ud800\"}", "{'a': 1}", "{\"a\": 1,}",
                            "// comment\n{}", "{\"a\": [1 2]}", "{\"a\": \"line\nbreak\"}"}) {
        c.logfile = true;
        check(!parse(bad, c, problems) && !c.logfile, bad);
    }
    std::string deep(100, '[');
    check(!parse("{\"a\": " + deep + "}", c, problems), "nesting limit");
    check(parse("{\"a\": 1} trailing", c, problems), "content after the object is ignored, like nlohmann's >>");
}

void patterns() {
    const auto pattern = cmfm::stracker::parse_bits(cmfm::stracker::kVersionPattern);
    check(pattern.bytes.size() == 9 && pattern.mask.size() == 9, "version pattern is 9 bytes");
    check(pattern.bytes[0] == 0x48 && pattern.mask[0] == 0xF8 && pattern.bytes[1] == 0x8D && pattern.mask[1] == 0xFF &&
          pattern.bytes[3] == 0x24 && pattern.mask[3] == 0x3F && pattern.bytes[8] == 0xF5, "bit notation");

    // Matches at 3 (REX.W variant 0x4F) and 40 (0x48), a near miss at 20.
    const std::uint8_t match_a[] = {0x4F, 0x8D, 0x4C, 0xE4, 0x30, 0x4A, 0x8D, 0x42, 0xF5};
    const std::uint8_t match_b[] = {0x48, 0x8D, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x42, 0xF5};
    std::vector<std::uint8_t> data(64, 0x8D);
    std::memcpy(data.data() + 3, match_a, sizeof(match_a));
    std::memcpy(data.data() + 20, match_b, sizeof(match_b));
    data[20 + 4] = 0x31;
    std::memcpy(data.data() + 40, match_b, sizeof(match_b));
    std::vector<std::size_t> found;
    cmfm::stracker::find_all(data, pattern, 1000, found, 10);
    check(found == std::vector<std::size_t>{1003, 1040}, "masked matches with base offset");
    found.clear();
    cmfm::stracker::find_all(data, pattern, 0, found, 1);
    check(found == std::vector<std::size_t>{3}, "match limit");
    found.clear();
    cmfm::stracker::find_all({data.data() + 40, 8}, pattern, 0, found, 10);
    check(found.empty(), "pattern longer than the data");
    found.clear();
    std::vector<std::uint8_t> edge(match_b, match_b + sizeof(match_b));
    cmfm::stracker::find_all(edge, pattern, 0, found, 10);
    check(found == std::vector<std::size_t>{0}, "match filling the whole buffer");

    check(cmfm::stracker::version_pointer_address(0x1000, 0x20) == 0x1000 - 0x18 + 7 + 0x20, "pointer, forward");
    check(cmfm::stracker::version_pointer_address(0x1000, -0x100) == 0x1000 - 0x18 + 7 - 0x100, "pointer, back");
    check(cmfm::stracker::plausible_version("421631") && cmfm::stracker::plausible_version("v15.11.01 (421471)"),
          "plausible versions");
    check(!cmfm::stracker::plausible_version("") && !cmfm::stracker::plausible_version(std::string(64, '1')) &&
          !cmfm::stracker::plausible_version("a\nb"), "implausible versions");
}

void plugin_order() {
    const auto files = cmfm::stracker::plugin_files({{L"Unbloat.dll"}, {L"!CRCBypass.dll"}, {L"auto_food_skill.dll"},
                                                     {L"LuaEngine.dll"}, {L"folder.dll", true}, {L"notes.txt"},
                                                     {L"Upper.DLL"}, {L".dll"}, {L"x.dll.bak"}, {L"a..dll"},
                                                     {L"BetterDailyCatSkills.dll"}});
    check(files == std::vector<std::wstring>{L"!CRCBypass.dll", L"a..dll", L"auto_food_skill.dll",
                                             L"BetterDailyCatSkills.dll", L"LuaEngine.dll", L"Unbloat.dll"},
          "only *.dll files (exact case), case-insensitive order");
}
}  // namespace

int main() {
    config();
    patterns();
    plugin_order();
    if (failures) {
        std::cerr << failures << " Stracker logic check(s) failed\n";
        return 1;
    }
    std::cout << "Stracker config, version pattern and plugin order checks passed\n";
    return 0;
}
