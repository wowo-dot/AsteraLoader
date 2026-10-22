#include "core/patch/writer.hpp"

#include "core/hooks/near_memory.hpp"
#include "core/hooks/thread_snapshot.hpp"

#include <windows.h>
#include <intrin.h>
#include <tlhelp32.h>

#include <cstring>

namespace al::patch {
namespace {

constexpr int kAttempts = 50;  // tries while a paused thread is inside the bytes, 1 ms apart
constexpr std::intptr_t kReach = 0x7FFF0000;  // a rel32 reaches +-2 GiB; keep a margin

bool writable_protection(DWORD protect) {
    protect &= 0xFF;
    return protect == PAGE_READWRITE || protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READWRITE ||
           protect == PAGE_EXECUTE_WRITECOPY;
}

bool executable_protection(DWORD protect) {
    protect &= 0xFF;
    return protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE ||
           protect == PAGE_EXECUTE_WRITECOPY;
}

// Write access to at most two pages for one write, and the old protection
// back afterwards. No allocation: it runs while other threads are paused.
class Unprotect {
public:
    Unprotect(std::uint8_t* at, std::size_t size) {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        const auto page = static_cast<std::uintptr_t>(info.dwPageSize);
        const auto first = reinterpret_cast<std::uintptr_t>(at) & ~(page - 1);
        const auto last = (reinterpret_cast<std::uintptr_t>(at) + size - 1) & ~(page - 1);
        for (auto address = first; address <= last && count_ < 2; address += page) {
            MEMORY_BASIC_INFORMATION memory{};
            if (!VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory))) {
                error_ = GetLastError();
                return;
            }
            if (writable_protection(memory.Protect)) continue;
            const DWORD wanted = executable_protection(memory.Protect) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
            DWORD old = 0;
            if (!VirtualProtect(reinterpret_cast<void*>(address), 1, wanted, &old)) {
                error_ = GetLastError();
                return;
            }
            pages_[count_] = address;
            old_[count_] = old;
            ++count_;
        }
    }
    ~Unprotect() {
        for (int k = 0; k < count_; ++k) {
            DWORD ignored = 0;
            VirtualProtect(reinterpret_cast<void*>(pages_[k]), 1, old_[k], &ignored);
        }
    }
    Unprotect(const Unprotect&) = delete;
    Unprotect& operator=(const Unprotect&) = delete;
    DWORD error() const { return error_; }

private:
    std::uintptr_t pages_[2]{};
    DWORD old_[2]{};
    int count_ = 0;
    DWORD error_ = 0;
};

// The process's other threads, opened before any is paused.
class Threads {
public:
    Threads() {
        LPDWORD ids = nullptr;
        UINT count = 0;
        if (al_enumerate_process_threads(GetProcessHeap(), &ids, &count)) {
            for (UINT k = 0; k < count; ++k) open(ids[k]);
            if (ids) HeapFree(GetProcessHeap(), 0, ids);
            return;
        }
        // The slower system-wide list, as MinHook falls back to.
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            failed_ = true;
            return;
        }
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        const auto process = GetCurrentProcessId();
        const auto self = GetCurrentThreadId();
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self) open(entry.th32ThreadID);
        }
        CloseHandle(snapshot);
    }
    ~Threads() {
        resume();
        for (const auto handle : handles_) CloseHandle(handle);
    }
    Threads(const Threads&) = delete;
    Threads& operator=(const Threads&) = delete;
    bool failed() const { return failed_; }

    // Pauses every thread; true if none is stopped inside (begin, end). Only
    // system calls from here to resume(): a paused thread may hold the heap.
    bool pause_outside(std::uintptr_t begin, std::uintptr_t end) {
        bool clear = true;
        for (std::size_t k = 0; k < handles_.size(); ++k) {
            paused_[k] = SuspendThread(handles_[k]) != static_cast<DWORD>(-1);
            if (!paused_[k]) continue;
            // Waits for the pause to take effect, and gives the thread's place.
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(handles_[k], &context) && context.Rip > begin && context.Rip < end) clear = false;
        }
        return clear;
    }
    void resume() {
        for (std::size_t k = 0; k < handles_.size(); ++k) {
            if (paused_[k]) ResumeThread(handles_[k]);
            paused_[k] = false;
        }
    }

private:
    void open(DWORD id) {
        const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                         FALSE, id);
        if (!handle) return;  // it has exited
        handles_.push_back(handle);
        paused_.push_back(false);
    }
    std::vector<HANDLE> handles_;
    std::vector<bool> paused_;
    bool failed_ = false;
};

bool compare_exchange(std::uint8_t* at, const std::uint8_t* expect, const std::uint8_t* bytes, std::size_t size,
                      std::uint8_t* found) {
    switch (size) {
    case 1: {
        const auto old = static_cast<char>(*expect);
        const auto seen = _InterlockedCompareExchange8(reinterpret_cast<volatile char*>(at), static_cast<char>(*bytes), old);
        std::memcpy(found, &seen, 1);
        return seen == old;
    }
    case 2: {
        SHORT old = 0, value = 0;
        std::memcpy(&old, expect, 2);
        std::memcpy(&value, bytes, 2);
        const auto seen = InterlockedCompareExchange16(reinterpret_cast<volatile SHORT*>(at), value, old);
        std::memcpy(found, &seen, 2);
        return seen == old;
    }
    case 4: {
        LONG old = 0, value = 0;
        std::memcpy(&old, expect, 4);
        std::memcpy(&value, bytes, 4);
        const auto seen = InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(at), value, old);
        std::memcpy(found, &seen, 4);
        return seen == old;
    }
    default: {
        LONG64 old = 0, value = 0;
        std::memcpy(&old, expect, 8);
        std::memcpy(&value, bytes, 8);
        const auto seen = InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(at), value, old);
        std::memcpy(found, &seen, 8);
        return seen == old;
    }
    }
}

std::string error_text(const char* what, DWORD error) {
    return std::string(what) + " failed, Windows error " + std::to_string(error);
}

}  // namespace

Writer::Result NativeWriter::write(std::uint8_t* at, const std::uint8_t* expect, const std::uint8_t* bytes,
                                   std::size_t size, bool code, std::uint8_t* found, std::string& problem) {
    const auto address = reinterpret_cast<std::uintptr_t>(at);
    // One step, no pause: naturally aligned data that one instruction reads.
    if (!code && (size == 1 || size == 2 || size == 4 || size == 8) && address % size == 0) {
        Unprotect unprotect(at, size);
        if (unprotect.error()) {
            problem = error_text("VirtualProtect", unprotect.error());
            return Result::failed;
        }
        return compare_exchange(at, expect, bytes, size, found) ? Result::written : Result::differs;
    }
    Threads threads;
    if (threads.failed()) {
        problem = "the game's threads could not be listed, so they could not be paused";
        return Result::failed;
    }
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        // A thread exactly at the first byte has not started on them: fine.
        if (!threads.pause_outside(code ? address : 0, code ? address + size : 0)) {
            threads.resume();
            Sleep(1);
            continue;
        }
        if (std::memcmp(at, expect, size) != 0) {
            std::memcpy(found, at, size);
            threads.resume();
            return Result::differs;
        }
        DWORD error = 0;
        {
            Unprotect unprotect(at, size);
            error = unprotect.error();
            if (!error) {
                std::memcpy(at, bytes, size);
                if (code) FlushInstructionCache(GetCurrentProcess(), at, size);
            }
        }
        threads.resume();
        if (error) {
            problem = error_text("VirtualProtect", error);
            return Result::failed;
        }
        return Result::written;
    }
    problem = "a game thread kept running inside the bytes (" + std::to_string(kAttempts) + " tries)";
    return Result::failed;
}

std::string NativeWriter::stub(const void* site, void* function, void** out) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto page = static_cast<std::uintptr_t>(info.dwPageSize);
    const auto granularity = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
    const auto origin = reinterpret_cast<std::uintptr_t>(site) + 5;  // where a rel32 counts from
    const auto in_reach = [&](std::uintptr_t base) {
        const auto distance = static_cast<std::intptr_t>(base - origin);
        return distance > -kReach && distance + static_cast<std::intptr_t>(granularity) < kReach;
    };

    // The reservation next to the game, made at bootstrap, first; then a
    // region of our own with a free page, or a new one.
    const auto near_lowest = origin > static_cast<std::uintptr_t>(kReach) ? origin - kReach + 1 : 0;
    auto* stub = static_cast<std::uint8_t*>(hooks::near_stub_page(near_lowest, origin + kReach - page));
    Region* region = nullptr;
    if (!stub) {
        for (auto& candidate : regions_) {
            if (candidate.pages_used < granularity / page && in_reach(reinterpret_cast<std::uintptr_t>(candidate.base))) {
                region = &candidate;
                break;
            }
        }
    }
    if (!stub && !region) {
        // A free allocation unit within reach, below the exe first (as MinHook
        // looks), then above.
        const auto lowest = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
        const auto highest = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
        const auto floor = origin > static_cast<std::uintptr_t>(kReach) + lowest ? origin - kReach : lowest;
        const auto ceiling = highest - origin > static_cast<std::uintptr_t>(kReach) ? origin + kReach : highest;
        void* reserved = nullptr;
        auto at = (origin & ~(granularity - 1)) - granularity;
        while (!reserved && at >= floor) {
            MEMORY_BASIC_INFORMATION memory{};
            if (!VirtualQuery(reinterpret_cast<void*>(at), &memory, sizeof(memory))) break;
            if (memory.State == MEM_FREE && in_reach(at)) {
                reserved = VirtualAlloc(reinterpret_cast<void*>(at), granularity, MEM_RESERVE, PAGE_NOACCESS);
            }
            const auto base = reinterpret_cast<std::uintptr_t>(memory.AllocationBase ? memory.AllocationBase : memory.BaseAddress);
            if (base < granularity + floor) break;
            at = (base & ~(granularity - 1)) - granularity;
        }
        at = (origin & ~(granularity - 1)) + granularity;
        while (!reserved && at + granularity <= ceiling) {
            MEMORY_BASIC_INFORMATION memory{};
            if (!VirtualQuery(reinterpret_cast<void*>(at), &memory, sizeof(memory))) break;
            if (memory.State == MEM_FREE && in_reach(at)) {
                reserved = VirtualAlloc(reinterpret_cast<void*>(at), granularity, MEM_RESERVE, PAGE_NOACCESS);
            }
            const auto next = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
            at = (next + granularity - 1) & ~(granularity - 1);
        }
        if (!reserved) return "no free memory within 2 GB of the call (" + hooks::near_summary() + ")";
        regions_.push_back({static_cast<std::uint8_t*>(reserved), 0});
        region = &regions_.back();
    }
    if (region) {
        stub = region->base + region->pages_used * page;
        if (!VirtualAlloc(stub, page, MEM_COMMIT, PAGE_READWRITE)) return error_text("VirtualAlloc", GetLastError());
    }
    // jmp qword ptr [rip+0]; the address; int3 after it.
    const std::uint8_t jump[6] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    std::memset(stub, 0xCC, 16);
    std::memcpy(stub, jump, sizeof(jump));
    std::memcpy(stub + sizeof(jump), &function, sizeof(function));
    DWORD old = 0;
    if (!VirtualProtect(stub, page, PAGE_EXECUTE_READ, &old)) return error_text("VirtualProtect", GetLastError());
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
    if (region) ++region->pages_used;
    *out = stub;
    return {};
}

}  // namespace al::patch
