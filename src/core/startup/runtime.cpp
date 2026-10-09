#include "core/startup/runtime.hpp"

#include "common/log_file.hpp"
#include "common/launch_plan.hpp"
#include "common/paths.hpp"
#include "common/windows.hpp"
#include "core/plugins/host.hpp"
#include "core/plugins/guard.hpp"
#include "core/startup/entry_signature.hpp"
#include "core/startup/entry_cache.hpp"
#include "core/startup/protect_trace.hpp"
#include "core/hooks/thread_snapshot.hpp"
#include "core/phases/runtime.hpp"

#include <MinHook.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace al::startup {
namespace {
Handle g_hash_done;
SRWLOCK g_hash_lock = SRWLOCK_INIT;
std::wstring g_hash;
std::wstring g_hash_logs_folder;
Log g_hash_log = nullptr;
std::atomic<std::uint32_t> g_cached_entry{0};

enum class State : unsigned { waiting, resolving, hooked, running, completed, skipped };
using Protect = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD, PDWORD);
using Entry = int(__cdecl*)();

struct Image {
    struct Section { CodeRange range; std::array<char, 9> name{}; };
    const std::uint8_t* base = nullptr;
    std::uint32_t size = 0;
    std::uint32_t entry = 0;
    CodeRange text{};
    CodeRange bind{};
    std::array<Section, 96> sections{};
    WORD section_count = 0;
};

struct Runtime {
    HMODULE self = nullptr;
    Log log = nullptr;
    Image image;
    std::wstring logs_folder;
    std::atomic<State> state{State::waiting};
    std::atomic<bool> observer_enabled{false};
    std::atomic<bool> entry_enabled{false};
    std::atomic<bool> warned{false};
    Protect protect_original = nullptr;
    void* protect_target = nullptr;
    std::array<std::uint8_t, 5> protect_patch{};
    Entry entry_original = nullptr;
    void* entry_target = nullptr;
    std::array<std::uint8_t, 5> entry_patch{};
    Handle resolved;
    ProtectTrace trace;
    std::atomic<std::uint32_t> entry_to_cache{0};
    plugins::Host host;
};
// Once a hook is installed, this state and the DLL are pinned until process
// exit. Plugin API pointers and in-flight API trampolines must stay valid.
Runtime* g_runtime = nullptr;

bool readable(const void* address, std::size_t size) {
    auto cursor = reinterpret_cast<std::uintptr_t>(address);
    const auto end = cursor + size;
    if (end < cursor) return false;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) ||
            region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const auto next = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

Image current_image() {
    Image image;
    image.base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!image.base || !readable(image.base, sizeof(IMAGE_DOS_HEADER))) return {};
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER) || dos->e_lfanew > 0x100000) return {};
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.base + dos->e_lfanew);
    if (!readable(pe, sizeof(*pe)) || pe->Signature != IMAGE_NT_SIGNATURE ||
        pe->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        pe->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        pe->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) || pe->FileHeader.NumberOfSections > 96) return {};
    image.size = pe->OptionalHeader.SizeOfImage;
    image.entry = pe->OptionalHeader.AddressOfEntryPoint;
    const auto* sections = IMAGE_FIRST_SECTION(pe);
    if (!readable(sections, sizeof(*sections) * pe->FileHeader.NumberOfSections)) return {};
    for (WORD i = 0; i < pe->FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        if (section.VirtualAddress >= image.size || section.Misc.VirtualSize > image.size - section.VirtualAddress) return {};
        const CodeRange range{section.VirtualAddress, section.Misc.VirtualSize};
        image.sections[i].range = range;
        std::memcpy(image.sections[i].name.data(), section.Name, 8);
        if (!std::memcmp(section.Name, ".text\0\0\0", 8) && (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) image.text = range;
        if (!std::memcmp(section.Name, ".bind\0\0\0", 8) && (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) image.bind = range;
    }
    image.section_count = pe->FileHeader.NumberOfSections;
    return image;
}

std::vector<CodeRange> readable_code(const Image& image) {
    std::vector<CodeRange> ranges;
    auto cursor = std::uintptr_t{image.text.rva};
    const auto end = cursor + image.text.size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(image.base + cursor, &region, sizeof(region))) break;
        const auto region_end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize -
                                reinterpret_cast<std::uintptr_t>(image.base);
        const auto next = std::min(region_end, end);
        if (next <= cursor) break;
        const auto access = region.Protect & 0xff;
        if (region.State == MEM_COMMIT && !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY)) {
            if (!ranges.empty() && ranges.back().rva + ranges.back().size == cursor) {
                ranges.back().size += static_cast<std::uint32_t>(next - cursor);
            } else {
                ranges.push_back({static_cast<std::uint32_t>(cursor), static_cast<std::uint32_t>(next - cursor)});
            }
        }
        cursor = next;
    }
    return ranges;
}

void write(std::wstring_view text) noexcept {
    try { g_runtime->log(text); } catch (...) { /* Logging cannot stop startup. */ }
}

LONGLONG ticks() noexcept {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

void hook_timing(std::wstring_view operation, LONGLONG begin, hooks::ThreadSnapshotCounts before) noexcept {
    const auto end = ticks();
    const auto after = hooks::thread_snapshot_counts();
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    try {
        if (frequency.QuadPart) write(L"startup_hook_timing operation=" + std::wstring(operation) +
            L" elapsed_us=" + std::to_wstring((end - begin) * 1000000 / frequency.QuadPart) +
            L" process_snapshots=" + std::to_wstring(after.succeeded - before.succeeded) +
            L" toolhelp_fallbacks=" + std::to_wstring((after.attempts - before.attempts) - (after.succeeded - before.succeeded)));
    } catch (...) { /* Diagnostics cannot stop startup. */ }
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), size);
    return result;
}

DWORD WINAPI show_warning(void* parameter) noexcept {
    auto* message = static_cast<std::wstring*>(parameter);
    // Resolve UI lazily: normal startup keeps a KERNEL32-only import table.
    const auto user32 = LoadLibraryExW(L"user32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (user32) {
        using MessageBox = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);
        const auto function = GetProcAddress(user32, "MessageBoxW");
        if (function) std::bit_cast<MessageBox>(function)(nullptr, message->c_str(), L"Astera Loader", 0x30u);
        FreeLibrary(user32);
    }
    delete message;
    return 0;
}

void visible_warning(std::wstring_view message) noexcept {
    try {
        if (g_runtime->warned.exchange(true)) return;
        auto copy = std::make_unique<std::wstring>(message);
        copy->append(L"\nSee astera\\logs for details. The game continues.");
        Handle thread(CreateThread(nullptr, 0, show_warning, copy.get(), 0, nullptr));
        if (thread) copy.release();
    } catch (...) { /* The logged status remains available for the future menu. */ }
}

void disable_observer() noexcept {
    g_runtime->trace.stop();
    if (g_runtime->observer_enabled.exchange(false)) {
        if (!readable(g_runtime->protect_target, g_runtime->protect_patch.size()) ||
            std::memcmp(g_runtime->protect_target, g_runtime->protect_patch.data(), g_runtime->protect_patch.size())) {
            write(L"warning: startup API observer changed by another hook; its code was left intact");
            return;
        }
        const auto before = hooks::thread_snapshot_counts();
        const auto begin = ticks();
        const auto result = MH_DisableHook(g_runtime->protect_target);
        hook_timing(L"observer_disable", begin, before);
        if (result != MH_OK && result != MH_ERROR_DISABLED) {
            write(L"warning: could not remove startup API observer");
        } else {
            write(L"startup_api_observer_removed");
        }
        // Keep the trampoline allocated: another thread may be finishing a
        // previously entered observer. Restored API code has no ongoing cost.
    }
}

bool disable_entry() noexcept {
    if (!g_runtime->entry_enabled.load()) return true;
    if (!readable(g_runtime->entry_target, g_runtime->entry_patch.size()) ||
        std::memcmp(g_runtime->entry_target, g_runtime->entry_patch.data(), g_runtime->entry_patch.size())) {
        write(L"warning: CRT entry changed by another hook; its code was left intact");
        return false;
    }
    const auto before = hooks::thread_snapshot_counts();
    const auto begin = ticks();
    const auto result = MH_DisableHook(g_runtime->entry_target);
    hook_timing(L"crt_entry_disable", begin, before);
    const auto restored = result == MH_OK || result == MH_ERROR_DISABLED;
    if (restored) g_runtime->entry_enabled.store(false);
    return restored;
}

bool restore_startup_hooks() noexcept {
    g_runtime->trace.stop();
    // Normal entry removes both verified patches under one MinHook freeze.
    // Conflicts and partial failures retain the existing per-hook recovery.
    if (g_runtime->observer_enabled.load() && g_runtime->entry_enabled.load() &&
        readable(g_runtime->protect_target, g_runtime->protect_patch.size()) &&
        !std::memcmp(g_runtime->protect_target, g_runtime->protect_patch.data(), g_runtime->protect_patch.size()) &&
        readable(g_runtime->entry_target, g_runtime->entry_patch.size()) &&
        !std::memcmp(g_runtime->entry_target, g_runtime->entry_patch.data(), g_runtime->entry_patch.size())) {
        const auto before = hooks::thread_snapshot_counts();
        const auto begin = ticks();
        auto result = MH_QueueDisableHook(g_runtime->protect_target);
        if (result == MH_OK) result = MH_QueueDisableHook(g_runtime->entry_target);
        if (result == MH_OK) result = MH_ApplyQueued();
        hook_timing(L"startup_restore_batch", begin, before);
        if (result == MH_OK) {
            g_runtime->observer_enabled.store(false);
            g_runtime->entry_enabled.store(false);
            write(L"startup_api_observer_removed");
            return true;
        }
        write(L"warning: batched startup hook removal failed; attempting individual removal");
    }
    disable_observer();
    return disable_entry();
}

void report_plugins() {
    unsigned refused = 0;
    for (const auto& plugin : g_runtime->host.report()) {
        const auto status = plugin.status == AL_STATUS_ACTIVE ? L"active" :
                            plugin.status == AL_STATUS_DEGRADED ? L"degraded" : L"disabled";
        const auto ms = [](std::uint64_t us) {
            return std::to_wstring(us / 1000) + L"." + std::to_wstring(us / 100 % 10);
        };
        write(L"plugin name=" + plugin.name + L" version=" + widen(plugin.version.empty() ? "unreported" : plugin.version) +
              L" status=" + status + L" load_ms=" + ms(plugin.load_us) + L" init_ms=" + ms(plugin.init_us) +
              L" pre_main_ms=" + ms(plugin.phase_us[AL_PHASE_PRE_MAIN]) + L" reason=" + widen(plugin.reason));
        if (plugin.status != AL_STATUS_ACTIVE) ++refused;
    }
    if (refused) visible_warning(L"Astera Loader reports " + std::to_wstring(refused) + L" plugin(s) with warnings or disabled features.");
}

int __cdecl crt_entry() noexcept {
    auto expected = State::hooked;
    if (!g_runtime->state.compare_exchange_strong(expected, State::running)) return g_runtime->entry_original();
    SetEvent(g_runtime->resolved.get());
    // Restore the startup wrapper before executing it. Do not touch the
    // cookie initializer, common-main body, or any SteamStub instruction.
    const auto restored = restore_startup_hooks();
    try {
        if (!restored) throw 0;
        write(L"phase_pre_main_enter thread_id=" + std::to_wstring(GetCurrentThreadId()));
        LARGE_INTEGER hash_start{}, hash_end{}, frequency{};
        QueryPerformanceCounter(&hash_start);
        QueryPerformanceFrequency(&frequency);
        std::wstring hash;
        if (g_hash_done && WaitForSingleObject(g_hash_done.get(), 15000) == WAIT_OBJECT_0) {
            AcquireSRWLockShared(&g_hash_lock);
            try { hash = g_hash; } catch (...) { ReleaseSRWLockShared(&g_hash_lock); throw; }
            ReleaseSRWLockShared(&g_hash_lock);
        } else {
            write(L"warning: EXE hash unavailable at PRE_MAIN; hash-restricted plugins will be skipped");
        }
        QueryPerformanceCounter(&hash_end);
        if (frequency.QuadPart) write(L"phase_pre_main_hash_wait_us=" + std::to_wstring(
            (hash_end.QuadPart - hash_start.QuadPart) * 1000000 / frequency.QuadPart));
        write(hash.empty() ? L"plugin_exe_sha256=unavailable" : L"plugin_exe_sha256=" + hash);
        const auto scanned_rva = g_runtime->entry_to_cache.exchange(0);
        if (scanned_rva && !hash.empty()) {
            try {
                const auto file = crt_cache_file(g_runtime->logs_folder, hash);
                write(append_crt_cache(file, scanned_rva)
                    ? L"startup_crt_cache_written rva=" + std::to_wstring(scanned_rva)
                    : L"startup_crt_cache_write=unavailable; next launch will scan");
            } catch (...) { write(L"startup_crt_cache_write=unavailable; next launch will scan"); }
        }
        const auto root = join_path(directory_of(module_path()), L"astera\\plugins");
        write(L"plugin_discovery_root=" + redact_profile(root, environment(L"USERPROFILE")));
        g_runtime->host.load_all({root, g_runtime->logs_folder, hash});
        report_plugins();
        g_runtime->host.dispatch(AL_PHASE_PRE_MAIN, nullptr);
        report_plugins();
        // A crash in the previous session (AL's crash handler), told once.
        for (const auto& notice : g_runtime->host.notices()) {
            write(L"previous_session_crash: " + redact_profile(notice, environment(L"USERPROFILE")));
            visible_warning(notice);
        }
        write(L"phase_pre_main_complete; startup wrapper restored");
        phases::arm(g_runtime->host, g_runtime->log, visible_warning, g_runtime->entry_target);
        g_runtime->state.store(State::completed);
    } catch (...) {
        g_runtime->state.store(State::skipped);
        write(L"phase_pre_main_failed: unexpected plugin-host error; game continues");
        visible_warning(L"Astera Loader could not complete its PRE_MAIN plugin phase.");
    }
    // If restoration failed, use MinHook's retained original trampoline.
    return !restored ? g_runtime->entry_original() :
                                           std::bit_cast<Entry>(g_runtime->entry_target)();
}

bool install_entry(const CrtEntry& candidate, bool full_scan = true) {
    auto* target = const_cast<std::uint8_t*>(g_runtime->image.base + candidate.entry);
    void* original = nullptr;
    auto result = MH_CreateHook(target, std::bit_cast<void*>(&crt_entry), &original);
    if (result != MH_OK) {
        write(L"phase_pre_main_hook_failed: " + widen(MH_StatusToString(result)));
        return false;
    }
    g_runtime->entry_target = target;
    g_runtime->entry_original = std::bit_cast<Entry>(original);
    const auto before = hooks::thread_snapshot_counts();
    const auto begin = ticks();
    result = MH_EnableHook(target);
    hook_timing(L"crt_entry_enable", begin, before);
    if (result != MH_OK) {
        MH_RemoveHook(target);
        write(L"phase_pre_main_hook_failed: " + widen(MH_StatusToString(result)));
        return false;
    }
    g_runtime->entry_enabled.store(true);
    std::memcpy(g_runtime->entry_patch.data(), target, g_runtime->entry_patch.size());
    if (full_scan) g_runtime->entry_to_cache.store(candidate.entry);
    g_runtime->state.store(State::hooked);
    write(L"phase_pre_main_hook_ready entry_rva=" + std::to_wstring(candidate.entry) +
          L" cookie_rva=" + std::to_wstring(candidate.cookie_initialiser) +
          L" common_main_rva=" + std::to_wstring(candidate.common_main));
    return true;
}

std::optional<CrtEntry> find_entry(std::string& reason) {
    const auto ranges = readable_code(g_runtime->image);
    return find_crt_entry({g_runtime->image.base, g_runtime->image.size}, ranges, reason);
}

bool scan_entry(CrtEntry* candidate, std::string* reason) {
    const auto found = find_entry(*reason);
    if (found) *candidate = *found;
    return found.has_value();
}

bool guarded_scan(CrtEntry* candidate, std::string* reason) {
    __try { return scan_entry(candidate, reason); }
    __except (plugins::recoverable_fault(GetExceptionCode())) { return false; }
}

bool verify_entry(CrtEntry* candidate, std::uint32_t rva, std::string* reason) {
    const auto ranges = readable_code(g_runtime->image);
    const auto found = verify_crt_entry({g_runtime->image.base, g_runtime->image.size}, ranges, rva, *reason);
    if (found) *candidate = *found;
    return found.has_value();
}

bool guarded_verify(CrtEntry* candidate, std::uint32_t rva, std::string* reason) {
    __try { return verify_entry(candidate, rva, reason); }
    __except (plugins::recoverable_fault(GetExceptionCode())) { return false; }
}

bool read_old_protection(PDWORD source, std::uint32_t* value) noexcept {
    __try {
        if (!source) return false;
        *value = *source;
        return true;
    } __except (plugins::recoverable_fault(GetExceptionCode())) { return false; }
}

__declspec(noinline) BOOL WINAPI protect_observer(LPVOID address, SIZE_T size, DWORD protection, PDWORD old) noexcept {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto incoming_error = GetLastError();
    const auto ticket = g_runtime->trace.claim();
    LARGE_INTEGER begin{};
    if (ticket < ProtectTrace::limit) QueryPerformanceCounter(&begin);
    SetLastError(incoming_error);
    const BOOL result = g_runtime->protect_original(address, size, protection, old);
    const DWORD last_error = GetLastError();
    const auto base = reinterpret_cast<std::uintptr_t>(g_runtime->image.base);
    const auto& bind = g_runtime->image.bind;
    const auto& text = g_runtime->image.text;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto access = protection & 0xff;
    const bool executable_read = !(protection & (PAGE_GUARD | PAGE_NOACCESS)) &&
                                 (access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE);
    const bool from_bind = caller >= base + bind.rva && caller - (base + bind.rva) < bind.size;
    const bool covers_text = start <= base + text.rva && size >= (base + text.rva - start) + text.size;
    if (ticket < ProtectTrace::limit) {
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        ProtectCall call;
        call.begin_qpc = begin.QuadPart;
        call.end_qpc = end.QuadPart;
        call.caller = caller;
        call.address = start;
        call.size = size;
        call.thread = GetCurrentThreadId();
        call.protection = protection;
        // A failed call need not initialize its output. Never read it then.
        call.old_available = result && read_old_protection(old, &call.old_protection);
        call.last_error = last_error;
        call.state = static_cast<unsigned>(g_runtime->state.load());
        call.success = result != FALSE;
        call.from_bind = from_bind;
        call.covers_text = covers_text;
        call.executable_read = executable_read;
        g_runtime->trace.publish(ticket, call);
    }
    // Run pre-main-2 restored .text from a temporary private allocation, not
    // .bind. Caller identity is diagnostic only: the complete executable range
    // and unique CRT wrapper/cookie/common-main signatures verify readiness.
    if (result && executable_read && covers_text) {
        auto expected = State::waiting;
        if (g_runtime->state.compare_exchange_strong(expected, State::resolving)) {
            try {
                std::string reason;
                CrtEntry candidate{};
                const auto scan_begin = ticks();
                const auto cached_rva = g_cached_entry.load(std::memory_order_acquire);
                std::string cache_reason;
                const bool from_cache = cached_rva && guarded_verify(&candidate, cached_rva, &cache_reason);
                const bool found = from_cache || guarded_scan(&candidate, &reason);
                const auto scan_end = ticks();
                LARGE_INTEGER frequency{};
                QueryPerformanceFrequency(&frequency);
                if (frequency.QuadPart) write(L"startup_signature_scan_us=" + std::to_wstring(
                    (scan_end - scan_begin) * 1000000 / frequency.QuadPart));
                write(from_cache ? L"startup_signature_source=cache" : L"startup_signature_source=scan");
                if (cached_rva && !from_cache) write(L"startup_signature_cache_rejected: " + widen(cache_reason));
                if (found && install_entry(candidate, !from_cache)) {
                    write(L"unpacked_crt_verified_after_protection_change");
                } else {
                    write(L"phase_pre_main_pending: " + widen(reason));
                    g_runtime->state.store(State::waiting);
                }
            } catch (...) {
                write(L"phase_pre_main_pending: signature scan failed");
                g_runtime->state.store(State::waiting);
            }
        }
    }
    // Preserve VirtualProtect's return value, output parameter and last error.
    SetLastError(last_error);
    return result;
}

std::wstring hex(std::uint64_t value) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

// Non-main modules may unload before the worker reports a call. Their section
// metadata is best effort, guarded against that race; recorded addresses remain.
bool module_section(HMODULE module, std::uintptr_t address, char* name) noexcept {
    __try {
        const auto* base = reinterpret_cast<const std::uint8_t*>(module);
        if (!readable(base, sizeof(IMAGE_DOS_HEADER))) return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(*dos) || dos->e_lfanew > 0x100000) return false;
        const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (!readable(pe, sizeof(*pe)) || pe->Signature != IMAGE_NT_SIGNATURE || pe->FileHeader.NumberOfSections > 96) return false;
        const auto* sections = IMAGE_FIRST_SECTION(pe);
        if (!readable(sections, sizeof(*sections) * pe->FileHeader.NumberOfSections)) return false;
        const auto rva = address - reinterpret_cast<std::uintptr_t>(base);
        for (WORD i = 0; i < pe->FileHeader.NumberOfSections; ++i) {
            if (rva >= sections[i].VirtualAddress && rva - sections[i].VirtualAddress < sections[i].Misc.VirtualSize) {
                std::memcpy(name, sections[i].Name, 8);
                name[8] = 0;
                return true;
            }
        }
    } __except (plugins::recoverable_fault(GetExceptionCode())) { return false; }
    return false;
}

std::wstring location(std::uintptr_t address) {
    const auto& image = g_runtime->image;
    const auto base = reinterpret_cast<std::uintptr_t>(image.base);
    if (address >= base && address - base < image.size) {
        const auto rva = address - base;
        std::string section = "headers-or-gap";
        for (WORD i = 0; i < image.section_count; ++i) {
            const auto& item = image.sections[i];
            if (rva >= item.range.rva && rva - item.range.rva < item.range.size) section = item.name.data();
        }
        return L"MonsterHunterWorld.exe section=" + widen(section) + L" rva=" + hex(rva);
    }
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) || region.State == MEM_FREE) return L"unmapped";
    const auto allocation = reinterpret_cast<std::uintptr_t>(region.AllocationBase);
    if (region.Type != MEM_IMAGE) return L"non-image allocation=" + hex(allocation) + L" offset=" + hex(address - allocation);
    const auto module = static_cast<HMODULE>(region.AllocationBase);
    const auto path = module_path(module);
    char section[9]{};
    module_section(module, address, section);
    return std::wstring(path.empty() ? L"unavailable-module" : file_name_of(path)) +
        L" section=" + (section[0] ? widen(section) : L"unavailable") + L" rva=" + hex(address - allocation);
}

void drain_trace(std::array<bool, ProtectTrace::limit>& reported, std::size_t& count) noexcept {
    for (std::size_t i = 0; i < ProtectTrace::limit; ++i) {
        ProtectCall call;
        if (reported[i] || !g_runtime->trace.read(i, call)) continue;
        reported[i] = true;
        ++count;
        try {
            write(L"startup_vprotect_call index=" + std::to_wstring(i) + L" begin_qpc=" + std::to_wstring(call.begin_qpc) +
                L" end_qpc=" + std::to_wstring(call.end_qpc) + L" thread=" + std::to_wstring(call.thread) +
                L" caller=" + hex(call.caller) + L" caller_location=[" + location(call.caller) +
                L"] target=" + hex(call.address) + L" target_location=[" + location(call.address) +
                L"] size=" + hex(call.size) + L" new=" + hex(call.protection) +
                L" old=" + (call.old_available ? hex(call.old_protection) : L"unavailable") +
                L" result=" + std::to_wstring(call.success) + L" last_error=" + std::to_wstring(call.last_error) +
                L" state=" + std::to_wstring(call.state) + L" from_bind=" + std::to_wstring(call.from_bind) +
                L" covers_text=" + std::to_wstring(call.covers_text) + L" executable_read=" + std::to_wstring(call.executable_read));
        } catch (...) { write(L"warning: startup VirtualProtect record formatting failed"); }
    }
}

DWORD WINAPI deadline(void*) noexcept {
    std::array<bool, ProtectTrace::limit> reported{};
    std::size_t count = 0;
    const auto until = GetTickCount64() + 15000;
    DWORD wait = WAIT_TIMEOUT;
    for (;;) {
        const auto now = GetTickCount64();
        if (now >= until) break;
        const auto remaining = until - now;
        wait = WaitForSingleObject(g_runtime->resolved.get(), static_cast<DWORD>(std::min<ULONGLONG>(remaining, 250)));
        drain_trace(reported, count);
        if (wait != WAIT_TIMEOUT) break;
    }
    if (wait == WAIT_TIMEOUT) {
        auto expected = State::waiting;
        bool skipped = g_runtime->state.compare_exchange_strong(expected, State::skipped);
        if (!skipped) {
            expected = State::hooked;
            skipped = g_runtime->state.compare_exchange_strong(expected, State::skipped);
        }
        if (skipped) {
            disable_observer();
            disable_entry();
            write(L"phase_pre_main_skipped: no verified post-unpack startup transition within 15 seconds");
            visible_warning(L"Astera Loader did not find a verified post-unpack startup point. Plugins were skipped.");
        }
    }
    g_runtime->trace.stop();
    // Give calls already inside the API a bounded chance to publish. Never
    // wait on them from the game thread, and expose missing records explicitly.
    Sleep(20);
    drain_trace(reported, count);
    try {
        const auto total = g_runtime->trace.total();
        const auto retained = std::min<std::uint64_t>(total, ProtectTrace::limit);
        write(L"startup_vprotect_summary total=" + std::to_wstring(total) + L" recorded=" + std::to_wstring(count) +
              L" limit=" + std::to_wstring(ProtectTrace::limit) + L" truncated=" + std::to_wstring(total - retained) +
              L" pending=" + std::to_wstring(retained - count) + L" state_ids=0:waiting,1:resolving,2:hooked,3:running,4:completed,5:skipped");
    } catch (...) { write(L"warning: startup VirtualProtect summary unavailable"); }
    return 0;
}

bool clean_system_export(void* target) {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(target, &region, sizeof(region)) || region.Type != MEM_IMAGE ||
        region.State != MEM_COMMIT || !readable(target, 16)) return false;
    const auto owner = module_path(static_cast<HMODULE>(region.AllocationBase));
    const auto name = file_name_of(owner);
    if (!iequals(name, L"kernelbase.dll") && !iequals(name, L"kernel32.dll")) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(target);
    // Refuse pre-existing inline detours instead of stacking another patch.
    return bytes[0] != 0xe9 && bytes[0] != 0xeb && !(bytes[0] == 0xff && bytes[1] == 0x25) &&
           !(bytes[0] == 0x48 && bytes[1] == 0xb8) && !(bytes[0] == 0x49 && bytes[1] == 0xbb);
}
}  // namespace

void initialise_hash_result(std::wstring_view logs_folder, Log log) {
    g_hash_logs_folder = logs_folder;
    g_hash_log = log;
    g_cached_entry.store(0);
    g_hash_done.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
}

void publish_hash_result(std::wstring hash) noexcept {
    std::wstring cache_file;
    try { cache_file = crt_cache_file(g_hash_logs_folder, hash); } catch (...) {}
    AcquireSRWLockExclusive(&g_hash_lock);
    g_hash.swap(hash);
    ReleaseSRWLockExclusive(&g_hash_lock);
    if (g_hash_done) SetEvent(g_hash_done.get());
    // Hash consumers are released before optional file I/O. A late cache read
    // simply loses this launch's fast path; the detour never waits for it.
    std::uint32_t rva = 0;
    try { rva = read_crt_cache(cache_file); } catch (...) {}
    g_cached_entry.store(rva, std::memory_order_release);
    try { if (g_hash_log) g_hash_log(L"startup_crt_cache_ready rva=" + std::to_wstring(rva)); } catch (...) {}
}

void arm_pre_main(HMODULE self, const std::wstring& logs_folder, Log log) noexcept {
    // Core ABI tests load the DLL into an ordinary test process and must retain
    // their explicit unload behavior. Production only operates in the game exe.
    try {
        if (!iequals(file_name_of(module_path()), kGameExeName)) {
            log(L"phase_pre_main_not_armed: process is not MonsterHunterWorld.exe");
            return;
        }
        auto* runtime = new Runtime;
        runtime->self = self;
        runtime->log = log;
        runtime->logs_folder = logs_folder;
        runtime->image = current_image();
        runtime->resolved.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        g_runtime = runtime;
        write(L"bootstrap_stage=image_metadata_ready image_base=" + hex(reinterpret_cast<std::uintptr_t>(runtime->image.base)) +
              L" entry_rva=" + hex(runtime->image.entry) + L" text_rva=" + hex(runtime->image.text.rva) +
              L" text_size=" + hex(runtime->image.text.size) + L" bind_rva=" + hex(runtime->image.bind.rva) +
              L" bind_size=" + hex(runtime->image.bind.size));
        if (!runtime->image.size || !runtime->image.text.size || !runtime->resolved) {
            write(L"phase_pre_main_skipped: executable metadata or event unavailable");
            visible_warning(L"Astera Loader could not verify this executable's startup layout.");
            return;
        }
        if (MH_Initialize() != MH_OK) {
            write(L"phase_pre_main_skipped: MinHook initialization failed");
            visible_warning(L"Astera Loader could not initialize its startup hook.");
            return;
        }
        write(L"bootstrap_stage=minhook_initialized");
        // Pin before any hook can reference our code, including warning threads.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&arm_pre_main), &pinned)) {
            write(L"phase_pre_main_skipped: core lifetime pin failed");
            return;
        }
        write(L"bootstrap_stage=core_pinned");
        const bool packed = runtime->image.entry >= runtime->image.bind.rva &&
            runtime->image.entry - runtime->image.bind.rva < runtime->image.bind.size;
        if (!packed) {
            std::string reason;
            const auto candidate = find_entry(reason);
            if (candidate && candidate->entry == runtime->image.entry && install_entry(*candidate)) return;
            write(L"phase_pre_main_skipped: " + widen(reason));
            visible_warning(L"Astera Loader could not verify this executable's CRT entry point.");
            return;
        }
        const auto kernel32 = GetModuleHandleW(L"kernel32.dll");
        runtime->protect_target = std::bit_cast<void*>(GetProcAddress(kernel32, "VirtualProtect"));
        if (!runtime->protect_target || !clean_system_export(runtime->protect_target)) {
            write(L"phase_pre_main_skipped: VirtualProtect is unavailable or already hooked");
            visible_warning(L"Astera Loader could not install its startup observer without a hook conflict.");
            return;
        }
        write(L"bootstrap_stage=observer_target_verified target=" + hex(reinterpret_cast<std::uintptr_t>(runtime->protect_target)) +
              L" location=[" + location(reinterpret_cast<std::uintptr_t>(runtime->protect_target)) + L"]");
        void* original = nullptr;
        auto result = MH_CreateHook(runtime->protect_target, std::bit_cast<void*>(&protect_observer), &original);
        if (result == MH_OK) {
            runtime->protect_original = std::bit_cast<Protect>(original);
            write(L"bootstrap_stage=observer_hook_created");
            const auto before = hooks::thread_snapshot_counts();
            const auto begin = ticks();
            result = MH_EnableHook(runtime->protect_target);
            hook_timing(L"observer_enable", begin, before);
        }
        if (result != MH_OK) {
            write(L"phase_pre_main_skipped: " + widen(MH_StatusToString(result)));
            visible_warning(L"Astera Loader could not install its post-unpack observer.");
            return;
        }
        runtime->observer_enabled.store(true);
        std::memcpy(runtime->protect_patch.data(), runtime->protect_target, runtime->protect_patch.size());
        runtime->trace.start();
        write(L"phase_pre_main_observer_armed; security cookie and SteamStub unchanged");
        Handle thread(CreateThread(nullptr, 0, deadline, nullptr, 0, nullptr));
        if (!thread) {
            runtime->state.store(State::skipped);
            disable_observer();
            write(L"phase_pre_main_skipped: observer deadline thread unavailable");
            visible_warning(L"Astera Loader could not start its startup observer deadline.");
        }
        write(L"bootstrap_stage=observer_deadline_ready");
    } catch (...) {
        if (g_runtime) {
            g_runtime->state.store(State::skipped);
            disable_observer();
            disable_entry();
            visible_warning(L"Astera Loader could not prepare its startup observer. Plugins were skipped.");
        }
        try { log(L"phase_pre_main_skipped: startup observer preparation failed"); } catch (...) {}
    }
}
}  // namespace al::startup
