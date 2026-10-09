#include "core/input/native.hpp"
#include "common/windows.hpp"
#include <windows.h>
#include <xinput.h>
#include <array>
#include <bit>
#include <cstring>

namespace al::input {
namespace {
Native* active{};
using KeyboardUpdate = std::uintptr_t(*)(void*);
using MouseRead = std::uintptr_t(*)(void*, void*);
using PadUpdate = std::uintptr_t(*)(void*, void*, const void*);
using GetLong = LONG_PTR(WINAPI*)(HWND,int);
using SetLong = LONG_PTR(WINAPI*)(HWND,int,LONG_PTR);
using CallProc = LRESULT(WINAPI*)(WNDPROC,HWND,UINT,WPARAM,LPARAM);
using Foreground = HWND(WINAPI*)();
using WindowThread = DWORD(WINAPI*)(HWND,DWORD*);
using Ancestor = HWND(WINAPI*)(HWND,UINT);
template<class T> T proc(HMODULE m, const char* name) { return m ? std::bit_cast<T>(GetProcAddress(m, name)) : nullptr; }
// The keyboard update adds the mouse buttons to its key maps (GetAsyncKeyState,
// VK 1, 2, 4, 5, 6): they belong to the mouse for capture.
constexpr std::uint32_t kMouseKeys = 1u<<VK_LBUTTON | 1u<<VK_RBUTTON | 1u<<VK_MBUTTON | 1u<<VK_XBUTTON1 | 1u<<VK_XBUTTON2;
constexpr std::uint32_t kMouseSeen = 1u << 4; // bits 0..3: pads seen this frame
constexpr const char* kNames[]{"Keyboard:Update","Mouse:Read","Pad:XInput"};
constexpr const char* kDevices[]{"keyboard","mouse","pad"};
constexpr std::uint32_t kBits[]{AL_INPUT_KEYBOARD,AL_INPUT_MOUSE,AL_INPUT_PAD};
}
struct Native::State {
    Service* input{};
    Log log, warning;
    game_hooks::Registry* registry{};
    game_hooks::Client client;
    Sites sites;
    std::array<std::uint32_t,3> handles{};
    std::array<void*,3> next{};
    std::array<std::string,3> problems;
    bool registered{}, reported{};
    std::atomic<std::uint32_t> thread{0};
    std::atomic<bool> failed{false};
    // Frame thread only. What capture hid stays hidden until released, so a
    // key still held when capture ends does not reach the game as a new press.
    std::array<std::uint32_t,8> hidden_keys{};
    std::uint32_t hidden_mouse{}, seen{}, connected{};
    std::array<std::uint16_t,4> hidden_buttons{};
    std::array<std::uint8_t,4> hidden_triggers{};
    GetLong get_long{}; SetLong set_long{}; std::atomic<CallProc> call{nullptr}; Foreground foreground{}; WindowThread window_thread{}; Ancestor ancestor{};
    std::mutex user32_mutex;
    struct Window { std::atomic<HWND> hwnd{nullptr}; std::atomic<WNDPROC> previous{nullptr}; };
    std::array<Window,4> windows{};
    explicit State(Service& service, Log logger, Log warn) : input(&service), log(std::move(logger)), warning(std::move(warn)) {
        client.name="AsteraLoader"; client.log=[this](std::string_view level,std::string_view text){note(std::string(level)+" "+std::string(text));};
    }
    void note(std::string_view text) noexcept { try { if(log) log(text); } catch (...) {} }
    bool game_thread() const noexcept { return thread.load(std::memory_order_relaxed) == GetCurrentThreadId(); }
    // Off the frame thread, after a failure and outside RUNNING (EXITING
    // included) the hooks only pass the call on.
    bool live() const noexcept { return game_thread() && !failed.load(std::memory_order_relaxed) && input->running(); }
    bool user32() {
        std::lock_guard lock(user32_mutex);
        if (call) return true;
        const auto m = GetModuleHandleW(L"user32.dll");
        get_long=proc<GetLong>(m,"GetWindowLongPtrW"); set_long=proc<SetLong>(m,"SetWindowLongPtrW");
        foreground=proc<Foreground>(m,"GetForegroundWindow"); window_thread=proc<WindowThread>(m,"GetWindowThreadProcessId"); ancestor=proc<Ancestor>(m,"GetAncestor");
        if (!get_long || !set_long || !foreground || !window_thread || !ancestor) return false;
        call=proc<CallProc>(m,"CallWindowProcW"); return call.load() != nullptr;
    }
};
Native::Native(Service& service, Log log, Log warning) : state_(new State(service,std::move(log),std::move(warning))) { active=this; }
void Native::fail(std::string_view reason) noexcept {
    if (state_->failed.exchange(true)) return;
    try { state_->input->unavailable(std::string(reason)); state_->note("input_hook_unavailable reason="+std::string(reason)); if(state_->warning) state_->warning(reason); } catch (...) {}
}
void Native::prepare(game_hooks::Registry& registry, Sites sites) noexcept {
    try { state_->registry=&registry; state_->sites=std::move(sites); if(state_->input->wanted()) hook(); }
    catch (...) { fail("input hook preparation failed"); }
}
void Native::hook() noexcept {
    auto& s=*state_;
    if(s.registered || s.failed) return;
    s.registered=true;
    try {
        if(!s.registry) { fail("the game's input sites were not prepared"); return; }
        if(!s.sites.problem.empty()) { fail(s.sites.problem); return; }
        const std::array<const frame::Site*,3> sites{&s.sites.keyboard,&s.sites.mouse,&s.sites.pad};
        const std::array<void*,3> detours{std::bit_cast<void*>(&keyboard_update),std::bit_cast<void*>(&mouse_read),std::bit_cast<void*>(&pad_update)};
        for(unsigned k=0;k<3;++k) {
            // Outermost, like the frame layer: other detours see the real
            // input, and capture covers whatever they leave for the game.
            const al_hook_t hook{sizeof(hook),-500,kNames[k],sites[k]->pattern.c_str(),sites[k]->offset,0,sites[k]->target,detours[k],&s.next[k]};
            if(!sites[k]->target) s.problems[k]="no site";
            else s.handles[k]=s.registry->hook(s.client,hook,s.problems[k]);
        }
    } catch (...) { fail("input hook registration failed"); }
}
void Native::installed() noexcept { report(); }
void Native::report() noexcept {
    auto& s=*state_;
    if(!s.registered || s.reported || s.failed) return;
    s.reported=true;
    try {
        std::uint32_t devices=0; std::string missing;
        for(unsigned k=0;k<3;++k) {
            if(s.handles[k] && s.registry->state(s.client,s.handles[k])==AL_HOOK_ACTIVE) { devices|=kBits[k]; continue; }
            missing+=(missing.empty() ? "":"; ")+std::string(kDevices[k])+": "+(s.problems[k].empty() ? "its hook is not active":s.problems[k]);
        }
        if(!devices) { fail(missing); return; }
        if(!missing.empty()) { s.input->missing(missing); s.note("input_hooks_missing "+missing); if(s.warning) s.warning(missing); }
        s.input->ready(devices); s.note("input_hooks_ready devices="+std::to_string(devices)+" sites=game");
    } catch (...) { fail("input hook report failed"); }
}
void Native::begin_frame(std::uint32_t thread) noexcept {
    state_->thread.store(thread,std::memory_order_relaxed);
    if(!state_->input->wanted()) return;
    if(!state_->registered) { hook(); report(); } // a plugin asked after PRE_MAIN
    if(state_->call) {
        const auto window=static_cast<HWND>(state_->input->window());
        state_->input->focus(window && state_->ancestor(state_->foreground(),GA_ROOT)==window);
    }
}
void Native::end_frame() noexcept {
    // Devices the game did not read this frame are idle: the mouse read is
    // skipped in some states, and a pad XInput reports missing never reaches
    // the pad function.
    auto& s=*state_;
    if(!s.game_thread()) return;
    if(!(s.seen&kMouseSeen)) s.input->mouse(0);
    for(unsigned user=0;user<4;++user) {
        const auto bit=1u<<user;
        if(!(s.connected&bit) || (s.seen&bit)) continue;
        al_pad_state_t idle{}; idle.size=sizeof(idle); s.input->pad(user,idle);
        s.connected&=~bit; s.hidden_buttons[user]=0; s.hidden_triggers[user]=0;
    }
    s.seen=0;
}
std::uintptr_t Native::keyboard_update(void* keyboard) {
    auto& s=*active->state_;
    const auto result=std::bit_cast<KeyboardUpdate>(s.next[0])(keyboard);
    if(!keyboard || !s.live()) return result;
    // held, previous, pressed, released, changed, repeat: eight dwords each.
    const auto maps=reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(keyboard)+kKeyMaps);
    s.input->keyboard(maps);
    const auto mask=s.input->capture_mask();
    for(unsigned i=0;i<8;++i) {
        std::uint32_t blocked=(mask&AL_INPUT_KEYBOARD) ? ~0u:0u;
        if(i==0) blocked=(blocked&~kMouseKeys) | ((mask&AL_INPUT_MOUSE) ? kMouseKeys:0u);
        s.hidden_keys[i]=maps[i]&(s.hidden_keys[i]|blocked);
        const auto hide=blocked|s.hidden_keys[i];
        if(hide) for(unsigned map=0;map<6;++map) maps[map*8+i]&=~hide;
    }
    return result;
}
std::uintptr_t Native::mouse_read(void* mouse, void* out) {
    auto& s=*active->state_;
    const auto result=std::bit_cast<MouseRead>(s.next[1])(mouse,out);
    if(!out || !s.live()) return result;
    const auto state=static_cast<std::uint8_t*>(out);
    std::uint32_t buttons{}; std::memcpy(&buttons,state+kMouseButtons,sizeof(buttons));
    s.input->mouse(buttons); s.seen|=kMouseSeen;
    const auto captured=(s.input->capture_mask()&AL_INPUT_MOUSE)!=0;
    s.hidden_mouse=buttons&(s.hidden_mouse|(captured ? ~0u:0u));
    if(captured) std::memset(state+kMouseMove,0,3*sizeof(std::int32_t)); // the cursor position stays
    buttons&=~s.hidden_mouse; std::memcpy(state+kMouseButtons,&buttons,sizeof(buttons));
    return result;
}
std::uintptr_t Native::pad_update(void* manager, void* slot, const void* input) {
    auto& s=*active->state_;
    const auto next=std::bit_cast<PadUpdate>(s.next[2]);
    if(!slot || !input || !s.live()) return next(manager,slot,input);
    std::uint32_t user{}; std::memcpy(&user,static_cast<const std::uint8_t*>(slot)+kPadUser,sizeof(user));
    if(user>=4) return next(manager,slot,input);
    XINPUT_STATE copy{}; std::memcpy(&copy,input,sizeof(copy));
    al_pad_state_t snapshot{}; snapshot.size=sizeof(snapshot); snapshot.connected=1; snapshot.packet=copy.dwPacketNumber;
    std::memcpy(&snapshot.buttons,&copy.Gamepad,sizeof(copy.Gamepad));
    s.input->pad(user,snapshot); s.seen|=1u<<user; s.connected|=1u<<user;
    const auto captured=(s.input->capture_mask()&AL_INPUT_PAD)!=0;
    // Triggers count as pressed past the game's own threshold for this slot.
    const auto level=*(static_cast<const std::uint8_t*>(slot)+kPadTriggerLevel);
    const unsigned pulled=(copy.Gamepad.bLeftTrigger>level ? 1u:0u) | (copy.Gamepad.bRightTrigger>level ? 2u:0u);
    s.hidden_buttons[user]=static_cast<std::uint16_t>(copy.Gamepad.wButtons&(s.hidden_buttons[user]|(captured ? 0xffffu:0u)));
    s.hidden_triggers[user]=static_cast<std::uint8_t>(pulled&(s.hidden_triggers[user]|(captured ? 3u:0u)));
    if(captured) copy.Gamepad={}; // packet number kept
    else {
        copy.Gamepad.wButtons&=static_cast<WORD>(~s.hidden_buttons[user]);
        if(s.hidden_triggers[user]&1) copy.Gamepad.bLeftTrigger=0;
        if(s.hidden_triggers[user]&2) copy.Gamepad.bRightTrigger=0;
    }
    return next(manager,slot,&copy);
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
