#include "core/plugins/load_order.hpp"
#include "core/plugins/manifest.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char* label) {
    if (!condition) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
}
bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

void AL_CALL no_phase(al_plugin_t*, al_phase_t, const void*) {}

const std::wstring kExe(64, L'a');

al_plugin_info_t info_named(const char* name) {
    al_plugin_info_t info{};
    info.size = sizeof(info);
    info.api_version = AL_API_VERSION;
    info.name = name;
    info.version = "1.2.3";
    return info;
}

bool accepted(const al_plugin_info_t& info, std::wstring_view folder = L"sample",
              std::wstring_view exe = kExe) {
    al::plugins::Manifest manifest;
    std::string reason;
    return al::plugins::read_manifest(&info, folder, exe, manifest, reason);
}

std::string refusal(const al_plugin_info_t* info, std::wstring_view folder = L"sample",
                    std::wstring_view exe = kExe) {
    al::plugins::Manifest manifest;
    std::string reason;
    return al::plugins::read_manifest(info, folder, exe, manifest, reason) ? std::string() : reason;
}

void names() {
    using al::plugins::valid_name;
    check(valid_name("mhw_dlss") && valid_name("a") && valid_name("a.b-c_d9"), "valid names");
    check(valid_name(std::string(64, 'x')) && !valid_name(std::string(65, 'x')), "name length");
    check(!valid_name("") && !valid_name(".hidden") && !valid_name("Upper") && !valid_name("a b") &&
          !valid_name("a\\b") && !valid_name("caf\xc3\xa9"), "invalid names");
    check(valid_name(L"mhw_dlss") && !valid_name(L"Mhw") && !valid_name(L"\x00e9t\x00e9"), "wide names");
}

void manifests() {
    auto info = info_named("sample");
    al::plugins::Manifest manifest;
    std::string reason;
    check(al::plugins::read_manifest(&info, L"sample", kExe, manifest, reason), "minimal plugin accepted");
    check(manifest.name == "sample" && manifest.display_name == "sample" && manifest.version == "1.2.3" &&
          manifest.load_after.empty() && manifest.exe_sha256.empty(), "minimal plugin copied");
    check(accepted(info, L"SAMPLE"), "folder name case does not matter");
    check(contains(refusal(nullptr), "returned nothing"), "null info refused");

    info.size = 64;  // smaller than API 1.0's 72 bytes
    check(contains(refusal(&info), "bytes"), "short info refused");
    info.size = sizeof(info);

    info.api_version = AL_MAKE_VERSION(2, 0);
    check(contains(refusal(&info), "needs Astera Loader API 2.0; this Astera Loader provides 1.7. Update Astera Loader."), "newer major");
    info.api_version = AL_MAKE_VERSION(1, 8);
    check(contains(refusal(&info), "needs Astera Loader API 1.8"), "newer minor refused");
    info.api_version = AL_MAKE_VERSION(0, 9);
    check(contains(refusal(&info), "no longer supports"), "older major refused");
    info.api_version = AL_API_VERSION;

    check(contains(refusal(&info, L"other"), "does not match its folder 'other'"), "name must match folder");
    auto bad = info_named("Bad Name");
    check(contains(refusal(&bad, L"Bad Name"), "not a valid plugin name"), "invalid name refused");

    info.display_name = "Sample Plugin";
    info.phases = AL_PHASE_BIT(AL_PHASE_PRE_MAIN);
    check(contains(refusal(&info), "no on_phase"), "phases need on_phase");
    info.on_phase = no_phase;
    check(al::plugins::read_manifest(&info, L"sample", kExe, manifest, reason) &&
          manifest.display_name == "Sample Plugin" && manifest.on_phase == no_phase, "phases accepted");
    info.phases = AL_PHASE_BIT(AL_PHASE_COUNT);
    check(contains(refusal(&info), "does not know"), "unknown phase refused");
    info.phases = 0;

    std::vector<const char*> endless(65, "x");
    info.load_after = endless.data();
    check(contains(refusal(&info), "no NULL end"), "unterminated list refused");
    const char* const after[] = {"gamma", "beta", nullptr};
    info.load_after = after;
    check(al::plugins::read_manifest(&info, L"sample", kExe, manifest, reason) &&
          manifest.load_after == std::vector<std::string>{"gamma", "beta"}, "load_after copied");
}

void exe_hashes() {
    auto info = info_named("sample");
    const std::string ours(64, 'a'), upper(64, 'A'), other(64, 'b');
    const char* const listed[] = {other.c_str(), upper.c_str(), nullptr};
    info.exe_sha256 = listed;
    check(accepted(info), "listed exe accepted, upper-case entry");
    check(contains(refusal(&info, L"sample", std::wstring(64, L'c')), "does not support this game exe"),
          "unlisted exe refused");
    check(contains(refusal(&info, L"sample", L""), "not available"), "unknown exe refused when hashes listed");
    const char* const invalid[] = {"abc", nullptr};
    info.exe_sha256 = invalid;
    check(contains(refusal(&info), "not a SHA-256"), "invalid hash entry refused");
    info.exe_sha256 = nullptr;
    check(accepted(info, L"sample", L""), "any exe when no hashes listed");
}

al::plugins::Manifest plugin(std::string name, int order = 0, std::vector<std::string> after = {},
                               std::vector<std::string> before = {}) {
    al::plugins::Manifest manifest;
    manifest.name = std::move(name);
    manifest.order = order;
    manifest.load_after = std::move(after);
    manifest.load_before = std::move(before);
    return manifest;
}

std::vector<std::string> sequence(const std::vector<al::plugins::Manifest>& plugins) {
    std::vector<std::string> names;
    for (const auto i : al::plugins::resolve_load_order(plugins).sequence) names.push_back(plugins[i].name);
    return names;
}

void versions() {
    using al::plugins::compare_versions;
    using al::plugins::parse_version;
    using V = std::vector<std::uint32_t>;
    check(parse_version("0.14.0") == V{0, 14, 0} && parse_version("v1.2") == V{1, 2} &&
          parse_version("0.14.0-beta") == V{0, 14, 0} && parse_version("2+build 7") == V{2} &&
          parse_version("421810") == V{421810}, "readable versions");
    for (const char* bad : {"", "v", "beta", ".1", "1.", "1..2", "1.x", "1_2", "99999999999"}) {
        check(!parse_version(bad), bad);
    }
    check(compare_versions(V{1, 2}, V{1, 2, 0}) == 0 && compare_versions(V{1, 2}, V{1, 10}) < 0 &&
          compare_versions(V{2}, V{1, 99}) > 0, "comparison, missing parts are 0");
}

void dependencies() {
    // A plugin built for API 1.0 ends before `dependencies`: still accepted.
    auto info = info_named("sample");
    info.api_version = AL_MAKE_VERSION(1, 0);
    info.size = 72;
    const al_dependency_t never_read[] = {{"invalid name!", nullptr}, {nullptr, nullptr}};
    info.dependencies = never_read;
    al::plugins::Manifest manifest;
    std::string reason;
    check(al::plugins::read_manifest(&info, L"sample", kExe, manifest, reason) && manifest.dependencies.empty(),
          "API 1.0 plugin accepted, dependencies not read");

    info = info_named("sample");
    const al_dependency_t needs[] = {{"provider", "0.2"}, {"other", nullptr}, {nullptr, nullptr}};
    info.dependencies = needs;
    check(al::plugins::read_manifest(&info, L"sample", kExe, manifest, reason) && manifest.dependencies.size() == 2 &&
          manifest.dependencies[0].name == "provider" && manifest.dependencies[0].min_version == "0.2" &&
          manifest.dependencies[1].min_version.empty(), "dependencies read");
    const al_dependency_t bad_name[] = {{"Bad Name", nullptr}, {nullptr, nullptr}};
    info.dependencies = bad_name;
    check(contains(refusal(&info), "invalid name"), "dependency name checked");
    const al_dependency_t bad_version[] = {{"provider", "latest"}, {nullptr, nullptr}};
    info.dependencies = bad_version;
    check(contains(refusal(&info), "unreadable version \"latest\""), "dependency version checked");
    std::vector<al_dependency_t> endless(65, al_dependency_t{"x", nullptr});
    info.dependencies = endless.data();
    check(contains(refusal(&info), "dependency list has no end"), "unterminated dependency list");
}

al::plugins::Manifest needing(std::string name, std::string version, std::vector<al::plugins::Dependency> deps) {
    al::plugins::Manifest manifest;
    manifest.name = std::move(name);
    manifest.version = std::move(version);
    manifest.dependencies = std::move(deps);
    return manifest;
}

void dependency_checks() {
    using al::plugins::check_dependencies;
    const std::vector<al::plugins::Manifest> plugins{
        needing("provider", "0.3.1", {}),
        needing("ok", "1", {{"provider", "0.3"}, {"ok", ""}}),
        needing("too_old", "1", {{"provider", "0.4"}}),
        needing("absent", "1", {{"ghost", "2.0"}}),
        needing("refused_dep", "1", {{"broken", ""}}),
        needing("cascade", "1", {{"too_old", ""}}),
        needing("odd_version", "1", {{"weird", "1.0"}}),
        needing("weird", "nightly", {}),
    };
    const auto refusals = check_dependencies(plugins, {{"broken", "its name is not a valid plugin name"}});
    std::vector<std::string> reasons(plugins.size());
    for (const auto& refusal : refusals) reasons[refusal.index] = refusal.reason;
    check(reasons[0].empty() && reasons[1].empty() && reasons[7].empty(), "met dependencies, self ignored");
    check(reasons[2] == "it needs provider 0.4 or newer; 0.3.1 is installed", "too old");
    check(reasons[3] == "it needs ghost 2.0, which is not installed", "not installed");
    check(reasons[4] == "it needs broken, which will not run", "dependency refused at load");
    check(reasons[5] == "it needs too_old, which will not run", "refusal cascades");
    check(reasons[6] == "it needs weird 1.0, but weird's version \"nightly\" cannot be compared", "uncomparable");
    check(refusals.size() == 5, "five refusals");

    // Dependencies also order the start: provider before ok, though ok sorts first.
    const auto order = al::plugins::resolve_load_order({needing("ok", "1", {{"zprovider", ""}}),
                                                          needing("zprovider", "1", {})});
    check(order.sequence == std::vector<std::size_t>{1, 0}, "dependency starts first");
}

void load_order() {
    using Names = std::vector<std::string>;
    std::vector<al::plugins::Manifest> plugins{
        plugin("beta", 0, {"GAMMA"}), plugin("delta", 10, {}, {"beta"}), plugin("alpha"), plugin("gamma", 5)};
    check(sequence(plugins) == Names{"alpha", "gamma", "delta", "beta"}, "constraints before order numbers");
    std::sort(plugins.begin(), plugins.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    do {
        check(sequence(plugins) == Names{"alpha", "gamma", "delta", "beta"}, "independent of input order");
    } while (std::next_permutation(plugins.begin(), plugins.end(),
                                   [](const auto& a, const auto& b) { return a.name < b.name; }));

    check(sequence({plugin("b", 0, {"missing", "b"}), plugin("a", 0, {}, {"absent"})}) == Names{"a", "b"},
          "absent and self names ignored");
    check(sequence({plugin("late", 1), plugin("early", -1), plugin("mid")}) == Names{"early", "mid", "late"},
          "order numbers, negative first");

    const std::vector<al::plugins::Manifest> cyclic{
        plugin("x", 0, {"y"}), plugin("y", 0, {"x"}), plugin("z", 0, {"x"}), plugin("a", 0, {}, {"b"}),
        plugin("b", 0, {}, {"c"}), plugin("c", 0, {}, {"a"}), plugin("free")};
    const auto result = al::plugins::resolve_load_order(cyclic);
    Names loaded;
    for (const auto i : result.sequence) loaded.push_back(cyclic[i].name);
    check(loaded == Names{"free", "z"}, "cycles refused, the rest still load");
    check(result.refused.size() == 5, "every cycle member refused");
    for (const auto& refused : result.refused) {
        if (cyclic[refused.index].name == "x") check(refused.reason == "its load order forms a cycle with y", "x");
        if (cyclic[refused.index].name == "a") {
            check(refused.reason == "its load order forms a cycle with b, c", "three-plugin cycle reason");
        }
    }

    const auto duplicate = al::plugins::resolve_load_order({plugin("same"), plugin("same", -5)});
    check(duplicate.sequence == std::vector<std::size_t>{0} && duplicate.refused.size() == 1 &&
          duplicate.refused[0].index == 1, "repeated name refused");
}
}  // namespace

int main() {
    names();
    manifests();
    exe_hashes();
    versions();
    dependencies();
    dependency_checks();
    load_order();
    if (failures) {
        std::cerr << failures << " plugin check(s) failed\n";
        return 1;
    }
    std::cout << "Plugin manifest and load-order checks passed\n";
    return 0;
}
