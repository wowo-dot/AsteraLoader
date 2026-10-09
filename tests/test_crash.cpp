// CMFM's crash handler. Pure parts first; then child processes of this exe
// load the crashing fixture from plugin paths and crash with the handler in
// place. Each child ends in a last filter that terminates it, so Windows
// error reporting never runs. argv[1] is the fixture DLL, argv[2] a work
// folder.
#include "core/crash/handler.hpp"
#include "core/crash/report.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
namespace crash = cmfm::crash;

namespace {
int failures = 0;
void check(bool condition, const std::string& label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        ++failures;
    }
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

void pure() {
    using crash::OwnerKind;
    const auto owner = [](const wchar_t* path, bool core = false, bool game = false) {
        return crash::classify(path, core, game);
    };
    auto o = owner(L"D:\\Games\\MHW\\cmfm\\plugins\\mhw_dlss\\mhw_dlss.dll");
    check(o.kind == OwnerKind::plugin && o.name == L"mhw_dlss", "CMFM plugin");
    o = owner(L"\\Device\\HarddiskVolume3\\MO2\\MHW\\mods\\MHW DLSS (CMFM plugin)\\cmfm\\plugins\\mhw_dlss\\sl.common.dll");
    check(o.kind == OwnerKind::plugin && o.name == L"mhw_dlss", "a DLL a plugin ships, MO2's real folder");
    o = owner(L"\\Device\\HarddiskVolume3\\MO2\\MHW\\mods\\Stracker plugins\\NativePC\\Plugins\\more_auto_shoutouts.dll");
    check(o.kind == OwnerKind::stracker_plugin && o.name == L"more_auto_shoutouts.dll", "Stracker plugin, any case");
    o = owner(L"\\Device\\HarddiskVolume2\\Windows\\System32\\KERNELBASE.dll");
    check(o.kind == OwnerKind::windows && o.name == L"KERNELBASE.dll", "Windows");
    o = owner(L"D:\\Games\\MHW\\cmfm\\cmfm.dll", true);
    check(o.kind == OwnerKind::core, "CMFM core by base");
    o = owner(L"D:\\Games\\MHW\\MonsterHunterWorld.exe", false, true);
    check(o.kind == OwnerKind::game && o.name == L"MonsterHunterWorld.exe", "the game by base");
    o = owner(L"C:\\Program Files (x86)\\Steam\\gameoverlayrenderer64.dll");
    check(o.kind == OwnerKind::other && o.name == L"gameoverlayrenderer64.dll", "other modules");
    o = owner(L"D:\\Games\\MHW\\cmfm\\plugins\\");
    check(o.kind == OwnerKind::other, "a plugins folder without a plugin name");
    check(owner(L"").kind == OwnerKind::unknown, "no path");

    check(crash::fatal(0xC0000005u) && crash::fatal(0xC00000FDu) && !crash::fatal(0xE06D7363u) &&
              !crash::fatal(0x80000004u) && !crash::fatal(0x80000003u), "fatal codes, not C++ or debug events");
    check(std::string(crash::exception_name(0xC0000005u)) == "access violation", "exception names");

    check(crash::minidump_setting("") == crash::Minidump::off, "minidumps off by default");
    check(crash::minidump_setting("; CMFM\r\n Crash_Minidump = Small ; opt in\r\n") == crash::Minidump::small,
          "small minidumps, case and comments");
    check(crash::minidump_setting("crash_minidump=full") == crash::Minidump::full, "full minidumps");
    check(crash::minidump_setting("crash_minidump = maybe") == crash::Minidump::off, "unknown values are off");
}

// --- child side ---

fs::path g_case;  // the child's case folder

void marker(const wchar_t* name) {
    std::ofstream(g_case / name) << "called";
}

// The filter that was there before CMFM's (as the game's C runtime's is).
LONG WINAPI previous_filter(EXCEPTION_POINTERS* info) {
    marker(L"previous.txt");
    TerminateProcess(GetCurrentProcess(), info->ExceptionRecord->ExceptionCode);
    return EXCEPTION_CONTINUE_SEARCH;
}

// A filter set after CMFM's that does not keep its predecessor.
LONG WINAPI replacing_filter(EXCEPTION_POINTERS* info) {
    marker(L"replacer.txt");
    TerminateProcess(GetCurrentProcess(), info->ExceptionRecord->ExceptionCode);
    return EXCEPTION_CONTINUE_SEARCH;
}

int child(const std::wstring& mode, const fs::path& folder, const fs::path& dll) {
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    g_case = folder;
    if (mode != L"replaced") SetUnhandledExceptionFilter(previous_filter);
    crash::install({(folder / L"logs").wstring(), (folder / L"cmfm.ini").wstring(), "test", "feedbeef"});
    if (!crash::installed()) return 3;
    crash::note_phase(mode == L"exiting" ? CMFM_PHASE_SHUTDOWN : CMFM_PHASE_FIRST_PRESENT);
    if (mode == L"replaced") SetUnhandledExceptionFilter(replacing_filter);
    const HMODULE module = LoadLibraryW(dll.c_str());
    if (!module) return 4;
    using Fn = void (*)();
    const auto function = reinterpret_cast<Fn>(GetProcAddress(module, mode == L"replaced" ? "crash_in_windows"
                                                                                          : "crash_on_thread"));
    if (!function) return 5;
    function();
    Sleep(30000);  // the crashing thread ends the process
    return 6;
}

// --- parent side ---

DWORD run_child(const std::wstring& mode, const fs::path& folder, const fs::path& dll) {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" child " + mode + L" \"" + folder.wstring() + L"\" \"" +
                           dll.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        return 0xFFFFFFFF;
    }
    WaitForSingleObject(process.hProcess, 60000);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
}

fs::path find_report(const fs::path& folder, const wchar_t* extension) {
    if (!fs::exists(folder / L"logs" / L"crashes")) return {};
    for (const auto& entry : fs::directory_iterator(folder / L"logs" / L"crashes")) {
        const auto name = entry.path().filename().wstring();
        if (name.rfind(L"crash-", 0) == 0 && entry.path().extension() == extension) return entry.path();
    }
    return {};
}

fs::path prepare(const fs::path& work, const wchar_t* name, const fs::path& relative_dll, const fs::path& fixture) {
    const auto folder = work / name;
    fs::create_directories((folder / relative_dll).parent_path());
    fs::copy_file(fixture, folder / relative_dll, fs::copy_options::overwrite_existing);
    return folder;
}

void plugin_thread(const fs::path& work, const fs::path& fixture) {
    const auto folder = prepare(work, L"plugin_thread", L"cmfm/plugins/crashy/crashy.dll", fixture);
    const auto code = run_child(L"plain", folder, folder / L"cmfm/plugins/crashy/crashy.dll");
    check(code == 0xC0000005u, "a plugin thread's crash ends the process with its code");
    const auto report = read_file(find_report(folder, L".txt"));
    check(has(report, "cmfm=test exe_sha256=feedbeef"), "report header");
    check(has(report, "phase=first_present (while the game was running)"), "phase");
    check(has(report, "exception=0xc0000005 access violation, writing 0x0"), "exception and access");
    check(has(report, "at=crashy.dll+0x") && has(report, "(plugin crashy)"), "faulting module and its plugin");
    check(has(report, "owner=plugin crashy (the faulting module)"), "owner");
    check(has(report, "thread_started_in=crashy.dll+0x"), "thread origin");
    check(has(report, "stack:\r\n  00 crashy.dll+0x"), "stack starts at the fault");
    check(has(report, " rip=0x"), "registers");
    check(!has(report, "minidump="), "no minidump unless asked");
    check(fs::exists(folder / L"previous.txt"), "chained to the previous filter");
    if (failures) std::cerr << report << '\n';

    const auto logs = (folder / L"logs").wstring();
    const auto notice = crash::take_previous(logs);
    check(notice.find(L"while the game was running, in the CMFM plugin \"crashy\": access violation at crashy.dll+0x") !=
              std::wstring::npos && notice.find(L"try the game without that plugin") != std::wstring::npos,
          "next-launch notice names the plugin");
    check(crash::take_previous(logs).empty() && fs::exists(folder / L"logs/crashes/last-crash.seen.txt"),
          "the notice is shown once");
}

void replaced_filter(const fs::path& work, const fs::path& fixture) {
    const auto folder = prepare(work, L"replaced", L"nativePC/plugins/stracky.dll", fixture);
    const auto code = run_child(L"replaced", folder, folder / L"nativePC/plugins/stracky.dll");
    check(code == 0xC0000005u, "a crash inside Windows ends the process with its code");
    const auto report = read_file(find_report(folder, L".txt"));
    check(has(report, "owner=Stracker plugin stracky.dll (the first plugin on the stack)"),
          "owner found on the stack below Windows code");
    check(has(report, "(Windows "), "faulting module is Windows code");
    check(has(report, "(the game's main thread)"), "main thread named");
    check(fs::exists(folder / L"replacer.txt"), "the replacing filter still runs, after CMFM's");
    const auto notice = crash::take_previous((folder / L"logs").wstring());
    check(notice.find(L"in the Stracker plugin stracky.dll (inside ") != std::wstring::npos, "notice: called from");
    if (failures) std::cerr << report << '\n';
}

void exiting_with_dump(const fs::path& work, const fs::path& fixture) {
    const auto folder = prepare(work, L"exiting", L"cmfm/plugins/crashy/crashy.dll", fixture);
    std::ofstream(folder / L"cmfm.ini") << "crash_minidump = small\r\n";
    const auto code = run_child(L"exiting", folder, folder / L"cmfm/plugins/crashy/crashy.dll");
    check(code == 0xC0000005u, "exit-time crash code");
    const auto report = read_file(find_report(folder, L".txt"));
    check(has(report, "phase=shutdown (while the game was exiting)"), "exit-time phase");
    const auto dump = find_report(folder, L".dmp");
    check(!dump.empty() && fs::file_size(dump) > 4096 && has(report, "minidump=") && !has(report, "minidump=failed"),
          "minidump written when asked");
    check(!fs::exists(folder / L"logs/crashes/last-crash.txt"), "no notice for a crash while exiting");
    if (failures) std::cerr << report << '\n';
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 5 && std::wstring(argv[1]) == L"child") return child(argv[2], argv[3], argv[4]);
    if (argc != 3) return 2;
    pure();
    const auto work = fs::path(argv[2]) / std::to_wstring(GetCurrentProcessId());
    fs::create_directories(work);
    plugin_thread(work, argv[1]);
    replaced_filter(work, argv[1]);
    exiting_with_dump(work, argv[1]);
    if (failures) {
        std::cerr << failures << " crash handler check(s) failed\n";
        return 1;
    }
    std::cout << "crash reports name the plugin, chain to other filters, and dump and notify as configured\n";
    return 0;
}
