#include "core/input/native.hpp"
#include <windows.h>
#include <xinput.h>
#include <bit>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <new>
#include <vector>

std::atomic<std::uint64_t> allocations{0};
void* operator new(std::size_t n) {++allocations;if(auto p=std::malloc(n ? n:1)) return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}

namespace {
void check(bool value,const char* why) {if(!value) throw std::runtime_error(why);}
unsigned game_messages=0,plugin_messages=0,late_messages=0;
WNDPROC previous{};
// Stand-ins with the game's layouts (sites.hpp). Each computes edges from
// what the detour left it last frame, as the game does.
struct Keyboard { std::uint8_t head[al::input::kKeyMaps]; std::uint32_t maps[6][8]; } keyboard_object{};
enum { Held, Previous, Pressed, Released, Changed, Repeat };
std::array<std::uint32_t,8> keys_now{};
struct Mouse { std::int32_t at[4]; std::int32_t unused; std::int32_t move[3]; std::uint32_t buttons; } mouse_out{};
std::uint32_t buttons_now{}, mouse_previous{}, mouse_pressed{};
struct Slot { std::uint8_t head[al::input::kPadTriggerLevel]; std::uint8_t level; } pad_slot{};
XINPUT_STATE pad_seen{};
WORD pad_previous{}, pad_pressed{};
__declspec(noinline) std::uintptr_t keyboard_update(void* p) {
    auto& maps=static_cast<Keyboard*>(p)->maps;
    for(unsigned i=0;i<8;++i) {
        const auto old=maps[Held][i], held=keys_now[i];
        maps[Previous][i]=old; maps[Held][i]=held; maps[Changed][i]=old^held;
        maps[Pressed][i]=held&(old^held); maps[Released][i]=old&(old^held); maps[Repeat][i]=held;
    }
    SetLastError(0x1122); return 0x5150;
}
__declspec(noinline) std::uintptr_t mouse_read(void*,void* out) {
    auto& m=*static_cast<Mouse*>(out); m.at[0]=640; m.move[0]=15; m.move[1]=-12; m.move[2]=120; m.buttons=buttons_now;
    SetLastError(0x3344); return 0x6160;
}
__declspec(noinline) std::uintptr_t pad_update(void*,void*,const void* state) {
    std::memcpy(&pad_seen,state,sizeof(pad_seen));
    pad_pressed=static_cast<WORD>(pad_seen.Gamepad.wButtons&~pad_previous); pad_previous=pad_seen.Gamepad.wButtons;
    SetLastError(0x7788); return 0x7170;
}
std::string signature(void* target) {
    std::string text;
    for(unsigned i=0;i<16;++i) {char byte[4]{};std::snprintf(byte,sizeof(byte),"%02X",static_cast<const unsigned char*>(target)[i]);if(i) text+=' ';text+=byte;}
    return text;
}
// Stands in for MinHook: the registry's chain head is the patched entry.
struct Installer : al::game_hooks::Installer {
    std::vector<std::pair<void*,void* const*>> heads;
    std::string create(void* target,void* const* head,void** original) override {heads.push_back({target,head});*original=target;return {};}
    std::string enable(void*) override {return {};}
    template<class F> F entry(F target) const {for(auto [t,h]:heads) if(t==std::bit_cast<void*>(target)) return std::bit_cast<F>(*h);return target;}
};
LRESULT CALLBACK game_proc(HWND w,UINT id,WPARAM wp,LPARAM lp) {if(id==WM_APP+1 || id==WM_CHAR) ++game_messages;return DefWindowProcW(w,id,wp,lp);}
LRESULT CALLBACK late_proc(HWND w,UINT id,WPARAM wp,LPARAM lp) {if(id==WM_APP+1 || id==WM_CHAR) ++late_messages;return CallWindowProcW(previous,w,id,wp,lp);}
std::uint32_t AL_CALL messages(al_plugin_t*,void*,std::uint32_t id,std::uintptr_t wp,std::intptr_t lp,void*) {
    if(id==WM_CHAR) {++plugin_messages;check(wp=='Z' && lp==12,"message fields");SetLastError(0xbad);return 1;}return 0;
}
bool bit(const std::uint32_t* map,unsigned key) {return (map[key/32]>>(key%32))&1;}
void set(unsigned key,bool down) {if(down) keys_now[key/32]|=1u<<(key%32); else keys_now[key/32]&=~(1u<<(key%32));}
}
int main(int argc,char** argv) {
    try {
        const std::string mode=argc>1 ? argv[1]:"";
        WNDCLASSW cls{};cls.lpfnWndProc=game_proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"AL input fake game";
        check(RegisterClassW(&cls)!=0,"class");const auto hwnd=CreateWindowW(cls.lpszClassName,L"AL input fake game",WS_OVERLAPPEDWINDOW,0,0,100,100,nullptr,nullptr,cls.hInstance,nullptr);check(hwnd!=nullptr,"window");
        // Like the production runtime, everything stays alive until exit.
        auto* service=new al::input::Service;unsigned warnings=0;
        if(mode!="late") service->request();
        auto* native=new al::input::Native(*service,{},[&](auto){++warnings;});native->attach_window(hwnd);
        al::input::Sites sites{{std::bit_cast<void*>(&keyboard_update),signature(std::bit_cast<void*>(&keyboard_update)),0},
            {std::bit_cast<void*>(&mouse_read),signature(std::bit_cast<void*>(&mouse_read)),0},
            {std::bit_cast<void*>(&pad_update),signature(std::bit_cast<void*>(&pad_update)),0},{}};
        if(mode=="refusal") sites.mouse.pattern="00 00 00 00 00 00";
        if(mode=="unavailable") sites.problem="fixture identity refused";
        auto* registry=new al::game_hooks::Registry(GetModuleHandleW(nullptr),{});auto* installer=new Installer;
        native->prepare(*registry,std::move(sites));registry->set_installer(installer);native->installed();
        al_input_status_t status{};status.size=sizeof(status);
        if(mode=="unavailable") {
            service->status(status);check(status.state==AL_INPUT_UNAVAILABLE && std::string(status.reason)=="fixture identity refused" && warnings==1 && installer->heads.empty(),"refused identity is visible, nothing hooked");
            std::cout<<"Unverified input sites leave the game unhooked and say why\n";return 0;
        }
        if(mode=="late") {
            service->status(status);check(installer->heads.empty() && status.state==AL_INPUT_WAITING,"nothing hooked before a plugin asks");
            service->request();native->begin_frame(GetCurrentThreadId());service->status(status);
            check(installer->heads.size()==3 && status.state==AL_INPUT_RUNNING && status.devices==AL_INPUT_ALL && !warnings,"a later request hooks at the next frame");
            std::cout<<"A plugin asking after PRE_MAIN gets the game sites at the next frame\n";return 0;
        }
        native->begin_frame(GetCurrentThreadId());service->focus(true);
        const auto kb=installer->entry(&keyboard_update);const auto ms=installer->entry(&mouse_read);const auto pad=installer->entry(&pad_update);
        auto mouse_frame=[&]{const auto old=mouse_out.buttons;mouse_out=Mouse{};check(ms(nullptr,&mouse_out)==0x6160,"mouse result");mouse_pressed=mouse_out.buttons&~old;mouse_previous=old;};
        XINPUT_STATE real{91,{XINPUT_GAMEPAD_A,22,33,-12000,13000,-14000,15000}};pad_slot.level=30;
        auto frame=[&](bool with_pad=true,bool with_mouse=true){
            check(kb(&keyboard_object)==0x5150 && GetLastError()==0x1122,"keyboard result and last error");
            if(with_mouse) mouse_frame();
            if(with_pad) check(pad(nullptr,&pad_slot,&real)==0x7170,"pad result");
            native->end_frame();service->end_frame();
        };
        service->status(status);
        if(mode=="refusal") {
            check(status.state==AL_INPUT_RUNNING && status.devices==(AL_INPUT_KEYBOARD|AL_INPUT_PAD) && warnings==1 && std::string(status.reason).find("mouse:")==0,"partial hooks run with the reason");
            al::input::Client c;c.name="refusal fixture";
            check(!service->capture(c,AL_INPUT_MOUSE).empty() && service->capture(c,AL_INPUT_KEYBOARD).empty(),"capture needs every requested path");
            set('A',true);frame();check(!bit(keyboard_object.maps[Held],'A') && service->key_down('A'),"installed paths still capture");
            std::cout<<"A refused input site is visible; the other devices keep working\n";return 0;
        }
        check(status.state==AL_INPUT_RUNNING && status.devices==AL_INPUT_ALL && !warnings,"all three game sites active");
        al::input::Client client;client.name="probe";service->messages(client,0,messages,nullptr);
        previous=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd,GWLP_WNDPROC,std::bit_cast<LONG_PTR>(&late_proc)));
        SendMessageW(hwnd,WM_CHAR,'Z',12);SendMessageW(hwnd,WM_APP+1,0,0);
        check(plugin_messages==1 && game_messages==1 && late_messages==2,"later subclass remains in chain, AL consumption stops game");
        // Real state reaches plugins and the game alike.
        set('A',true);set(VK_CONTROL,true);set(VK_LBUTTON,true);buttons_now=1;frame();
        check(bit(keyboard_object.maps[Pressed],'A') && service->key_down('A') && service->key_down(VK_CONTROL) && service->key_down(VK_LBUTTON),"real keyboard snapshot");
        check(mouse_out.move[0]==15 && mouse_pressed==1,"real mouse");
        al_pad_state_t observed{};observed.size=sizeof(observed);service->pad_state(0,observed);
        check(observed.connected && observed.buttons==AL_PAD_A && observed.left_trigger==22 && observed.right_y==15000 && pad_seen.Gamepad.wButtons==XINPUT_GAMEPAD_A,"real pad snapshot");
        // Keyboard capture leaves the mouse buttons in the key maps alone.
        check(service->capture(client,AL_INPUT_KEYBOARD).empty(),"capture keyboard");frame();
        check(!bit(keyboard_object.maps[Held],'A') && !bit(keyboard_object.maps[Previous],'A') && !bit(keyboard_object.maps[Released],'A') && bit(keyboard_object.maps[Held],VK_LBUTTON),"keyboard capture");
        check(service->key_down('A') && mouse_out.move[0]==15,"captured keyboard keeps real state, mouse untouched");
        // Mouse capture also takes the mouse buttons out of the key maps.
        check(service->capture(client,AL_INPUT_MOUSE|AL_INPUT_PAD).empty(),"capture mouse and pad");frame();
        check(!bit(keyboard_object.maps[Held],VK_LBUTTON) && !mouse_out.buttons && !mouse_out.move[0] && !mouse_out.move[2] && mouse_out.at[0]==640,"mouse capture keeps the cursor position");
        check(service->key_down(VK_LBUTTON),"captured mouse keeps real state");
        check(!pad_seen.Gamepad.wButtons && !pad_seen.Gamepad.bLeftTrigger && !pad_seen.Gamepad.sThumbRY && pad_seen.dwPacketNumber==91,"captured pad keeps the packet");
        service->pad_state(0,observed);check(observed.buttons==AL_PAD_A && observed.left_trigger==22,"captured pad keeps real snapshot");
        // Held through the release: hidden until let go, never a fresh press.
        real.Gamepad.bRightTrigger=200;frame();
        check(service->release(client).empty() && service->release(client).empty() && !service->capture_mask(),"release all");
        set('B',true);frame();
        check(!bit(keyboard_object.maps[Held],'A') && !bit(keyboard_object.maps[Pressed],'A') && bit(keyboard_object.maps[Pressed],'B'),"held key hidden, new key passes");
        check(!bit(keyboard_object.maps[Held],VK_LBUTTON) && !mouse_out.buttons && !mouse_pressed && mouse_out.move[0]==15,"held button hidden, movement back");
        check(!pad_seen.Gamepad.wButtons && !pad_seen.Gamepad.bRightTrigger && !pad_pressed && pad_seen.Gamepad.bLeftTrigger==22 && pad_seen.Gamepad.sThumbRY==15000,"held pad button and trigger hidden, sticks back");
        set('A',false);set(VK_LBUTTON,false);buttons_now=0;real.Gamepad.wButtons=0;real.Gamepad.bRightTrigger=0;frame();
        check(!bit(keyboard_object.maps[Released],'A') && !mouse_out.buttons,"no late release edge");
        set('A',true);set(VK_LBUTTON,true);buttons_now=1;real.Gamepad.wButtons=XINPUT_GAMEPAD_A;real.Gamepad.bRightTrigger=200;frame();
        check(bit(keyboard_object.maps[Pressed],'A') && bit(keyboard_object.maps[Pressed],VK_LBUTTON) && mouse_pressed==1 && pad_pressed==XINPUT_GAMEPAD_A && pad_seen.Gamepad.bRightTrigger==200,"pressed again after release");
        // Devices the game did not read this frame read as idle.
        frame(false,false);service->pad_state(0,observed);check(!observed.connected && !service->key_down(VK_LBUTTON) && service->key_down('A'),"unread pad and mouse idle");
        frame();service->pad_state(0,observed);check(observed.connected,"pad back");
        // Only the frame thread's calls participate.
        check(service->capture(client,AL_INPUT_ALL).empty(),"capture all");
        std::thread unrelated([&]{Keyboard other{};kb(&other);if(!bit(other.maps[Held],'A')) ++warnings;});unrelated.join();check(!warnings,"other-thread call unchanged");
        SendMessageW(hwnd,WM_KILLFOCUS,0,0);check(!service->capture_mask() && !service->key_down('A'),"focus loss suspends capture and keys");
        frame();check(bit(keyboard_object.maps[Held],'B'),"background frame reaches the game");service->focus(true);check(!service->key_down('A'),"stale foreground snapshot rejected");
        frame();check(service->key_down('A') && !bit(keyboard_object.maps[Held],'B'),"capture resumes with focus");
        check(service->release(client).empty(),"release");
        // Synthetic frames with capture closed: six detour calls and both
        // end-of-frame passes. Not an MHW measurement.
        const auto before=allocations.load();LARGE_INTEGER start{},finish{},frequency{};QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
        for(unsigned i=0;i<25000;++i) {kb(&keyboard_object);ms(nullptr,&mouse_out);for(unsigned k=0;k<4;++k) pad(nullptr,&pad_slot,&real);native->end_frame();service->end_frame();}
        QueryPerformanceCounter(&finish);check(allocations.load()==before,"steady frames allocate nothing");
        const auto us=static_cast<double>(finish.QuadPart-start.QuadPart)*1000000.0/static_cast<double>(frequency.QuadPart)/25000.0;
        std::cout<<"synthetic_input six_calls_us="<<us<<" allocations=0 (stand-in game functions, no patching; not MHW)\n";
        service->stop();check(service->capture(client,AL_INPUT_ALL).empty()==false,"stopped input refuses capture");frame();check(bit(keyboard_object.maps[Held],'A'),"hooks fall through after exit");
        DestroyWindow(hwnd);check(service->window()==nullptr,"destroy reaches original and clears window");
        std::cout<<"Fake game: game-site keyboard, mouse and pad capture, held-input hiding, idle unread devices, window chain, thread and focus isolation passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
