#define DIRECTINPUT_VERSION 0x0800
#include "core/input/native.hpp"
#include "core/game_hooks/installer.hpp"
#include "common/memory.hpp"
#include "common/windows.hpp"
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
#include <MinHook.h>
#include <algorithm>
#include <bit>
#include <cstring>

namespace al::input {
namespace {
Native* active{};
thread_local bool reading = false;
struct ReadScope { bool outer{!reading}; ReadScope(){reading=true;} ~ReadScope(){if(outer) reading=false;} };
constexpr GUID input_iid{0xbf798031,0x483a,0x4da2,{0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0x00}};
constexpr GUID keyboard_guid{0x6f1d2b61,0xd5a0,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0x00,0x00}};
constexpr GUID mouse_guid{0x6f1d2b60,0xd5a0,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0x00,0x00}};
using GetState = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, void*);
using GetData = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, void*, DWORD*, DWORD);
using PadState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using GetLong = LONG_PTR(WINAPI*)(HWND,int);
using SetLong = LONG_PTR(WINAPI*)(HWND,int,LONG_PTR);
using CallProc = LRESULT(WINAPI*)(WNDPROC,HWND,UINT,WPARAM,LPARAM);
using Foreground = HWND(WINAPI*)();
using WindowThread = DWORD(WINAPI*)(HWND,DWORD*);
using Ancestor = HWND(WINAPI*)(HWND,UINT);
using MapKey = UINT(WINAPI*)(UINT,UINT);
template<class T> T proc(HMODULE m, const char* name) { return m ? std::bit_cast<T>(GetProcAddress(m, name)) : nullptr; }
bool system_target(void* target) {
    MEMORY_BASIC_INFORMATION info{};
    if (!target || !VirtualQuery(target,&info,sizeof(info)) || info.Type != MEM_IMAGE) return false;
    const auto module = module_path(static_cast<HMODULE>(info.AllocationBase));
    wchar_t folder[MAX_PATH]{}; const auto size = GetSystemDirectoryW(folder,MAX_PATH);
    if (!size || size >= MAX_PATH) return false;
    const auto slash = module.find_last_of(L"\\/");
    return slash != std::wstring::npos && _wcsicmp(module.substr(0,slash).c_str(),folder) == 0;
}
}
struct Native::State {
    Service* input{};
    Log log, warning;
    game_hooks::NativeInstaller installer;
    struct Entry { void* target{}; void* head{}; void* original{}; bool enabled{}; };
    std::array<Entry,8> entries{};
    std::array<std::atomic<void*>,8> originals{};
    std::array<std::uint8_t,256> map{};
    std::atomic<std::uint32_t> thread{0};
    std::uint64_t frames{};
    bool di_done{};
    std::atomic<bool> failed{false};
    GetLong get_long{}; SetLong set_long{}; std::atomic<CallProc> call{nullptr}; Foreground foreground{}; WindowThread window_thread{}; Ancestor ancestor{};
    std::mutex user32_mutex;
    struct Window { std::atomic<HWND> hwnd{nullptr}; std::atomic<WNDPROC> previous{nullptr}; };
    std::array<Window,4> windows{};
    explicit State(Service& service, Log logger, Log warn) : input(&service), log(std::move(logger)), warning(std::move(warn)), installer(log) {}
    void note(std::string_view text) noexcept { try { if(log) log(text); } catch (...) {} }
    bool game_thread() const noexcept { return thread.load(std::memory_order_relaxed) == GetCurrentThreadId(); }
    bool user32() {
        std::lock_guard lock(user32_mutex);
        if (call) return true;
        const auto m = GetModuleHandleW(L"user32.dll");
        get_long=proc<GetLong>(m,"GetWindowLongPtrW"); set_long=proc<SetLong>(m,"SetWindowLongPtrW");
        foreground=proc<Foreground>(m,"GetForegroundWindow"); window_thread=proc<WindowThread>(m,"GetWindowThreadProcessId"); ancestor=proc<Ancestor>(m,"GetAncestor");
        const auto mapper=proc<MapKey>(m,"MapVirtualKeyW");
        if (!get_long || !set_long || !foreground || !window_thread || !ancestor || !mapper) return false;
        for (unsigned scan=0;scan<256;++scan) map[scan]=static_cast<std::uint8_t>(mapper(scan & 0x80 ? 0xe000u | (scan & 0x7f) : scan,MAPVK_VSC_TO_VK_EX));
        map[DIK_PAUSE]=VK_PAUSE; map[DIK_NUMLOCK]=VK_NUMLOCK;
        call=proc<CallProc>(m,"CallWindowProcW"); return call.load() != nullptr;
    }
};
Native::Native(Service& service, Log log, Log warning) : state_(new State(service,std::move(log),std::move(warning))) { active=this; }
void Native::fail(std::string_view reason) noexcept {
    if (state_->failed.exchange(true)) return;
    try { state_->input->unavailable(std::string(reason)); state_->note("input_hook_unavailable reason="+std::string(reason)); if(state_->warning) state_->warning(reason); } catch (...) {}
}
bool Native::install(const Methods& methods) noexcept {
    try {
        if(state_->failed || !state_->user32()) { fail("input requires the game's loaded User32"); return false; }
        const auto init=MH_Initialize(); if(init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) { fail(MH_StatusToString(init)); return false; }
        // Stable per-target slots; shared keyboard/mouse implementations and
        // forwarded XInput exports reuse the same hook and continuation.
        const std::array<void*,8> targets{methods.state[0],methods.data[0],methods.state[1],methods.data[1],methods.pads[0],methods.pads[1],methods.pads[2],methods.pads[3]};
        const std::array<void*,8> detours{
            std::bit_cast<void*>(+[](void* self,DWORD n,void* p)->HRESULT{return state_read(0,self,n,p);}),
            std::bit_cast<void*>(+[](void* self,DWORD n,void* p,DWORD* c,DWORD f)->HRESULT{return data_read(1,self,n,p,c,f);}),
            std::bit_cast<void*>(+[](void* self,DWORD n,void* p)->HRESULT{return state_read(2,self,n,p);}),
            std::bit_cast<void*>(+[](void* self,DWORD n,void* p,DWORD* c,DWORD f)->HRESULT{return data_read(3,self,n,p,c,f);}),
            std::bit_cast<void*>(+[](DWORD user,XINPUT_STATE* p)->DWORD{return pad_read(4,user,p);}),
            std::bit_cast<void*>(+[](DWORD user,XINPUT_STATE* p)->DWORD{return pad_read(5,user,p);}),
            std::bit_cast<void*>(+[](DWORD user,XINPUT_STATE* p)->DWORD{return pad_read(6,user,p);}),
            std::bit_cast<void*>(+[](DWORD user,XINPUT_STATE* p)->DWORD{return pad_read(7,user,p);})};
        for(unsigned i=0;i<targets.size();++i) {
            if(!targets[i]) continue;
            auto found=std::find_if(state_->entries.begin(),state_->entries.end(),[&](const auto& e){return e.target==targets[i];});
            if(found == state_->entries.end()) {
                found=std::find_if(state_->entries.begin(),state_->entries.end(),[](const auto& e){return !e.target;});
                if(found == state_->entries.end()) { fail("input method capacity exceeded"); return false; }
                found->target=targets[i]; found->head=detours[i];
                const auto why=state_->installer.create(found->target,&found->head,&found->original);
                if(!why.empty()) { fail(why); return false; }
            }
            state_->originals[i]=found->original;
        }
        // All continuation slots precede activation. Aliased exports are only
        // enabled once, and later discoveries do not re-enable unrelated hooks.
        for(auto& entry:state_->entries) if(entry.target && !entry.enabled) {
            const auto why=state_->installer.enable(entry.target); if(!why.empty()) { fail(why); return false; } entry.enabled=true;
        }
        unsigned mask=0;
        if(methods.state[0] && methods.data[0]) mask|=AL_INPUT_KEYBOARD;
        if(methods.state[1] && methods.data[1]) mask|=AL_INPUT_MOUSE;
        if(std::any_of(std::begin(methods.pads),std::end(methods.pads),[](void* p){return p != nullptr;})) mask|=AL_INPUT_PAD;
        state_->input->ready(mask); state_->note("input_hooks_ready devices="+std::to_string(mask)); return true;
    } catch (...) { fail("input hook setup failed"); return false; }
}
void Native::discover() noexcept {
    try {
        Methods methods;
        const auto di=GetModuleHandleW(L"dinput8.dll");
        if(!state_->di_done && di) {
            using Create=HRESULT(WINAPI*)(HINSTANCE,DWORD,REFIID,void**,IUnknown*);
            const auto create=proc<Create>(di,"DirectInput8Create"); IDirectInput8W* input{};
            if(!create || !system_target(std::bit_cast<void*>(create)) || FAILED(create(GetModuleHandleW(nullptr),DIRECTINPUT_VERSION,input_iid,reinterpret_cast<void**>(&input),nullptr)) || !input) { fail("cannot resolve System32 DirectInput8 devices"); return; }
            for(unsigned k=0;k<2;++k) {
                IDirectInputDevice8W* device{};
                if(SUCCEEDED(input->CreateDevice(k ? mouse_guid:keyboard_guid,&device,nullptr)) && device) {
                    const auto table=*reinterpret_cast<void* const* const*>(device); methods.state[k]=table[9]; methods.data[k]=table[10]; device->Release();
                }
            }
            input->Release();
            for(unsigned k=0;k<2;++k) if(!system_target(methods.state[k]) || !system_target(methods.data[k])) { fail("DirectInput method is missing or outside System32"); return; }
            state_->di_done=true;
        }
        const wchar_t* names[]{L"xinput9_1_0.dll",L"xinput1_4.dll",L"xinput1_3.dll",L"xinputuap.dll"};
        for(unsigned k=0;k<4;++k) {
            const auto target=proc<PadState>(GetModuleHandleW(names[k]),"XInputGetState");
            if(target && system_target(std::bit_cast<void*>(target))) methods.pads[k]=std::bit_cast<void*>(target);
        }
        bool changed=methods.state[0] != nullptr;
        for(unsigned k=0;k<4;++k) if(methods.pads[k] && !state_->originals[k+4]) changed=true;
        if(changed) install(methods);
    } catch (...) { fail("input device discovery failed"); }
}
void Native::begin_frame(std::uint32_t thread) noexcept {
    state_->thread.store(thread,std::memory_order_relaxed);
    if(!state_->input->wanted()) return;
    if(!state_->failed && state_->frames++ % 30 == 0) discover();
    if(state_->call) {
        const auto window=static_cast<HWND>(state_->input->window());
        state_->input->focus(window && state_->ancestor(state_->foreground(),GA_ROOT)==window);
    }
}
void Native::end_frame() noexcept { if(state_->input->wanted()) state_->input->end_frame(); }
long Native::state_read(unsigned slot,void* self,unsigned long size,void* data) {
    ReadScope scope;
    auto& s=*active->state_; const auto result=std::bit_cast<GetState>(s.originals[slot].load())(self,size,data); const auto error=GetLastError();
    if(scope.outer && s.game_thread() && !s.failed) {
        std::array<std::uint8_t,256> empty{};
        if(size==256) { s.input->keyboard(SUCCEEDED(result) && data ? static_cast<const std::uint8_t*>(data):empty.data(),s.map); if(SUCCEEDED(result) && data && (s.input->capture_mask()&AL_INPUT_KEYBOARD)) std::memset(data,0,size); }
        else if(size==sizeof(DIMOUSESTATE) || size==sizeof(DIMOUSESTATE2)) {
            const auto buttons=SUCCEEDED(result) && data ? static_cast<const std::uint8_t*>(data)+12:empty.data(); s.input->mouse(buttons,size-12);
            if(SUCCEEDED(result) && data && (s.input->capture_mask()&AL_INPUT_MOUSE)) std::memset(data,0,size);
        }
    }
    SetLastError(error); return result;
}
long Native::data_read(unsigned slot,void* self,unsigned long size,void* events,unsigned long* count,unsigned long flags) {
    ReadScope scope;
    auto& s=*active->state_; const auto result=std::bit_cast<GetData>(s.originals[slot].load())(self,size,events,count,flags); const auto error=GetLastError();
    auto device=(slot==1 ? AL_INPUT_KEYBOARD:AL_INPUT_MOUSE);
    const auto mask=s.input->capture_mask();
    if(scope.outer && SUCCEEDED(result) && count && s.game_thread() && !s.failed && mask) {
        // Some Windows versions share a GetDeviceData implementation. Ask the
        // device class only on this uncommon buffered/captured path.
        if(s.originals[1].load() && s.originals[1].load()==s.originals[3].load()) {
            DIDEVICEINSTANCEW info{}; info.dwSize=sizeof(info);
            if(FAILED(static_cast<IDirectInputDevice8W*>(self)->GetDeviceInfo(&info))) { SetLastError(error); return result; }
            const auto kind=GET_DIDEVICE_TYPE(info.dwDevType);
            device=kind==DI8DEVTYPE_KEYBOARD ? AL_INPUT_KEYBOARD:kind==DI8DEVTYPE_MOUSE ? AL_INPUT_MOUSE:0u;
        }
        if(mask&device) *count=0;
    }
    SetLastError(error); return result;
}
unsigned long Native::pad_read(unsigned slot,unsigned long user,void* output) {
    ReadScope scope;
    auto& s=*active->state_; auto* data=static_cast<XINPUT_STATE*>(output);
    const auto result=std::bit_cast<PadState>(s.originals[slot].load())(user,data); const auto error=GetLastError();
    if(scope.outer && user<4 && s.game_thread() && !s.failed) {
        al_pad_state_t snapshot{}; snapshot.size=sizeof(snapshot);
        if(result==ERROR_SUCCESS && data) { snapshot.connected=1; snapshot.packet=data->dwPacketNumber; std::memcpy(&snapshot.buttons,&data->Gamepad,sizeof(data->Gamepad)); }
        s.input->pad(user,snapshot);
        if(result==ERROR_SUCCESS && data && (s.input->capture_mask()&AL_INPUT_PAD)) std::memset(&data->Gamepad,0,sizeof(data->Gamepad));
    }
    SetLastError(error); return result;
}
void Native::attach_window(void* pointer) noexcept {
    try {
        if(!pointer || !state_->user32()) return;
        const auto hwnd=static_cast<HWND>(pointer); DWORD pid{};
        if(!state_->window_thread(hwnd,&pid) || pid!=GetCurrentProcessId() || state_->ancestor(hwnd,GA_ROOT)!=hwnd) { fail("input window is not a top-level window of this game"); return; }
        for(const auto& item:state_->windows) if(item.hwnd.load()==hwnd) { state_->input->window(hwnd); return; }
        for(auto& item:state_->windows) if(!item.hwnd.load()) {
            item.previous=reinterpret_cast<WNDPROC>(state_->get_long(hwnd,GWLP_WNDPROC));
            if(!item.previous) { fail("game window procedure is missing"); return; }
            item.hwnd.store(hwnd,std::memory_order_release); SetLastError(0);
            const auto old=state_->set_long(hwnd,GWLP_WNDPROC,std::bit_cast<LONG_PTR>(&window_proc));
            if(!old && GetLastError()) { item.hwnd=nullptr; fail("cannot subclass game window"); return; }
            item.previous=reinterpret_cast<WNDPROC>(old); state_->input->window(hwnd);
            state_->note("input_window_subclassed"); return;
        }
        fail("too many live game windows for input");
    } catch (...) { fail("input window setup failed"); }
}
std::intptr_t Native::window_proc(void* pointer,unsigned id,std::uintptr_t wp,std::intptr_t lp) {
    auto& s=*active->state_; const auto hwnd=static_cast<HWND>(pointer);
    for(auto& item:s.windows) if(item.hwnd.load(std::memory_order_acquire)==hwnd) {
        const auto error=GetLastError();
        if(id==WM_KILLFOCUS || (id==WM_ACTIVATEAPP && !wp)) s.input->focus(false);
        const auto consumed=s.input->message(pointer,id,wp,lp);
        SetLastError(error);
        const auto result=consumed && id!=WM_NCDESTROY && id!=WM_DESTROY ?
            (id==WM_SETCURSOR || (id==WM_UNICHAR && wp==UNICODE_NOCHAR) ? 1:0):s.call.load()(item.previous.load(),hwnd,id,wp,lp);
        if(id==WM_NCDESTROY) { if(s.input->window()==pointer) {s.input->window(nullptr);s.input->focus(false);} item.hwnd=nullptr; }
        return result;
    }
    return 0;
}
} // namespace al::input
