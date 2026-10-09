// Original stand-in, a real WARP device, and a test mod that behaves like
// DLSS's caller + code-hash checks. No actual game or vendor binary analysis.
#include "common/log_file.hpp"
#include "common/paths.hpp"
#include "common/windows.hpp"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstring>
#include <bit>

extern "C" void WinMainCRTStartup();

namespace {
std::uint8_t saved[4096]{};
const std::uint8_t* begin{};
std::size_t extent{};
struct InstallSplLikeHook {
    InstallSplLikeHook() {
        using Install=BOOL (*)(void*);
        const auto plugin=GetModuleHandleW(L"phase_probe.dll");
        if (!plugin) return;
        const auto install=std::bit_cast<Install>(GetProcAddress(plugin,"al_test_install_win_main_hook"));
        const auto wrapper=reinterpret_cast<const std::uint8_t*>(&WinMainCRTStartup);
        std::int32_t delta=0; std::memcpy(&delta,wrapper+14,4);
        const auto common=wrapper+18+delta;
        for (unsigned i=0; i+12<512; ++i) {
            if (common[i]!=0x48 || common[i+1]!=0x8d || common[i+2]!=0x0d || common[i+7]!=0xe8) continue;
            std::memcpy(&delta,common+i+3,4);
            if (common+i+7+delta!=reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr))) continue;
            std::memcpy(&delta,common+i+8,4);
            auto target=const_cast<std::uint8_t*>(common+i+12+delta);
            // SPL resolves this target before installing its inline hook.
            if (target!=std::bit_cast<void*>(&WinMain) || !install || !install(target)) ExitProcess(96);
            return;
        }
        ExitProcess(97);
    }
} install_spl_like_hook;
}
extern "C" __declspec(dllexport) __declspec(noinline) void* al_test_create_device(IUnknown* adapter, BOOL probe) {
    void* device=nullptr;
    const auto result=D3D12CreateDevice(adapter,D3D_FEATURE_LEVEL_11_0,__uuidof(ID3D12Device),probe?nullptr:&device);
    if (GetLastError()!=0x5a || FAILED(result)) return reinterpret_cast<void*>(1);
    return device;
}
extern "C" __declspec(dllexport) BOOL al_test_prime_code() {
    const auto pointer=reinterpret_cast<DWORD64>(&al_test_create_device);
    DWORD64 base=0;
    const auto fn=RtlLookupFunctionEntry(pointer,&base,nullptr);
    if (!fn || fn->EndAddress<=fn->BeginAddress || fn->EndAddress-fn->BeginAddress>sizeof(saved)) return FALSE;
    begin=reinterpret_cast<const std::uint8_t*>(base+fn->BeginAddress);
    extent=fn->EndAddress-fn->BeginAddress;
    std::memcpy(saved,begin,extent);
    return TRUE;
}
extern "C" __declspec(dllexport) BOOL al_test_validate_device_caller(const void* caller) {
    const auto at=static_cast<const std::uint8_t*>(caller);
    return begin && at>=begin+6 && at<begin+extent &&
        ((at[-6]==0xff && at[-5]==0x15) || at[-5]==0xe8) && !std::memcmp(saved,begin,extent);
}
extern "C" BOOL al_test_startup_restored();
extern "C" BOOL al_test_cookie_unchanged();
struct RenderSetup { IUnknown* adapter; ID3D12Device* device{}; DWORD thread{}; };
DWORD WINAPI create_on_setup_thread(void* parameter) {
    auto& setup=*static_cast<RenderSetup*>(parameter);
    setup.thread=GetCurrentThreadId();
    setup.device=static_cast<ID3D12Device*>(al_test_create_device(setup.adapter,FALSE));
    auto again=static_cast<ID3D12Device*>(al_test_create_device(setup.adapter,FALSE));
    if (reinterpret_cast<std::uintptr_t>(again)<=1) return 95;
    again->Release();
    return reinterpret_cast<std::uintptr_t>(setup.device)>1?0:94;
}
int WINAPI WinMain(HINSTANCE,HINSTANCE,char*,int) {
    al::LogFile log;
    if (!log.open(al::environment(L"AL_FAKE_OUTPUT"))) return 90;
    log.line(L"main_thread_id="+std::to_wstring(GetCurrentThreadId()));
    if (!al_test_startup_restored() || !al_test_cookie_unchanged()) return 91;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IUnknown> adapter;
    if (FAILED(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) return 92;
    if (al_test_create_device(adapter.Get(),TRUE)) return 93;
    if (al_test_create_device(adapter.Get(),TRUE)) return 93;
    RenderSetup setup{adapter.Get()};
    if (al::environment(L"AL_FAKE_DEVICE_THREAD").empty()) {
        if (const auto result=create_on_setup_thread(&setup)) return static_cast<int>(result);
    } else {
        al::Handle thread(CreateThread(nullptr,0,create_on_setup_thread,&setup,0,nullptr));
        DWORD result=99;
        if (!thread || WaitForSingleObject(thread.get(),5000)!=WAIT_OBJECT_0 || !GetExitCodeThread(thread.get(),&result) || result) return 98;
    }
    log.line(L"device="+std::to_wstring(reinterpret_cast<std::uintptr_t>(setup.device)));
    log.line(L"device_thread_id="+std::to_wstring(setup.thread));
    setup.device->Release();
    log.line(L"warp_device_and_caller_checks=1");
    return 37;
}
