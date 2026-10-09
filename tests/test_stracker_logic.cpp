#include "stracker_logic.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char* label) {
    if (!condition) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
}

using al::stracker::Config;

bool parse(std::string_view text, Config& config, std::vector<std::string>& problems) {
    problems.clear();
    return al::stracker::parse_config(text, config, problems);
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

void plugin_order() {
    const auto files = al::stracker::plugin_files({{L"Unbloat.dll"}, {L"!First.dll"}, {L"auto_food_skill.dll"},
                                                     {L"LuaEngine.dll"}, {L"folder.dll", true}, {L"notes.txt"},
                                                     {L"Upper.DLL"}, {L".dll"}, {L"x.dll.bak"}, {L"a..dll"},
                                                     {L"BetterDailyCatSkills.dll"}});
    check(files == std::vector<std::wstring>{L"!First.dll", L"a..dll", L"auto_food_skill.dll",
                                             L"BetterDailyCatSkills.dll", L"LuaEngine.dll", L"Unbloat.dll"},
          "only *.dll files (exact case), case-insensitive order");
}
}  // namespace

int main() {
    config();
    plugin_order();
    if (failures) {
        std::cerr << failures << " Stracker logic check(s) failed\n";
        return 1;
    }
    std::cout << "Stracker config and plugin order checks passed\n";
    return 0;
}
