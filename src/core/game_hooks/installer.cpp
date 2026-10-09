#include "core/game_hooks/installer.hpp"
#include "common/memory.hpp"
#include "common/windows.hpp"
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace cmfm::game_hooks {
namespace {
bool executable(const void* pointer) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(pointer, &info, sizeof(info)) && info.State == MEM_COMMIT &&
        !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}
std::string address(const void* pointer) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pointer)));
    return text;
}
bool copy(const void* source, void* dest, std::size_t count) noexcept {
    __try { std::memcpy(dest, source, count); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* jump_destination(void* target) noexcept {
    std::uint8_t bytes[16]{};
    if (!readable(target, sizeof(bytes)) || !copy(target, bytes, sizeof(bytes))) return nullptr;
    const auto at = reinterpret_cast<std::uintptr_t>(target);
    if (bytes[0] == 0xe9) {
        std::int32_t delta{}; std::memcpy(&delta, bytes + 1, 4); return reinterpret_cast<void*>(at + 5 + delta);
    }
    if (bytes[0] == 0xeb) return reinterpret_cast<void*>(at + 2 + static_cast<std::int8_t>(bytes[1]));
    if (bytes[0] == 0xff && bytes[1] == 0x25) {
        std::int32_t delta{}; std::memcpy(&delta, bytes + 2, 4);
        void* destination{};
        const auto slot = reinterpret_cast<const void*>(at + 6 + delta);
        return readable(slot, sizeof(destination)) && copy(slot, &destination, sizeof(destination)) ? destination : nullptr;
    }
    if (bytes[0] == 0x48 && bytes[1] == 0xb8 && bytes[10] == 0xff && bytes[11] == 0xe0) {
        void* destination{}; std::memcpy(&destination, bytes + 2, 8); return destination;
    }
    return nullptr;
}
std::string owner(void* target) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(target, &info, sizeof(info)) || info.Type != MEM_IMAGE) return "private-code";
    auto path = module_path(static_cast<HMODULE>(info.AllocationBase));
    const auto slash = path.find_last_of(L"\\/");
    const auto name = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
    const auto length = WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(length), '\0');
    if (length) WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), text.data(), length, nullptr, nullptr);
    return text;
}
void* entry_stub(void* const* head) noexcept {
    // mov r11, imm64(head); jmp qword ptr [r11]. No calls, stack adjustment,
    // flags changes, TLS access, or writes to the argument registers.
    std::uint8_t bytes[]{0x49, 0xbb, 0, 0, 0, 0, 0, 0, 0, 0, 0x41, 0xff, 0x23};
    std::memcpy(bytes + 2, &head, sizeof(head));
    const auto memory = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory) return nullptr;
    std::memcpy(memory, bytes, sizeof(bytes));
    DWORD old{};
    if (!VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &old) || !FlushInstructionCache(GetCurrentProcess(), memory, sizeof(bytes))) {
        VirtualFree(memory, 0, MEM_RELEASE); return nullptr;
    }
    return memory;
}
std::string failure(std::string_view operation, MH_STATUS status) {
    if (status == MH_ERROR_UNSUPPORTED_FUNCTION)
        return "MinHook cannot relocate this function's entry instructions (MH_ERROR_UNSUPPORTED_FUNCTION)";
    return "MinHook " + std::string(operation) + ": " + MH_StatusToString(status);
}
}
struct NativeInstaller::State {
    struct Entry { void* target; void* stub; void* const* head; };
    Log log;
    std::vector<Entry> entries;
    void note(std::string_view text) noexcept { try { if (log) log(text); } catch (...) {} }
};
NativeInstaller::NativeInstaller(Log log) : state_(new State) { state_->log = std::move(log); }
std::string NativeInstaller::create(void* target, void* const* head, void** original) {
    try {
        if (!target || !executable(target) || !readable(target, 32)) return "target entry is not readable executable memory";
        if (!head || !original || reinterpret_cast<std::uintptr_t>(head) % alignof(void*) || !readable(head, sizeof(*head)))
            return "missing or invalid head/original storage";
        if (std::any_of(state_->entries.begin(), state_->entries.end(), [&](const auto& e) { return e.target == target; }))
            return "this target already has a CMFM game hook";
        if (auto destination = jump_destination(target)) {
            std::array<void*, 9> seen{target}; unsigned count = 1;
            while (count < seen.size() && std::find(seen.begin(), seen.begin() + count, destination) == seen.begin() + count) {
                seen[count++] = destination;
                const auto next = jump_destination(destination);
                if (!next) break;
                destination = next;
            }
            state_->note("game_hook_existing_patch target=" + address(target) + " destination=" + address(destination) + " module=" + owner(destination));
        }
        // Reserve before creating a hook: successful records must survive
        // even if a later allocation or activation fails.
        state_->entries.reserve(state_->entries.size() + 1);
        const auto stub = entry_stub(head);
        if (!stub) return "cannot allocate the game-hook entry stub";
        void* trampoline{};
        const auto result = MH_CreateHook(target, stub, &trampoline);
        if (result != MH_OK) { VirtualFree(stub, 0, MEM_RELEASE); return failure("create", result); }
        state_->entries.push_back({target, stub, head});
        *original = trampoline;
        return {};
    } catch (...) { return "game-hook creation failed"; }
}
std::string NativeInstaller::enable(void* target) {
    const auto found = std::find_if(state_->entries.begin(), state_->entries.end(), [&](const auto& e) { return e.target == target; });
    if (found == state_->entries.end()) return "no inactive CMFM game hook for this target";
    void* head{};
    if (!copy(found->head, &head, sizeof(head)) || !head || !executable(head)) return "the registry has not published an executable chain head";
    const auto result = MH_EnableHook(target);
    return result == MH_OK || result == MH_ERROR_ENABLED ? std::string() : failure("enable", result);
}
}  // namespace cmfm::game_hooks
