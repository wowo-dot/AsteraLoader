#include "core/input/service.hpp"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>

std::atomic<std::uint64_t> allocations{0};
std::atomic<int> fail_after{-1};
void* operator new(std::size_t n) {
    ++allocations;
    if(fail_after.load()>=0 && fail_after.fetch_sub(1)==0) {fail_after=-1;throw std::bad_alloc();}
    if(auto p=std::malloc(n ? n:1)) return p; throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
namespace {
void check(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
void AL_CALL key(al_plugin_t*,std::uint32_t,void* user) { ++*static_cast<unsigned*>(user); }
void AL_CALL bad(al_plugin_t*,std::uint32_t,void*) { throw std::runtime_error("fixture"); }
void AL_CALL access_fault(al_plugin_t*,std::uint32_t,void*) { RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr); }
struct Message { std::vector<unsigned>* order; unsigned index; bool consume; };
std::uint32_t AL_CALL message(al_plugin_t*,void*,std::uint32_t,std::uintptr_t,std::intptr_t,void* p) {
    auto& item=*static_cast<Message*>(p); item.order->push_back(item.index); return item.consume;
}
// The game's key maps: 256 virtual-key bits in eight dwords.
void keys(al::input::Service& s,const std::array<std::uint8_t,256>& raw) {
    std::array<std::uint32_t,8> held{};for(unsigned k=0;k<256;++k) if(raw[k]&0x80) held[k/32]|=1u<<(k%32);s.keyboard(held.data());
}
std::uint32_t AL_CALL message_fault(al_plugin_t*,void*,std::uint32_t,std::uintptr_t,std::intptr_t,void*) { throw 1; }
}
int main() {
    try {
        al::input::Service s;
        std::atomic<bool> a_stopped{false},b_stopped{false}; unsigned conflicts=0,disabled=0;
        al::input::Client a,b; a.name="a"; b.name="b"; a.load_index=0; b.load_index=1;
        a.stopped=&a_stopped; b.stopped=&b_stopped;
        a.log=[&](auto level,auto){if(level=="WARN") ++conflicts;}; b.log=a.log;
        a.disable=[&](auto){a_stopped=true;++disabled;}; b.disable=[&](auto){b_stopped=true;++disabled;};
        s.ready(AL_INPUT_ALL); s.focus(true);
        std::array<std::uint8_t,256> raw{};
        std::string why; unsigned a_calls=0,b_calls=0,pad_calls=0;
        const auto ka=s.add(a,'M',AL_MOD_CTRL,"menu",key,&a_calls,why);
        const auto kb=s.add(b,'M',AL_MOD_CTRL,"other",key,&b_calls,why);
        check(ka && kb && conflicts==2,"duplicate combinations reported to both plugins");
        check(!s.add(a,256,0,"",key,nullptr,why) && !why.empty(),"invalid key refused");
        check(!s.remove(b,ka).empty(),"foreign hotkey handle refused");
        raw['M']=0x80; keys(s,raw); s.end_frame(); check(!a_calls,"required modifier");
        raw[VK_LCONTROL]=0x80; keys(s,raw); s.end_frame(); s.end_frame();
        check(a_calls==1 && b_calls==1 && s.key_down(VK_CONTROL),"combination edge and generic modifiers");
        check(s.capture(a,AL_INPUT_KEYBOARD).empty() && s.capture(a,AL_INPUT_MOUSE).empty(),"nested capture");
        check(s.capture_mask()==3 && s.release(a).empty() && s.capture_mask()==1,"LIFO release keeps keyboard capture");
        raw['M']=0; keys(s,raw); s.end_frame(); raw['M']=0x80; keys(s,raw); s.end_frame();
        check(a_calls==2 && b_calls==1,"own capture permits hotkey, another owner's suppresses it");
        check(s.release(a).empty() && !s.release(a).empty(),"balanced capture refuses over-release");
        s.focus(false); check(!s.key_down('M') && !s.capture_mask(),"background neutral");
        s.end_frame(); s.focus(true); check(!s.key_down('M'),"stale snapshot not reused on focus gain");
        raw.fill(0); keys(s,raw); s.end_frame(); raw['M']=raw[VK_LCONTROL]=0x80; keys(s,raw); s.end_frame();
        check(a_calls==3 && b_calls==2,"fresh read restores hotkeys");
        const auto pk=s.add(a,AL_PAD_A|AL_PAD_B,0,"pad",key,&pad_calls,why,2); check(pk!=0,"pad hotkey registration");
        al_pad_state_t pad{}; pad.size=sizeof(pad); pad.connected=1; pad.packet=17; pad.buttons=AL_PAD_A;
        pad.left_trigger=12; pad.right_trigger=220; pad.left_x=-20000; pad.left_y=23000; pad.right_x=-123; pad.right_y=456;
        s.pad(2,pad); s.end_frame(); check(!pad_calls,"all pad buttons required");
        pad.buttons|=AL_PAD_B; s.pad(2,pad); s.end_frame(); s.end_frame(); check(pad_calls==1,"pad combination edge");
        al_pad_state_t out{}; out.size=sizeof(out); check(s.pad_state(2,out) && out.packet==17 && out.right_trigger==220 && out.right_x==-123,"pad fields preserved");
        out.size=8; out.packet=99; check(s.pad_state(2,out) && out.size==8 && out.packet==99,"bounded pad query");
        check(!s.pad_state(4,out),"invalid slot refused");
        std::vector<unsigned> order; Message ma{&order,1,false},mb{&order,2,true};
        s.messages(a,10,message,&ma); s.messages(b,-1,message,&mb);
        check(s.message(nullptr,WM_CHAR,1,2) && order==std::vector<unsigned>{2},"ordered consumption");
        s.messages(b,20,message,&mb); order.clear();
        check(s.message(nullptr,WM_CHAR,1,2) && order==std::vector<unsigned>({1,2}),"callback replacement");
        s.messages(b,0,nullptr,nullptr); order.clear(); check(!s.message(nullptr,WM_CHAR,1,2) && order.size()==1,"unsubscribe");
        s.capture(b,AL_INPUT_ALL); s.messages(b,-1,message_fault,nullptr); order.clear();
        check(!s.message(nullptr,WM_CHAR,1,2) && disabled==1 && s.capture_mask()==0 && order.size()==1,"message fault disables and releases capture, forwards");
        s.add(a,'Q',0,"bad",bad,nullptr,why); s.capture(a,AL_INPUT_ALL); raw['Q']=0x80; keys(s,raw); s.end_frame();
        check(disabled==2 && s.capture_mask()==0,"C++ hotkey fault releases capture");
        a_stopped=false; s.add(a,'R',0,"AV",access_fault,nullptr,why); raw['R']=0x80; keys(s,raw); s.end_frame();
        check(disabled==3 && a_stopped,"native hotkey fault isolated");
        // Every field in a concurrent pad query must belong to one publication.
        std::atomic<bool> done{false},coherent{true};
        std::thread reader([&]{while(!done) {al_pad_state_t p{};p.size=sizeof(p);s.pad_state(0,p);if(p.connected && (p.buttons!=static_cast<std::uint16_t>(p.packet) || p.right_x!=static_cast<std::int16_t>(p.packet))) coherent=false;}});
        for(unsigned i=1;i<10000;++i) {pad.packet=i;pad.buttons=static_cast<std::uint16_t>(i);pad.right_x=static_cast<std::int16_t>(i);s.pad(0,pad);} done=true;reader.join(); check(coherent,"pad snapshots coherent across threads");
        s.stop(); check(!s.capture_mask() && !s.capture(a,1).empty() && !s.key_down('M'),"exit releases input");
        al::input::Service idle; idle.ready(AL_INPUT_ALL); idle.focus(true); idle.end_frame();
        const auto before=allocations.load(); for(unsigned i=0;i<100000;++i) idle.end_frame(); check(allocations.load()==before,"idle frames allocate nothing");
        al::input::Service failed; failed.unavailable("fixture refusal"); al_input_status_t status{};status.size=sizeof(status);failed.status(status);
        check(status.state==AL_INPUT_UNAVAILABLE && std::string(status.reason)=="fixture refusal" && !failed.capture(a,1).empty(),"visible unavailable status");
        // A failed registration must not appear in a later publication. A
        // failed message replacement must leave its previous callback intact.
        for(int step=0;step<12;++step) {
            al::input::Service f;al::input::Client c;c.name="allocation fixture";unsigned calls=0;std::uint32_t handle=0;
            f.ready(AL_INPUT_ALL);f.focus(true);std::vector<unsigned> trace;Message old{&trace,1,false},fresh{&trace,2,false};f.messages(c,0,message,&old);f.end_frame();
            bool refused=false;fail_after=step;
            try{f.messages(c,0,message,&fresh);}catch(const std::bad_alloc&){refused=true;}fail_after=-1;
            f.message(nullptr,WM_CHAR,0,0);check(trace==std::vector<unsigned>{refused ? 1u:2u},"failed replacement retains old callback");
            refused=false;fail_after=step;
            try{handle=f.add(c,'S',0,"",key,&calls,why);}catch(const std::bad_alloc&){refused=true;}fail_after=-1;
            // Force another publication after the refusal.
            f.messages(c,0,nullptr,nullptr);raw.fill(0);raw['S']=0x80;keys(f,raw);f.end_frame();
            check(refused ? calls==0:handle && calls==1,"failed hotkey registration never becomes active later");
            fail_after=step;try{f.capture(c,AL_INPUT_ALL);}catch(const std::bad_alloc&){}fail_after=-1;
            // Retry must publish a usable owner if its first allocation failed.
            f.cancel(c);check(f.capture(c,AL_INPUT_KEYBOARD).empty() && f.capture_mask()==AL_INPUT_KEYBOARD,"capture owner survives allocation refusal and retry");
        }
        std::cout<<"Input edges, modifiers, pad snapshots, conflicts, capture, messages, faults and empty-frame allocation passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
