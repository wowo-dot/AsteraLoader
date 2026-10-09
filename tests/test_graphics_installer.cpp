#include "core/graphics/installer.hpp"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <array>
#include <bit>
#include <cstdarg>
#include <iostream>
#include <stdexcept>

namespace {
using namespace al::graphics;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool arguments_valid{};
using Mixed = double (*)(std::uint64_t, double, std::uint64_t, double, std::uint64_t, double, std::uint64_t, double);
double mixed(std::uint64_t a, double b, std::uint64_t c, double d, std::uint64_t e, double f, std::uint64_t g, double h) {
    arguments_valid = a == 0x1234567812345678ull && b == 2.25 && c == 0xabcdef98abcdef98ull && d == -4.5 &&
        e == 0xaaaa5555aaaa5555ull && f == 6.75 && g == 0xffff0000ffff0000ull && h == -8.125;
    return 123.5;
}
void* select_mixed(void* context) noexcept {
    // Volatile GPR/XMM use and last-error observation inside the bridge.
    arguments_valid = false;
    return context;
}
using Varargs = double (*)(int, ...);
double varargs(int count, ...) {
    va_list args; va_start(args, count);
    double sum = 0; for (int i = 0; i < count; ++i) sum += va_arg(args, double);
    va_end(args); return sum;
}
void foreign() {}
struct Fake {
    void** table;
    bool newest = false;
};
HRESULT STDMETHODCALLTYPE query(Fake* self, REFIID iid, void** out) {
    *out = nullptr;
    if (iid == __uuidof(IDXGISwapChain) || self->newest) { *out = self; return S_OK; }
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE count(Fake*) { return 1; }
UINT STDMETHODCALLTYPE node_count(ID3D12Device*) { return 777; }
UINT STDMETHODCALLTYPE descriptor_size(ID3D12Device*, D3D12_DESCRIPTOR_HEAP_TYPE) { return 888; }
Chain chain(std::uint32_t family, unsigned slot, void** next, void* detour = std::bit_cast<void*>(&foreign)) {
    return {AL_D3D12_METHOD(family, slot), {{detour, next, nullptr, 0}}};
}
}
int main() {
    try {
        // x64 float arguments in XMM1/XMM3 and arguments 5..8 on the stack,
        // as well as varargs' duplicated floating-point integer registers.
        const auto stub = detail::entry_stub(std::bit_cast<void*>(&mixed), &select_mixed);
        require(stub != nullptr, "mixed bridge allocation");
        SetLastError(0x1234);
        require(std::bit_cast<Mixed>(stub)(0x1234567812345678ull, 2.25, 0xabcdef98abcdef98ull, -4.5,
            0xaaaa5555aaaa5555ull, 6.75, 0xffff0000ffff0000ull, -8.125) == 123.5 && arguments_valid &&
            GetLastError() == 0x1234, "bridge changed argument/return/last-error contract");
        VirtualFree(stub, 0, MEM_RELEASE);
        const auto variadic = detail::entry_stub(std::bit_cast<void*>(&varargs), &select_mixed);
        require(variadic && std::bit_cast<Varargs>(variadic)(5, 1.25, 2.5, 3.75, 4.125, 5.5) == 17.125, "bridge varargs registers/stack");
        VirtualFree(variadic, 0, MEM_RELEASE);
        require(detail::interface_version(AL_D3D12_SWAPCHAIN, 8) == 0 &&
            detail::interface_version(AL_D3D12_SWAPCHAIN, 22) == 1 &&
            detail::interface_version(AL_D3D12_SWAPCHAIN, 39) == 3 &&
            detail::interface_version(AL_D3D12_DEVICE, 2) == -1 &&
            detail::interface_version(AL_D3D12_COMMAND_LIST, 255) == -1, "SDK interface bounds");

        using Microsoft::WRL::ComPtr;
        ComPtr<IDXGIFactory4> factory; ComPtr<IUnknown> adapter; ComPtr<ID3D12Device> device;
        require(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "WARP device");
        require(MH_Initialize() == MH_OK, "MinHook init");
        auto native_table = *reinterpret_cast<void***>(device.Get());
        const auto sentinel = reinterpret_cast<void*>(0x1234);
        void* next = sentinel;
        NativeInstaller installer({}, {});
        require(installer.install(AL_D3D12_DEVICE, nullptr, {}).empty() && next == sentinel, "empty registry changed storage");
        std::array<void*, 256> table{};
        table[0] = std::bit_cast<void*>(&query); table[1] = table[2] = std::bit_cast<void*>(&count);
        table[8] = std::bit_cast<void*>(&foreign);
        Fake fake{table.data()};
        auto invalid = chain(AL_D3D12_SWAPCHAIN, 8, &next);
        require(installer.install(AL_D3D12_SWAPCHAIN, &fake, {&invalid}).find("not a native") != std::string::npos && next == sentinel,
            "non-System32 target accepted/changed next");
        auto high = chain(AL_D3D12_SWAPCHAIN, 255, &next);
        require(installer.install(AL_D3D12_SWAPCHAIN, &fake, {&high}).find("exceeds") != std::string::npos && next == sentinel,
            "out-of-range slot accepted");
        auto newer = chain(AL_D3D12_SWAPCHAIN, 39, &next);
        require(installer.install(AL_D3D12_SWAPCHAIN, &fake, {&newer}).find("does not implement") != std::string::npos && next == sentinel,
            "missing interface accepted");
        table[8] = table[13] = native_table[7];
        void* other_next = sentinel;
        auto duplicate = chain(AL_D3D12_SWAPCHAIN, 13, &other_next);
        require(installer.install(AL_D3D12_SWAPCHAIN, &fake, {&invalid, &duplicate}).find("one target") != std::string::npos &&
            next == sentinel && other_next == sentinel, "duplicate target accepted/partially linked");
        table[13] = native_table[15];
        duplicate.layers[0].next = &next;
        require(installer.install(AL_D3D12_SWAPCHAIN, &fake, {&invalid, &duplicate}).find("share next") != std::string::npos &&
            next == sentinel, "shared next storage accepted");
        auto readonly = static_cast<void**>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        require(readonly != nullptr, "readonly storage allocation");
        *readonly = sentinel; DWORD old{};
        require(VirtualProtect(readonly, 4096, PAGE_READONLY, &old) != FALSE, "readonly protection");
        invalid.layers[0].next = readonly;
        require(installer.install(AL_D3D12_SWAPCHAIN, &fake, {&invalid}).find("unwritable") != std::string::npos &&
            *readonly == sentinel, "readonly next storage accepted");
        VirtualFree(readonly, 0, MEM_RELEASE);

        // A real second-target create failure after the first disabled hook
        // was created. No next may change and the first target must stay native.
        void* blocked_original{};
        require(MH_CreateHook(native_table[15], std::bit_cast<void*>(&descriptor_size), &blocked_original) == MH_OK, "conflicting hook fixture");
        auto first = chain(AL_D3D12_DEVICE, 7, &next, std::bit_cast<void*>(&node_count));
        auto blocked = chain(AL_D3D12_DEVICE, 15, &other_next, std::bit_cast<void*>(&descriptor_size));
        require(installer.install(AL_D3D12_DEVICE, device.Get(), {&first, &blocked}).find("MinHook create") != std::string::npos &&
            next == sentinel && other_next == sentinel && device->GetNodeCount() == 1, "create failure did not roll back");
        require(MH_RemoveHook(native_table[15]) == MH_OK, "conflicting hook cleanup");

        NativeInstaller active({}, {});
        require(active.install(AL_D3D12_DEVICE, device.Get(), {&blocked}).empty() && other_next != sentinel &&
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) == 888, "native detour not enabled");
        begin_internal(); begin_internal();
        require(device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) != 888, "internal call entered layers");
        end_internal(); end_internal();
        require(device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) == 888, "internal nesting did not restore chain");
        Hub hub; hub.set_unavailable("fixture later-family failure"); active.report(hub);
        require(device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) != 888, "unavailable installer still entered plugin");
        std::cout << "Graphics bridge ABI, resolution, failure rollback and bypass passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
