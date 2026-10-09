#include "cmfm/cmfm_plugin.h"
#include "common/log_file.hpp"
#include "common/paths.hpp"
#include "common/windows.hpp"
#include <windows.h>
#include <d3d12.h>
#include <MinHook.h>
#include <intrin.h>
#include <bit>
#include <atomic>

namespace {
cmfm::LogFile record_log;
using Device=HRESULT (WINAPI*)(IUnknown*,D3D_FEATURE_LEVEL,REFIID,void**);
Device original{};
bool coexistence=true;
bool win_main_seen=false, spl_seen=false;
std::atomic<unsigned> api_calls{0};
using WinMainFn=int (WINAPI*)(HINSTANCE,HINSTANCE,char*,int);
WinMainFn win_original{};
int WINAPI spl_like_win_main(HINSTANCE instance,HINSTANCE previous,char* command,int show) {
    spl_seen=win_main_seen;
    record_log.line(spl_seen?L"spl_on_win_main_after_cmfm=1":L"spl_on_win_main_after_cmfm=0");
    return win_original(instance,previous,command,show);
}
HRESULT WINAPI device_hook(IUnknown* adapter,D3D_FEATURE_LEVEL level,REFIID iid,void** out) {
    LARGE_INTEGER entered{}; QueryPerformanceCounter(&entered);
    record_log.line(L"device_api_call_enter call="+std::to_wstring(api_calls.fetch_add(1)+1)+
        L" entry_qpc="+std::to_wstring(entered.QuadPart)+L" thread="+std::to_wstring(GetCurrentThreadId()));
    const auto caller=_ReturnAddress();
    using Check=BOOL (*)(const void*);
    const auto check=std::bit_cast<Check>(GetProcAddress(GetModuleHandleW(nullptr),"cmfm_test_validate_device_caller"));
    coexistence=coexistence && check && check(caller);
    const auto result=original(adapter,level,iid,out);
    coexistence=coexistence && check && check(caller);
    SetLastError(0x5a);
    return result;
}
void CMFM_CALL phase(cmfm_plugin_t*,cmfm_phase_t id,const void* data) {
    bool valid=false;
    if (id==CMFM_PHASE_PRE_MAIN) {
        using Prime=BOOL (*)();
        const auto prime=std::bit_cast<Prime>(GetProcAddress(GetModuleHandleW(nullptr),"cmfm_test_prime_code"));
        const auto module=LoadLibraryW(L"d3d12.dll");
        void* function=module?std::bit_cast<void*>(GetProcAddress(module,"D3D12CreateDevice")):nullptr;
        void* forward=nullptr;
        valid=!data && prime && prime() && function && MH_Initialize()==MH_OK &&
            MH_CreateHook(function,std::bit_cast<void*>(&device_hook),&forward)==MH_OK;
        if (valid) { original=std::bit_cast<Device>(forward); valid=MH_EnableHook(function)==MH_OK; }
    } else if (id==CMFM_PHASE_WIN_MAIN) {
        const auto p=static_cast<const cmfm_phase_win_main_t*>(data);
        valid=p && p->size==sizeof(*p) && p->instance==GetModuleHandleW(nullptr) && p->command_line;
        win_main_seen=valid;
    } else if (id==CMFM_PHASE_DEVICE_CREATED) {
        const auto p=static_cast<const cmfm_phase_device_t*>(data);
        valid=p && p->size==sizeof(*p) && p->device && p->adapter && p->riid &&
            *static_cast<const GUID*>(p->riid)==__uuidof(ID3D12Device) && p->minimum_feature_level==D3D_FEATURE_LEVEL_11_0 && coexistence;
        if (valid) { auto device=static_cast<ID3D12Device*>(p->device); device->AddRef(); valid=SUCCEEDED(device->GetDeviceRemovedReason()); device->Release();
            record_log.line(L"device="+std::to_wstring(reinterpret_cast<std::uintptr_t>(p->device))); }
    } else if (id==CMFM_PHASE_SHUTDOWN) {
        const auto p=static_cast<const cmfm_phase_shutdown_t*>(data); valid=p && p->size==sizeof(*p) && p->exit_code==37 && coexistence && spl_seen;
    }
    record_log.line(L"phase="+std::to_wstring(id)+L" valid="+std::to_wstring(valid)+L" thread="+std::to_wstring(GetCurrentThreadId()));
    SetLastError(0xbad); // The core must preserve the game's last error.
}
const cmfm_plugin_info_t info{sizeof(info),CMFM_API_VERSION,"phase_probe","Phase probe","0.0.1",CMFM_PHASE_BIT(CMFM_PHASE_PRE_MAIN)|
    CMFM_PHASE_BIT(CMFM_PHASE_WIN_MAIN)|CMFM_PHASE_BIT(CMFM_PHASE_DEVICE_CREATED)|CMFM_PHASE_BIT(CMFM_PHASE_SHUTDOWN),
    0,nullptr,nullptr,nullptr,phase,nullptr};
}
extern "C" CMFM_PLUGIN_EXPORT const cmfm_plugin_info_t* CMFM_CALL cmfm_plugin_info() { return &info; }
extern "C" CMFM_PLUGIN_EXPORT cmfm_result_t CMFM_CALL cmfm_plugin_init(const cmfm_api_t*,const cmfm_init_t*) {
    return record_log.open(cmfm::environment(L"CMFM_TEST_PLUGIN_RECORD"))?CMFM_OK:CMFM_ERROR;
}
extern "C" __declspec(dllexport) BOOL cmfm_test_install_win_main_hook(void* target) {
    void* forward=nullptr;
    if (MH_CreateHook(target,std::bit_cast<void*>(&spl_like_win_main),&forward)!=MH_OK) return FALSE;
    win_original=std::bit_cast<WinMainFn>(forward);
    return MH_EnableHook(target)==MH_OK;
}
