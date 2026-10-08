#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace cmfm {

inline constexpr std::wstring_view kGameExeName = L"MonsterHunterWorld.exe";

// Quotes one argument so CommandLineToArgvW and the MSVC CRT read it back
// unchanged.
std::wstring quote_argument(std::wstring_view argument);

struct LaunchPlan {
    std::wstring exe;           // full path of the game exe
    std::wstring command_line;  // quoted exe, then the pass-through arguments
    std::wstring working_dir;   // the exe's folder
    std::wstring source;        // "argument", "launcher folder" or "current folder"
};

// Decides which exe to start and with what command line.
//
// `arguments` excludes argv[0]. The game is found, in order:
//   1. a MonsterHunterWorld.exe path as the first argument (Steam's %command%);
//   2. next to the launcher;
//   3. in the current folder.
// Every other argument passes through to the game. `file_exists` reports
// whether a regular file exists at a path.
bool plan_launch(const std::vector<std::wstring>& arguments, std::wstring_view launcher_dir,
                 std::wstring_view current_dir,
                 const std::function<bool(const std::wstring&)>& file_exists, LaunchPlan& plan,
                 std::wstring& error);

}  // namespace cmfm
