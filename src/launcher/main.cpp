#include "common/launch_plan.hpp"
#include "common/log_file.hpp"
#include "common/paths.hpp"
#include "common/sha256.hpp"
#include "common/startup.hpp"
#include "common/windows.hpp"

#include <windows.h>
#include <psapi.h>
#include <shellapi.h>

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr DWORD kThreadTimeoutMs = 15000;
struct Failure { std::wstring message; };

[[noreturn]] void fail(std::wstring_view action, DWORD code = GetLastError()) {
    throw Failure{std::wstring(action) + L": " + al::win32_error_text(code)};
}

void notify(al::LogFile& log, const std::wstring& message, UINT icon) {
    log.line(message);
#ifndef AL_TEST_NO_DIALOGS
    MessageBoxW(nullptr, message.c_str(), L"Astera Loader Launcher", MB_OK | icon);
#else
    (void)icon;
#endif
}

void log_exe_hash(al::LogFile& log, const std::wstring& exe) noexcept {
    // This is diagnostic data. Once the game is resumed, neither a read
    // failure nor a logging/allocation exception may end the launcher early.
    try {
        const auto hash = al::sha256_file(exe);
        log.line(hash.empty() ? L"warning: exe SHA-256 unavailable; game continues" :
                               L"exe_sha256=" + hash);
    } catch (...) {
        try {
            log.line(L"warning: exe SHA-256 logging failed; game continues");
        } catch (...) { /* Keep waiting for the game even if the log cannot be written. */ }
    }
}

struct CreatedGame {
    al::Handle process;
    al::Handle thread;
    al::LogFile& log;
    bool resumed = false;
    bool terminated = false;
    void stop_before_resume() {
        if (terminated) return;
        if (process && !resumed) {
            // Only the child returned by our own CreateProcessW can reach here.
            if (TerminateProcess(process.get(), ERROR_CANCELLED)) {
                const auto result = WaitForSingleObject(process.get(), 5000);
                terminated = result == WAIT_OBJECT_0;
                log.line(result == WAIT_OBJECT_0 ? L"suspended_child_terminated" :
                                                  L"suspended_child_termination_wait_failed");
            } else {
                log.line(L"suspended_child_termination_failed: " +
                         al::win32_error_text(GetLastError()));
            }
        }
    }
    ~CreatedGame() {
        try { stop_before_resume(); } catch (...) { /* Cleanup must not throw. */ }
    }
};

class RemoteBuffer {
public:
    RemoteBuffer(HANDLE process, const void* data, SIZE_T size) : process_(process) {
        address_ = VirtualAllocEx(process, nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!address_) fail(L"Allocate DLL-loading parameter");
        SIZE_T written = 0;
        if (!WriteProcessMemory(process, address_, data, size, &written) || written != size) {
            const DWORD code = GetLastError();
            VirtualFreeEx(process_, address_, 0, MEM_RELEASE);
            fail(L"Write DLL-loading parameter", code);
        }
    }
    RemoteBuffer(const RemoteBuffer&) = delete;
    RemoteBuffer& operator=(const RemoteBuffer&) = delete;
    ~RemoteBuffer() {
        // On a timeout, do not free a parameter the live thread may still use.
        // The suspended-child guard terminates that child and releases its VA.
        if (safe_to_free_) VirtualFreeEx(process_, address_, 0, MEM_RELEASE);
    }
    DWORD invoke(std::uintptr_t entry) {
        const auto routine = std::bit_cast<LPTHREAD_START_ROUTINE>(entry);
        al::Handle thread(CreateRemoteThread(process_, nullptr, 0, routine, address_, 0, nullptr));
        if (!thread) fail(L"Start DLL-loading thread");
        safe_to_free_ = false;
        const DWORD wait = WaitForSingleObject(thread.get(), kThreadTimeoutMs);
        if (wait != WAIT_OBJECT_0) {
            fail(L"Wait for DLL-loading thread", wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
        }
        safe_to_free_ = true;
        DWORD code = 0;
        if (!GetExitCodeThread(thread.get(), &code)) fail(L"Read DLL-loading thread result");
        return code;
    }

private:
    HANDLE process_;
    void* address_ = nullptr;
    bool safe_to_free_ = true;
};

DWORD invoke_remote(HANDLE process, std::uintptr_t entry, const void* parameter, SIZE_T size) {
    RemoteBuffer buffer(process, parameter, size);
    return buffer.invoke(entry);
}

std::wstring mapped_name(HANDLE process, void* address) {
    std::wstring name(32768, L'\0');
    const auto length = GetMappedFileNameW(process, address, name.data(), static_cast<DWORD>(name.size()));
    if (!length || length >= name.size()) return {};
    name.resize(length);
    return name;
}

// Module lists may not exist yet in a just-created suspended process. Walking
// MEM_IMAGE allocations finds ntdll without running the primary thread. Names
// on both sides come from NT file paths, never virtual vs canonical paths.
std::uintptr_t remote_image(HANDLE process, const std::wstring& native_name) {
    std::uintptr_t cursor = 0;
    void* previous = nullptr;
    MEMORY_BASIC_INFORMATION info{};
    while (VirtualQueryEx(process, reinterpret_cast<void*>(cursor), &info, sizeof(info))) {
        if (info.Type == MEM_IMAGE && info.AllocationBase != previous) {
            previous = info.AllocationBase;
            const auto name = mapped_name(process, info.AllocationBase);
            if (!name.empty() && al::iequals(name, native_name)) {
                return reinterpret_cast<std::uintptr_t>(info.AllocationBase);
            }
        }
        const auto next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= cursor) break;
        cursor = next;
    }
    return 0;
}

bool executable_page(DWORD protection) {
    if (protection & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const auto access = protection & 0xff;
    return access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ ||
           access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
}

std::uintptr_t remote_system_function(HANDLE process, HMODULE module, const char* name) {
    const auto function = GetProcAddress(module, name);
    if (!function) fail(L"Find Windows DLL-loading function");
    const auto local_address = std::bit_cast<std::uintptr_t>(function);
    MEMORY_BASIC_INFORMATION local{};
    if (!VirtualQuery(reinterpret_cast<void*>(local_address), &local, sizeof(local))) {
        fail(L"Find Windows function's owning image");
    }
    const auto native_name = mapped_name(GetCurrentProcess(), local.AllocationBase);
    const auto base = native_name.empty() ? 0 : remote_image(process, native_name);
    if (!base) fail(L"Find matching Windows image in suspended child", ERROR_MOD_NOT_FOUND);
    const auto offset = local_address - reinterpret_cast<std::uintptr_t>(local.AllocationBase);
    const auto* local_dos = static_cast<const IMAGE_DOS_HEADER*>(local.AllocationBase);
    const auto* local_pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        static_cast<const std::byte*>(local.AllocationBase) + local_dos->e_lfanew);
    IMAGE_DOS_HEADER remote_dos{};
    IMAGE_NT_HEADERS64 remote_pe{};
    SIZE_T got = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<void*>(base), &remote_dos, sizeof(remote_dos), &got) ||
        got != sizeof(remote_dos) || remote_dos.e_magic != IMAGE_DOS_SIGNATURE ||
        remote_dos.e_lfanew != local_dos->e_lfanew ||
        !ReadProcessMemory(process, reinterpret_cast<void*>(base + remote_dos.e_lfanew),
                           &remote_pe, sizeof(remote_pe), &got) || got != sizeof(remote_pe) ||
        remote_pe.Signature != IMAGE_NT_SIGNATURE ||
        remote_pe.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        remote_pe.FileHeader.TimeDateStamp != local_pe->FileHeader.TimeDateStamp ||
        remote_pe.OptionalHeader.SizeOfImage != local_pe->OptionalHeader.SizeOfImage ||
        offset >= remote_pe.OptionalHeader.SizeOfImage) {
        fail(L"Verify Windows function's remote image", ERROR_BAD_EXE_FORMAT);
    }
    MEMORY_BASIC_INFORMATION remote{};
    if (!VirtualQueryEx(process, reinterpret_cast<void*>(base + offset), &remote, sizeof(remote)) ||
        remote.State != MEM_COMMIT || reinterpret_cast<std::uintptr_t>(remote.AllocationBase) != base ||
        !executable_page(remote.Protect)) {
        fail(L"Verify Windows function's remote code page", ERROR_INVALID_ADDRESS);
    }
    return base + offset;
}

class CoreImage {
public:
    explicit CoreImage(HANDLE file) {
        mapping_.reset(CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr));
        if (!mapping_) fail(L"Map Astera Loader image for export validation");
        view_ = static_cast<const std::byte*>(MapViewOfFile(mapping_.get(), FILE_MAP_READ, 0, 0, 0));
        if (!view_) fail(L"Read Astera Loader image");
    }
    ~CoreImage() { if (view_) UnmapViewOfFile(view_); }
    CoreImage(const CoreImage&) = delete;
    CoreImage& operator=(const CoreImage&) = delete;

    DWORD bootstrap_rva() const {
        if (!readable(0, sizeof(IMAGE_DOS_HEADER))) fail(L"Read Astera Loader DOS header", ERROR_BAD_EXE_FORMAT);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(view_);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER) ||
            dos->e_lfanew > 0x100000) fail(L"Validate Astera Loader DOS header", ERROR_BAD_EXE_FORMAT);
        if (!readable(static_cast<DWORD>(dos->e_lfanew), sizeof(IMAGE_NT_HEADERS64))) {
            fail(L"Validate Astera Loader PE header mapping", ERROR_BAD_EXE_FORMAT);
        }
        const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(view_ + dos->e_lfanew);
        if (pe->Signature != IMAGE_NT_SIGNATURE || pe->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            !(pe->FileHeader.Characteristics & IMAGE_FILE_DLL) ||
            pe->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
            pe->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            pe->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) {
            fail(L"Validate Astera Loader x64 DLL", ERROR_BAD_EXE_FORMAT);
        }
        const auto size = pe->OptionalHeader.SizeOfImage;
        const auto& directory = pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        const auto within = [this, size](DWORD offset, std::uint64_t length) {
            return offset != 0 && offset < size && length <= size - offset && readable(offset, length);
        };
        if (!within(directory.VirtualAddress, directory.Size) || directory.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
            fail(L"Astera Loader has no bootstrap export", ERROR_PROC_NOT_FOUND);
        }
        const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(view_ + directory.VirtualAddress);
        if (!within(exports->AddressOfNames, std::uint64_t{exports->NumberOfNames} * sizeof(DWORD)) ||
            !within(exports->AddressOfNameOrdinals, std::uint64_t{exports->NumberOfNames} * sizeof(WORD)) ||
            !within(exports->AddressOfFunctions, std::uint64_t{exports->NumberOfFunctions} * sizeof(DWORD))) {
            fail(L"Validate Astera Loader export table", ERROR_BAD_EXE_FORMAT);
        }
        const auto* names = reinterpret_cast<const DWORD*>(view_ + exports->AddressOfNames);
        const auto* ordinals = reinterpret_cast<const WORD*>(view_ + exports->AddressOfNameOrdinals);
        const auto* functions = reinterpret_cast<const DWORD*>(view_ + exports->AddressOfFunctions);
        for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
            constexpr auto length = sizeof(al::kBootstrapExport);
            if (!within(names[i], length) || std::memcmp(view_ + names[i], al::kBootstrapExport, length)) continue;
            if (ordinals[i] >= exports->NumberOfFunctions) break;
            const DWORD rva = functions[ordinals[i]];
            if (!within(rva, 1) || (rva >= directory.VirtualAddress &&
                rva - directory.VirtualAddress < directory.Size)) break;
            const auto* sections = IMAGE_FIRST_SECTION(pe);
            const auto section_offset = reinterpret_cast<const std::byte*>(sections) - view_;
            if (section_offset < 0 || static_cast<std::uint64_t>(section_offset) >= size ||
                !within(static_cast<DWORD>(section_offset),
                        std::uint64_t{pe->FileHeader.NumberOfSections} * sizeof(IMAGE_SECTION_HEADER))) break;
            for (WORD j = 0; j < pe->FileHeader.NumberOfSections; ++j) {
                const auto& section = sections[j];
                if (rva >= section.VirtualAddress && rva - section.VirtualAddress < section.Misc.VirtualSize &&
                    (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) return rva;
            }
            break;
        }
        fail(L"Astera Loader has no executable bootstrap export", ERROR_PROC_NOT_FOUND);
    }

private:
    bool readable(DWORD offset, std::uint64_t length) const {
        const auto base = reinterpret_cast<std::uintptr_t>(view_);
        auto cursor = base + offset;
        const auto end = cursor + length;
        if (end < cursor) return false;
        while (cursor < end) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<void*>(cursor), &region, sizeof(region)) ||
                region.State != MEM_COMMIT || region.AllocationBase != view_ ||
                (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
            const auto next = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
            if (next <= cursor) return false;
            cursor = next;
        }
        return true;
    }
    al::Handle mapping_;
    const std::byte* view_ = nullptr;
};

DWORD launch() {
    al::LogFile log;
    CreatedGame child{{}, {}, log};
    try {
        LARGE_INTEGER launcher_qpc{};
        QueryPerformanceCounter(&launcher_qpc);
        const auto launcher_path = al::module_path();
        al::Handle launcher_file(CreateFileW(launcher_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!launcher_file) fail(L"Open launcher path");
        const auto launcher_real = al::final_path(launcher_file.get());
        if (launcher_real.empty()) fail(L"Resolve launcher file's real path");
        const auto own_folder = al::join_path(al::directory_of(launcher_real), L"AsteraLoader");
        if (!CreateDirectoryW(own_folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            fail(L"Create Astera Loader log parent folder");
        }
        const auto log_path = al::join_path(al::join_path(own_folder, L"logs"),
                             L"launcher." + std::to_wstring(GetCurrentProcessId()) + L".log");
        if (!log.open(log_path)) fail(L"Open launcher log");
        log.line(L"Astera Loader launcher " AL_BUILD_VERSION L" commit=" AL_GIT_COMMIT);
        const auto profile = al::environment(L"USERPROFILE");
        int argc = 0;
        auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) fail(L"Parse launcher arguments");
        std::vector<std::wstring> arguments;
        try {
            for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
        } catch (...) {
            LocalFree(argv);
            throw;
        }
        LocalFree(argv);
        std::wstring cwd(32768, L'\0');
        const DWORD length = GetCurrentDirectoryW(static_cast<DWORD>(cwd.size()), cwd.data());
        if (!length || length >= cwd.size()) fail(L"Read current directory");
        cwd.resize(length);
        al::LaunchPlan plan;
        std::wstring error;
        if (!al::plan_launch(arguments, al::directory_of(launcher_path), cwd,
                               al::regular_file, plan, error)) throw Failure{error};
        if (plan.command_line.size() >= 32767) fail(L"Game command line is too long", ERROR_INVALID_PARAMETER);

        // Resolve from the launcher's visible folder, then keep the file open
        // and use its real path for injection. usvfs need not be ready in the child.
        const auto visible_core = al::join_path(al::directory_of(launcher_path), L"AsteraLoader\\AsteraLoader.dll");
        al::Handle core_file(CreateFileW(visible_core.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!core_file) fail(L"Open AsteraLoader\\AsteraLoader.dll beside the launcher");
        const auto core_real = al::final_path(core_file.get());
        const auto core_native = al::final_path(core_file.get(), VOLUME_NAME_NT);
        if (core_real.empty() || core_native.empty()) fail(L"Resolve Astera Loader DLL file's real path");
        const CoreImage image(core_file.get());
        const auto bootstrap_rva = image.bootstrap_rva();
        const auto dll_hash = al::sha256_file(core_real);
        if (dll_hash.empty()) fail(L"Hash Astera Loader DLL", ERROR_READ_FAULT);
        log.line(L"exe=" + al::redact_profile(plan.exe, profile));
        log.line(L"dll=" + al::redact_profile(core_real, profile));
        log.line(L"dll_sha256=" + dll_hash);
        log.line(L"game_path_source=" + plan.source);
        log.line(L"MO2 profile and force-load: record in the run manifest; not inferred here");

        STARTUPINFOW startup{sizeof(STARTUPINFOW)};
        PROCESS_INFORMATION process{};
        LARGE_INTEGER before_create_qpc{};
        QueryPerformanceCounter(&before_create_qpc);
        if (!CreateProcessW(plan.exe.c_str(), plan.command_line.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED, nullptr, plan.working_dir.c_str(), &startup, &process)) {
            fail(L"Create suspended MonsterHunterWorld.exe");
        }
        child.process.reset(process.hProcess);
        child.thread.reset(process.hThread);
        log.line(L"game_pid=" + std::to_wstring(process.dwProcessId));
        USHORT process_machine = 0, native_machine = 0;
        if (!IsWow64Process2(child.process.get(), &process_machine, &native_machine) ||
            process_machine != IMAGE_FILE_MACHINE_UNKNOWN || native_machine != IMAGE_FILE_MACHINE_AMD64) {
            fail(L"Verify child is native x64", ERROR_BAD_EXE_FORMAT);
        }

        // In a fresh suspended process kernel32/kernelbase may not be mapped.
        // A one-argument ntdll query allows Windows to initialize its loader
        // on a helper thread without resuming the primary thread. No code is
        // copied into the process; only RW parameter buffers are allocated.
        OSVERSIONINFOW version{sizeof(OSVERSIONINFOW)};
        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) fail(L"Find ntdll");
        const auto get_version = remote_system_function(child.process.get(), ntdll, "RtlGetVersion");
        if (invoke_remote(child.process.get(), get_version, &version, sizeof(version)) != 0) {
            fail(L"Initialize child's Windows loader", ERROR_DLL_INIT_FAILED);
        }
        log.line(L"windows_loader_initialized; primary_thread_suspended");
        const auto kernel32 = GetModuleHandleW(L"kernel32.dll");
        if (!kernel32) fail(L"Find kernel32");
        const auto load_library = remote_system_function(child.process.get(), kernel32, "LoadLibraryW");
        const auto load_status = invoke_remote(child.process.get(), load_library, core_real.c_str(),
                                               (core_real.size() + 1) * sizeof(wchar_t));
        log.line(L"LoadLibraryW_thread_exit_low32=" + std::to_wstring(load_status));
        // HMODULE is 64 bits; GetExitCodeThread is only 32. Find the mapped
        // image by its physical file path, never by the truncated return value.
        const auto core_base = remote_image(child.process.get(), core_native);
        bool confirmed = false;
        if (core_base) {
            al::StartupRequest request;
            request.launcher_qpc = static_cast<std::uint64_t>(before_create_qpc.QuadPart);
            const auto status = invoke_remote(child.process.get(), core_base + bootstrap_rva, &request, sizeof(request));
            confirmed = status == al::kStartupOk;
            log.line(L"bootstrap_status=" + std::to_wstring(status));
        }
        log.line(confirmed ? L"core_confirmed_before_resume" : L"core_not_confirmed_before_resume");
        const auto previous_count = ResumeThread(child.thread.get());
        if (previous_count == static_cast<DWORD>(-1)) fail(L"Resume game primary thread");
        if (previous_count != 1) fail(L"Unexpected primary-thread suspension count", ERROR_INVALID_STATE);
        child.resumed = true;
        LARGE_INTEGER resumed_qpc{};
        QueryPerformanceCounter(&resumed_qpc);
        log.line(L"launcher_start_qpc=" + std::to_wstring(launcher_qpc.QuadPart));
        log.line(L"before_create_qpc=" + std::to_wstring(before_create_qpc.QuadPart));
        log.line(L"resumed_qpc=" + std::to_wstring(resumed_qpc.QuadPart));
        log.line(L"game_resumed");
        log_exe_hash(log, plan.exe);
        if (!confirmed) {
            notify(log, L"Astera Loader did not confirm startup. The game has been resumed without active Astera Loader.\n"
                        L"See the logs in the launcher's AsteraLoader\\logs folder.", MB_ICONWARNING);
        }
        if (WaitForSingleObject(child.process.get(), INFINITE) != WAIT_OBJECT_0) fail(L"Wait for game exit");
        DWORD exit_code = 0;
        if (!GetExitCodeProcess(child.process.get(), &exit_code)) fail(L"Read game exit code");
        log.line(L"game_exit_code=" + std::to_wstring(exit_code));
        return exit_code;
    } catch (const Failure& failure) {
        child.stop_before_resume();
        notify(log, failure.message + (child.terminated ? L"\nThe suspended child was terminated." : L""),
               MB_ICONERROR);
    } catch (...) {
        child.stop_before_resume();
        notify(log, L"Astera Loader launcher encountered an unexpected error. See AsteraLoader\\logs.", MB_ICONERROR);
    }
    return 1;
}
}  // namespace

#ifdef AL_TEST_NO_DIALOGS
int wmain() { return static_cast<int>(launch()); }
#else
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return static_cast<int>(launch()); }
#endif
