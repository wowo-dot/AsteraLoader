#include "common/launch_plan.hpp"

#include "common/paths.hpp"

namespace al {

std::wstring quote_argument(std::wstring_view argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }
    std::wstring quoted = L"\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t backslashes = 0;
        while (i < argument.size() && argument[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == argument.size()) {
            // Backslashes before the closing quote are doubled.
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (argument[i] == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
        } else {
            quoted.append(backslashes, L'\\');
        }
        quoted.push_back(argument[i]);
    }
    quoted.push_back(L'"');
    return quoted;
}

bool plan_launch(const std::vector<std::wstring>& arguments, std::wstring_view launcher_dir,
                 std::wstring_view current_dir,
                 const std::function<bool(const std::wstring&)>& file_exists, LaunchPlan& plan,
                 std::wstring& error) {
    plan = {};
    error.clear();
    std::size_t first_pass_through = 0;

    if (!arguments.empty() && iequals(file_name_of(arguments[0]), kGameExeName)) {
        const auto& argument = arguments[0];
        if (is_absolute(argument)) {
            plan.exe = argument;
        } else if (argument.size() >= 2 && argument[1] == L':') {
            error = L"Use a full game path, such as C:\\folder\\MonsterHunterWorld.exe, rather than a drive-relative path.";
            return false;
        } else if (!argument.empty() && (argument.front() == L'\\' || argument.front() == L'/')) {
            if (current_dir.size() < 3 || current_dir[1] != L':') {
                error = L"Use a full game path when the current folder has no drive letter.";
                return false;
            }
            plan.exe = std::wstring(current_dir.substr(0, 2)) + argument;
        } else {
            plan.exe = join_path(current_dir, argument);
        }
        plan.source = L"argument";
        first_pass_through = 1;
        if (!file_exists(plan.exe)) {
            error = L"The game given as the first argument does not exist:\n" + plan.exe;
            return false;
        }
    } else {
        const std::wstring beside_launcher = join_path(launcher_dir, kGameExeName);
        const std::wstring in_current = join_path(current_dir, kGameExeName);
        if (file_exists(beside_launcher)) {
            plan.exe = beside_launcher;
            plan.source = L"launcher folder";
        } else if (file_exists(in_current)) {
            plan.exe = in_current;
            plan.source = L"current folder";
        } else {
            error = std::wstring(kGameExeName) + L" was not found next to the launcher (" +
                    std::wstring(launcher_dir) + L") or in the current folder (" +
                    std::wstring(current_dir) +
                    L").\nPass the game's path as the first argument, for example as Steam "
                    L"launch options: \"<path>\\astera_launcher.exe\" %command%";
            return false;
        }
    }

    plan.command_line = quote_argument(plan.exe);
    for (std::size_t i = first_pass_through; i < arguments.size(); ++i) {
        plan.command_line.push_back(L' ');
        plan.command_line += quote_argument(arguments[i]);
    }
    plan.working_dir = std::wstring(directory_of(plan.exe));
    return true;
}

}  // namespace al
