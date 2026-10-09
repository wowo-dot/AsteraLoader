#define DIRECTINPUT_VERSION 0x0800
#include "core/input/native.hpp"
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
#include <bit>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <new>

std::atomic<std::uint64_t> allocations{0};
void* operator new(std::size_t n) {++allocations;if(auto p=std::malloc(n ? n:1)) return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}

namespace {
void check(bool value,const char* why) {if(!value) throw std::runtime_error(why);}
volatile bool lost=false;
unsigned game_messages=0,plugin_messages=0,late_messages=0;
WNDPROC previous{};
__declspec(noinline) HRESULT STDMETHODCALLTYPE keyboard(void*,DWORD size,void* data) {
    SetLastError(0x1122); if(lost) return DIERR_INPUTLOST;
    std::memset(data,0,size); if(size==256) {auto bytes=static_cast<unsigned char*>(data);bytes[0x41]=0x80;bytes[0x1d]=0x80;} return DI_OK;
}
__declspec(noinline) HRESULT STDMETHODCALLTYPE mouse(void*,DWORD size,void* data) {
    SetLastError(0x3344);std::memset(data,0,size);
    if(size==sizeof(DIMOUSESTATE2)) {auto p=static_cast<DIMOUSESTATE2*>(data);p->lX=15;p->lY=-12;p->rgbButtons[0]=0x80;} return DI_OK;
}
__declspec(noinline) HRESULT STDMETHODCALLTYPE data_keyboard(void*,DWORD,void*,DWORD* count,DWORD) {if(count) *count=2;SetLastError(0x5566);return DI_BUFFEROVERFLOW;}
__declspec(noinline) HRESULT STDMETHODCALLTYPE data_mouse(void*,DWORD n,void* p,DWORD* count,DWORD flags) {return data_keyboard(nullptr,n,p,count,flags);}
__declspec(noinline) DWORD WINAPI pad_inner(DWORD slot,XINPUT_STATE* p) {
    SetLastError(0x7788);if(slot>=2) return ERROR_DEVICE_NOT_CONNECTED;
    *p={91,{XINPUT_GAMEPAD_A,22,33,-12000,13000,-14000,15000}};return ERROR_SUCCESS;
}
__declspec(noinline) DWORD WINAPI pad_outer(DWORD slot,XINPUT_STATE* p) {return pad_inner(slot,p);}
LRESULT CALLBACK game_proc(HWND w,UINT id,WPARAM wp,LPARAM lp) {if(id==WM_APP+1 || id==WM_CHAR) ++game_messages;return DefWindowProcW(w,id,wp,lp);}
LRESULT CALLBACK late_proc(HWND w,UINT id,WPARAM wp,LPARAM lp) {if(id==WM_APP+1 || id==WM_CHAR) ++late_messages;return CallWindowProcW(previous,w,id,wp,lp);}
std::uint32_t AL_CALL messages(al_plugin_t*,void*,std::uint32_t id,std::uintptr_t wp,std::intptr_t lp,void*) {
    if(id==WM_CHAR) {++plugin_messages;check(wp=='Z' && lp==12,"message fields");SetLastError(0xbad);return 1;}return 0;
}
}
int main(int argc,char** argv) {
    try {
        WNDCLASSW cls{};cls.lpfnWndProc=game_proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"AL input fake game";
        check(RegisterClassW(&cls)!=0,"class");const auto hwnd=CreateWindowW(cls.lpszClassName,L"AL input fake game",WS_OVERLAPPEDWINDOW,0,0,100,100,nullptr,nullptr,cls.hInstance,nullptr);check(hwnd!=nullptr,"window");
        auto* service=new al::input::Service;service->request();unsigned warnings=0;
        auto* native=new al::input::Native(*service,{},[&](auto){++warnings;});native->attach_window(hwnd);native->begin_frame(GetCurrentThreadId());service->focus(true);
        al::input::Native::Methods methods{{std::bit_cast<void*>(&keyboard),std::bit_cast<void*>(&mouse)},
            {std::bit_cast<void*>(&data_keyboard),std::bit_cast<void*>(&data_mouse)},
            {std::bit_cast<void*>(&pad_outer),std::bit_cast<void*>(&pad_inner),std::bit_cast<void*>(&pad_outer),nullptr}};
        check(native->install(methods) && !warnings,"native input install");
        if(argc>1 && std::strcmp(argv[1],"refusal")==0) {
            al::input::Client c;c.name="refusal fixture";check(service->capture(c,AL_INPUT_ALL).empty(),"pre-refusal capture");
            al::input::Native::Methods invalid{};invalid.pads[0]=reinterpret_cast<void*>(1);
            check(!native->install(invalid) && warnings==1 && !service->capture_mask(),"late installation failure releases all capture visibly");
            std::array<unsigned char,256> bytes{};keyboard(nullptr,256,bytes.data());check(bytes[0x41]!=0,"previous hook falls through after failure");
            al_input_status_t status{};status.size=sizeof(status);service->status(status);check(status.state==AL_INPUT_UNAVAILABLE && status.reason && *status.reason,"reason published");
            DestroyWindow(hwnd);std::cout<<"Late input-hook refusal is visible and restores game input\n";return 0;
        }
        al::input::Client client;client.name="probe";service->messages(client,0,messages,nullptr);
        previous=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd,GWLP_WNDPROC,std::bit_cast<LONG_PTR>(&late_proc)));
        SendMessageW(hwnd,WM_CHAR,'Z',12);SendMessageW(hwnd,WM_APP+1,0,0);
        check(plugin_messages==1 && game_messages==1 && late_messages==2,"later subclass remains in chain, AL consumption stops game");
        std::array<unsigned char,256> keys{};DIMOUSESTATE2 m{};XINPUT_STATE p{};
        check(keyboard(nullptr,256,keys.data())==DI_OK && keys[0x41] && service->key_down(VK_F7) && service->key_down(VK_CONTROL) && GetLastError()==0x1122,"real keyboard snapshot");
        mouse(nullptr,sizeof(m),&m);check(m.lX==15 && service->key_down(VK_LBUTTON),"real mouse snapshot");
        pad_outer(0,&p);al_pad_state_t observed{};observed.size=sizeof(observed);service->pad_state(0,observed);check(observed.buttons==AL_PAD_A && observed.right_y==15000,"real pad snapshot");
        check(service->capture(client,AL_INPUT_ALL).empty(),"capture all");
        keyboard(nullptr,256,keys.data());check(keys[0x41]==0 && service->key_down(VK_F7) && GetLastError()==0x1122,"captured keyboard real state retained");
        mouse(nullptr,sizeof(m),&m);check(m.lX==0 && m.rgbButtons[0]==0 && service->key_down(VK_LBUTTON) && GetLastError()==0x3344,"captured mouse");
        DWORD count=10;DIDEVICEOBJECTDATA events[10]{};check(data_keyboard(nullptr,sizeof(events[0]),events,&count,0)==DI_BUFFEROVERFLOW && !count && GetLastError()==0x5566,"buffered input capture keeps HRESULT and last error");
        count=INFINITE;data_mouse(nullptr,sizeof(events[0]),nullptr,&count,DIGDD_PEEK);check(!count,"buffered count query neutral");
        check(pad_outer(0,&p)==ERROR_SUCCESS && !p.Gamepad.wButtons && p.dwPacketNumber==91 && GetLastError()==0x7788,"captured pad packet preserved");
        service->pad_state(0,observed);check(observed.connected && observed.buttons==AL_PAD_A && observed.left_trigger==22,"nested XInput hooks do not overwrite real snapshot");
        check(pad_outer(2,&p)==ERROR_DEVICE_NOT_CONNECTED,"disconnected status preserved");service->pad_state(2,observed);check(!observed.connected,"disconnected snapshot cleared");
        lost=true;keys.fill(0x5a);check(keyboard(nullptr,256,keys.data())==DIERR_INPUTLOST && keys[0]==0x5a && !service->key_down(VK_F7),"failed read clears snapshot, preserves caller buffer");lost=false;
        std::thread unrelated([&]{std::array<unsigned char,256> other{};keyboard(nullptr,256,other.data());if(!other[0x41]) ++warnings;});unrelated.join();check(!warnings,"other-thread read unchanged");
        SendMessageW(hwnd,WM_KILLFOCUS,0,0);check(!service->capture_mask() && !service->key_down(VK_F7),"focus loss suspends capture and keys");
        keyboard(nullptr,256,keys.data());check(keys[0x41]!=0,"background call unchanged");service->focus(true);check(!service->key_down(VK_F7),"stale foreground snapshot rejected");
        check(service->release(client).empty(),"release");keyboard(nullptr,256,keys.data());check(keys[0x41]!=0,"game input restored");
        // Six real hook entries per synthetic frame, capture closed. Includes
        // MinHook and snapshot bookkeeping, not Windows device-driver latency.
        const auto before=allocations.load();LARGE_INTEGER start{},finish{},frequency{};QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
        for(unsigned i=0;i<25000;++i) {keyboard(nullptr,256,keys.data());mouse(nullptr,sizeof(m),&m);for(DWORD slot=0;slot<4;++slot) pad_outer(slot,&p);service->end_frame();}
        QueryPerformanceCounter(&finish);check(allocations.load()==before,"steady native reads allocate nothing");
        const auto us=static_cast<double>(finish.QuadPart-start.QuadPart)*1000000.0/static_cast<double>(frequency.QuadPart)/25000.0;
        std::cout<<"synthetic_input six_reads_us="<<us<<" allocations=0 (includes stand-in originals; not MHW)\n";
        service->stop();keyboard(nullptr,256,keys.data());pad_outer(0,&p);check(keys[0x41] && p.Gamepad.wButtons==AL_PAD_A,"hooks fall through after exit");
        DestroyWindow(hwnd);check(service->window()==nullptr,"destroy reaches original and clears window");
        std::cout<<"Fake game: real window chain, DirectInput state/data, forwarded XInput, capture, failures and thread isolation passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
