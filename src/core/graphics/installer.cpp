#include "core/graphics/installer.hpp"
#include "common/memory.hpp"
#include "common/windows.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <set>

namespace cmfm::graphics {
namespace {
using Microsoft::WRL::ComPtr;
struct Internal {
    Internal() { begin_internal(); }
    ~Internal() { end_internal(); }
};
std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const auto count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(count), '\0');
    if (count) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::string address(const void* pointer) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pointer)));
    return text;
}
bool executable(const void* pointer) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(pointer, &info, sizeof(info)) && info.State == MEM_COMMIT &&
        !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}
bool writable(void** pointer) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if (reinterpret_cast<std::uintptr_t>(pointer) % alignof(void*)) return false;
    return VirtualQuery(pointer, &info, sizeof(info)) && info.State == MEM_COMMIT &&
        !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        (info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
        reinterpret_cast<std::uintptr_t>(pointer) + sizeof(void*) <=
        reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
}
// Pointer access guards contain faulty storage supplied by a plugin. No C++
// objects requiring unwinding are created in these SEH helpers.
bool exchange(void** pointer, void* value, void** previous) noexcept {
    __try { *previous = InterlockedExchangePointer(pointer, value); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool read_pointer(void* const* pointer, void** value) noexcept {
    __try { *value = *pointer; return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool same_file(const std::wstring& a, const std::wstring& b) {
    Handle left(CreateFileW(a.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle right(CreateFileW(b.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    BY_HANDLE_FILE_INFORMATION x{}, y{};
    return left && right && GetFileInformationByHandle(left.get(), &x) && GetFileInformationByHandle(right.get(), &y) &&
        x.dwVolumeSerialNumber == y.dwVolumeSerialNumber && x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow;
}
std::string verify_native(void* target) {
    MEMORY_BASIC_INFORMATION info{};
    if (!executable(target) || !VirtualQuery(target, &info, sizeof(info)) || info.Type != MEM_IMAGE)
        return "target is not executable image memory";
    const auto module = static_cast<HMODULE>(info.AllocationBase);
    const auto path = module_path(module);
    const auto slash = path.find_last_of(L"\\/");
    const auto name = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
    if (_wcsicmp(name.c_str(), L"dxgi.dll") && _wcsicmp(name.c_str(), L"d3d12.dll") && _wcsicmp(name.c_str(), L"D3D12Core.dll"))
        return "target belongs to " + utf8(name) + ", not a native DXGI/D3D12 module";
    wchar_t system[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(system, MAX_PATH);
    if (!length || length >= MAX_PATH || !same_file(path, std::wstring(system) + L"\\" + name))
        return "target module is not the System32 file: " + utf8(name);
    const auto base = static_cast<const std::uint8_t*>(info.AllocationBase);
    if (!readable(base, sizeof(IMAGE_DOS_HEADER))) return "unreadable native image header";
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 64 || dos->e_lfanew > 0x100000) return "invalid native image header";
    const auto pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!readable(pe, sizeof(*pe)) || pe->Signature != IMAGE_NT_SIGNATURE ||
        pe->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || pe->FileHeader.NumberOfSections > 96)
        return "invalid native PE header";
    const auto sections = IMAGE_FIRST_SECTION(pe);
    if (!readable(sections, pe->FileHeader.NumberOfSections * sizeof(*sections))) return "unreadable native sections";
    const auto rva = reinterpret_cast<std::uintptr_t>(target) - reinterpret_cast<std::uintptr_t>(base);
    for (unsigned i = 0; i < pe->FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && rva >= section.VirtualAddress &&
            rva - section.VirtualAddress < section.Misc.VirtualSize && rva < pe->OptionalHeader.SizeOfImage) {
            HMODULE pinned{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                static_cast<LPCWSTR>(target), &pinned)) return "cannot retain the native target module";
            return {};
        }
    }
    return "target is outside native executable sections";
}
void* jump_destination(void* target) noexcept {
    auto p = static_cast<const std::uint8_t*>(target);
    if (!readable(p, 16)) return nullptr;
    if (p[0] == 0xe9) { std::int32_t delta{}; std::memcpy(&delta, p + 1, 4); return const_cast<std::uint8_t*>(p + 5 + delta); }
    if (p[0] == 0xeb) return const_cast<std::uint8_t*>(p + 2 + static_cast<std::int8_t>(p[1]));
    if (p[0] == 0xff && p[1] == 0x25) {
        std::int32_t delta{}; std::memcpy(&delta, p + 2, 4);
        void* result{};
        if (readable(p + 6 + delta, 8) && read_pointer(reinterpret_cast<void* const*>(p + 6 + delta), &result)) return result;
    }
    if (p[0] == 0x48 && p[1] == 0xb8 && p[10] == 0xff && p[11] == 0xe0) {
        void* result{}; std::memcpy(&result, p + 2, 8); return result;
    }
    return nullptr;
}
std::string owner_of(void* target) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(target, &info, sizeof(info)) || info.Type != MEM_IMAGE) return "private-code";
    auto path = module_path(static_cast<HMODULE>(info.AllocationBase));
    const auto slash = path.find_last_of(L"\\/");
    return utf8(path.substr(slash == std::wstring::npos ? 0 : slash + 1));
}

REFIID interface_id(std::uint32_t family, int version) {
    switch (family) {
#define CMFM_IID(n, type) case n: return __uuidof(type)
    case CMFM_D3D12_FACTORY: switch (version) {
        CMFM_IID(0, IDXGIFactory); CMFM_IID(1, IDXGIFactory1); CMFM_IID(2, IDXGIFactory2); CMFM_IID(3, IDXGIFactory3);
        CMFM_IID(4, IDXGIFactory4); CMFM_IID(5, IDXGIFactory5); CMFM_IID(6, IDXGIFactory6); CMFM_IID(7, IDXGIFactory7);
    } break;
    case CMFM_D3D12_SWAPCHAIN: switch (version) {
        CMFM_IID(0, IDXGISwapChain); CMFM_IID(1, IDXGISwapChain1); CMFM_IID(2, IDXGISwapChain2);
        CMFM_IID(3, IDXGISwapChain3); CMFM_IID(4, IDXGISwapChain4);
    } break;
    case CMFM_D3D12_DEVICE: switch (version) {
        CMFM_IID(0, ID3D12Device); CMFM_IID(1, ID3D12Device1); CMFM_IID(2, ID3D12Device2); CMFM_IID(3, ID3D12Device3);
        CMFM_IID(4, ID3D12Device4); CMFM_IID(5, ID3D12Device5); CMFM_IID(6, ID3D12Device6); CMFM_IID(7, ID3D12Device7);
        CMFM_IID(8, ID3D12Device8); CMFM_IID(9, ID3D12Device9); CMFM_IID(10, ID3D12Device10); CMFM_IID(11, ID3D12Device11);
        CMFM_IID(12, ID3D12Device12); CMFM_IID(13, ID3D12Device13); CMFM_IID(14, ID3D12Device14);
    } break;
    case CMFM_D3D12_QUEUE: return __uuidof(ID3D12CommandQueue);
    case CMFM_D3D12_COMMAND_LIST: switch (version) {
        CMFM_IID(0, ID3D12GraphicsCommandList); CMFM_IID(1, ID3D12GraphicsCommandList1); CMFM_IID(2, ID3D12GraphicsCommandList2);
        CMFM_IID(3, ID3D12GraphicsCommandList3); CMFM_IID(4, ID3D12GraphicsCommandList4); CMFM_IID(5, ID3D12GraphicsCommandList5);
        CMFM_IID(6, ID3D12GraphicsCommandList6); CMFM_IID(7, ID3D12GraphicsCommandList7); CMFM_IID(8, ID3D12GraphicsCommandList8);
        CMFM_IID(9, ID3D12GraphicsCommandList9); CMFM_IID(10, ID3D12GraphicsCommandList10);
    } break;
#undef CMFM_IID
    }
    return __uuidof(IUnknown); // Only called with an SDK-validated version.
}
std::string make_object(std::uint32_t family, void* object, ComPtr<IUnknown>& out) {
    if (family == CMFM_D3D12_DEVICE || family == CMFM_D3D12_SWAPCHAIN) {
        out = static_cast<IUnknown*>(object); return {};
    }
    if (family == CMFM_D3D12_FACTORY) {
        // Only COM calls and dynamic exports in the core: no DXGI/D3D12 imports.
        const auto module = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        using Create = HRESULT (WINAPI*)(UINT, REFIID, void**);
        const auto create = module ? std::bit_cast<Create>(GetProcAddress(module, "CreateDXGIFactory2")) : nullptr;
        const auto result = create ? create(0, __uuidof(IDXGIFactory), reinterpret_cast<void**>(out.GetAddressOf())) : E_FAIL;
        // Retain the library reference until exit, like the hook trampolines.
        if (module && (FAILED(result) || !out)) FreeLibrary(module);
        return SUCCEEDED(result) && out ? "" : "cannot create a native factory";
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(static_cast<IUnknown*>(object)->QueryInterface(IID_PPV_ARGS(&device)))) return "object is not a D3D12 device";
    if (family == CMFM_D3D12_QUEUE) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        return SUCCEEDED(device->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue),
            reinterpret_cast<void**>(out.GetAddressOf()))) && out ? "" : "cannot create a direct queue";
    }
    if (family == CMFM_D3D12_COMMAND_LIST) {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
            FAILED(list->Close())) return "cannot create a direct command list";
        out = list;
        return {};
    }
    return "unknown interface family";
}
}

namespace detail {
void* entry_stub(void* context, void* (*select)(void*) noexcept) {
    // At entry RSP%16 == 8. Save flags and every volatile integer register,
    // then reserve shadow space + six XMM registers + alignment (0x88).
    std::vector<std::uint8_t> bytes{0x9c, 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,
        0x48, 0x81, 0xec, 0x88, 0, 0, 0};
    for (std::uint8_t i = 0; i < 6; ++i) bytes.insert(bytes.end(), {0xf3, 0x0f, 0x7f,
        static_cast<std::uint8_t>(0x44 + i * 8), 0x24, static_cast<std::uint8_t>(0x20 + i * 16)});
    const auto append = [&](std::uintptr_t value) {
        const auto p = reinterpret_cast<const std::uint8_t*>(&value); bytes.insert(bytes.end(), p, p + 8);
    };
    bytes.insert(bytes.end(), {0x48, 0xb9}); append(reinterpret_cast<std::uintptr_t>(context));
    bytes.insert(bytes.end(), {0x48, 0xb8}); append(std::bit_cast<std::uintptr_t>(select));
    bytes.insert(bytes.end(), {0xff, 0xd0, 0x48, 0x89, 0x84, 0x24, 0x88, 0, 0, 0}); // saved R11 = destination
    for (std::uint8_t i = 0; i < 6; ++i) bytes.insert(bytes.end(), {0xf3, 0x0f, 0x6f,
        static_cast<std::uint8_t>(0x44 + i * 8), 0x24, static_cast<std::uint8_t>(0x20 + i * 16)});
    bytes.insert(bytes.end(), {0x48, 0x81, 0xc4, 0x88, 0, 0, 0, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59,
        0x41, 0x58, 0x5a, 0x59, 0x58, 0x9d, 0x41, 0xff, 0xe3});
    const auto memory = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!memory) return nullptr;
    std::memcpy(memory, bytes.data(), bytes.size());
    DWORD old{};
    if (!VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &old) || !FlushInstructionCache(GetCurrentProcess(), memory, bytes.size())) {
        VirtualFree(memory, 0, MEM_RELEASE); return nullptr;
    }
    return memory;
}
}

struct NativeInstaller::State {
    struct Entry {
        State* state{};
        void* target{};
        void* stub{};
        void* original{};
        void* head{};
        std::atomic<bool> ready{false};
        std::vector<Layer> layers;
    };
    Log log;
    Warning warning;
    std::mutex mutex;
    std::atomic<bool> unavailable{false};
    std::atomic<bool> warned{false};
    std::vector<std::unique_ptr<Entry>> entries;
    void note(std::string_view text) noexcept { try { if (log) log(text); } catch (...) {} }
    static void* select(void* context) noexcept {
        const auto& entry = *static_cast<Entry*>(context);
        return internal() || !entry.ready.load(std::memory_order_acquire) || entry.state->unavailable.load(std::memory_order_relaxed)
            ? entry.original : entry.head;
    }
};

NativeInstaller::NativeInstaller(Log log, Warning warning) : state_(new State) {
    state_->log = std::move(log); state_->warning = std::move(warning);
}
std::string NativeInstaller::install(std::uint32_t family, void* object, const std::vector<const Chain*>& chains) {
    std::lock_guard lock(state_->mutex);
    Internal internal_region;
    struct Link { void** storage; void* value; void* previous; bool written; };
    std::vector<Link> links;
    std::vector<State::Entry*> created;
    const auto rollback = [&]() noexcept {
        for (auto* entry : created) { entry->ready.store(false, std::memory_order_release); MH_QueueDisableHook(entry->target); }
        if (!created.empty() && MH_ApplyQueued() != MH_OK) state_->note("d3d12_rollback: disabling failed; entries remain in original-only mode");
        for (auto& link : links) if (link.written) {
            void* ignored{};
            if (!exchange(link.storage, link.previous, &ignored)) state_->note("d3d12_rollback: plugin next storage became inaccessible");
        }
        // Records and MinHook trampolines remain pinned, never dangling.
    };
    try {
        if (chains.empty()) return {};
        if (!object || state_->unavailable.load()) return "no usable graphics object/installer";
        ComPtr<IUnknown> base;
        auto problem = make_object(family, object, base);
        if (!problem.empty()) return problem;
        std::set<void*> targets;
        std::set<void**> storages;
        std::vector<std::unique_ptr<State::Entry>> pending;
        for (const auto* chain : chains) {
            if (!chain || chain->layers.empty() || CMFM_D3D12_METHOD_FAMILY(chain->method) != family) return "invalid chain";
            const auto slot = CMFM_D3D12_METHOD_SLOT(chain->method);
            const auto version = detail::interface_version(family, slot);
            if (version < 0) return "slot " + std::to_string(slot) + " exceeds the SDK interface";
            ComPtr<IUnknown> typed;
            if (FAILED(base->QueryInterface(interface_id(family, version), reinterpret_cast<void**>(typed.GetAddressOf()))) || !typed)
                return "object does not implement the interface for slot " + std::to_string(slot);
            void* table{}; void* target{};
            if (!read_pointer(reinterpret_cast<void* const*>(typed.Get()), &table) ||
                !read_pointer(static_cast<void* const*>(table) + slot, &target)) return "unreadable vtable slot";
            problem = verify_native(target);
            if (!problem.empty()) return "slot " + std::to_string(slot) + ": " + problem;
            if (!targets.insert(target).second || std::any_of(state_->entries.begin(), state_->entries.end(),
                [&](const auto& entry) { return entry->target == target; })) return "two chains resolve to one target";
            for (const auto& layer : chain->layers) {
                if (!executable(layer.detour) || !writable(layer.next)) return "invalid detour or unwritable/alignment-invalid next storage";
                if (!storages.insert(layer.next).second) return "two layers share next storage";
                for (const auto& entry : state_->entries) for (const auto& old : entry->layers)
                    if (old.next == layer.next) return "next storage is already used by another family";
            }
            if (auto destination = jump_destination(target)) {
                // MinHook/overlay relays commonly live in private executable
                // memory and jump once more to the owning DLL. Bounded walk;
                // this is diagnostic only, never the target we patch.
                std::set<void*> seen{target};
                for (unsigned step = 0; step < 8 && seen.insert(destination).second; ++step) {
                    const auto next = jump_destination(destination);
                    if (!next) break;
                    destination = next;
                }
                state_->note("d3d12_existing_patch target=" + address(target) +
                    " destination=" + address(destination) + " module=" + owner_of(destination));
            }
            auto entry = std::make_unique<State::Entry>();
            entry->state = state_; entry->target = target; entry->head = chain->layers.front().detour; entry->layers = chain->layers;
            pending.push_back(std::move(entry));
        }
        // Allocate all bookkeeping before any hook or published pointer exists.
        created.reserve(pending.size());
        std::size_t count = 0; for (const auto& entry : pending) count += entry->layers.size();
        links.reserve(count); state_->entries.reserve(state_->entries.size() + pending.size());
        for (auto& entry : pending) {
            entry->stub = detail::entry_stub(entry.get(), &State::select);
            if (!entry->stub) { rollback(); return "cannot allocate an entry stub"; }
            const auto result = MH_CreateHook(entry->target, entry->stub, &entry->original);
            if (result != MH_OK) { VirtualFree(entry->stub, 0, MEM_RELEASE); rollback(); return std::string("MinHook create: ") + MH_StatusToString(result); }
            auto* record = entry.get(); created.push_back(record); state_->entries.push_back(std::move(entry));
            for (std::size_t k = 0; k < record->layers.size(); ++k) links.push_back({record->layers[k].next,
                k + 1 < record->layers.size() ? record->layers[k + 1].detour : record->original, nullptr, false});
        }
        for (auto& link : links) {
            if (!exchange(link.storage, link.value, &link.previous)) { rollback(); return "cannot write next storage"; }
            link.written = true;
        }
        for (auto* entry : created) {
            const auto result = MH_QueueEnableHook(entry->target);
            if (result != MH_OK) { rollback(); return std::string("MinHook queue: ") + MH_StatusToString(result); }
        }
        const auto result = MH_ApplyQueued();
        if (result != MH_OK) { rollback(); return std::string("MinHook enable: ") + MH_StatusToString(result); }
        for (auto* entry : created) entry->ready.store(true, std::memory_order_release);
        state_->note("d3d12_install family=" + std::to_string(family) + " targets=" + std::to_string(created.size()));
        return {};
    } catch (...) { rollback(); return "graphics hook preparation failed"; }
}
void NativeInstaller::report(const Hub& hub) noexcept {
    try {
        cmfm_d3d12_status_t status{}; status.size = sizeof(status); hub.status(status);
        if (status.state == CMFM_D3D12_UNAVAILABLE) {
            state_->unavailable.store(true, std::memory_order_relaxed);
            if (!state_->warned.exchange(true)) {
                try { if (state_->warning) state_->warning(status.reason ? status.reason : "graphics installation failed"); } catch (...) {}
            }
        }
        for (const auto& line : hub.summary()) state_->note(line);
    } catch (...) {}
}
}  // namespace cmfm::graphics
