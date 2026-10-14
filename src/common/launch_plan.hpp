#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace al {

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

// Monster Hunter: World's Steam app id.
inline constexpr std::wstring_view kSteamAppId = L"582010";

struct SteamPlan {
    std::wstring app_id;  // the SteamAppId the game starts with
    std::wstring source;  // "inherited" (Steam or MO2 set it) or "launcher"
};

// Decides the Steam app id the game starts with. Steam (with launch
// options) and MO2 start the launcher with SteamAppId set, and the game
// inherits it. Without it, the game's Steam start-up has Steam start the
// game again, without Astera Loader (exit code 53), so a launcher started
// any other way gives the game MHW's id, as they do. Steam still checks
// that the game is owned, which needs Steam running: `steam_running` is
// asked only then.
bool plan_steam(std::wstring_view inherited, const std::function<bool()>& steam_running,
                SteamPlan& plan, std::wstring& error);

}  // namespace al
