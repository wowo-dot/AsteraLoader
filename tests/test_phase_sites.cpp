#include "core/phases/sites.hpp"
#include "core/phases/imports.hpp"
#include "core/phases/debug_slot.hpp"
#include <windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <vector>

int main() {
    int failures=0;
    const auto check=[&](bool ok,const char* name) { if (!ok) { std::cerr<<name<<'\n'; ++failures; } };
    std::vector<std::uint8_t> image(4096,0xcc);
    const std::array<std::uint8_t,33> pattern{0xe8,0,0,0,0,0x0f,0xb7,0xd8,0xe8,0,0,0,0,0x4c,0x8b,0xc0,
        0x44,0x8b,0xcb,0x33,0xd2,0x48,0x8d,0x0d,0,0,0,0,0xe8,0,0,0,0};
    std::memcpy(image.data()+512,pattern.data(),33);
    const auto rel=[&](std::size_t at,std::int32_t target) { const auto value=target-static_cast<std::int32_t>(at+4); std::memcpy(image.data()+at,&value,4); };
    rel(513,256); rel(521,300); rel(536,0); rel(541,2048);
    const std::array<al::startup::CodeRange,1> code{{{128,3968}}};
    std::string reason;
    auto find=[&] { return al::phases::find_win_main(image,code,{480,512},reason); };
    const auto original=image;
    auto site=find(); check(site && site->call==540 && site->target==2048,"relative call and image base verified");
    check(image==original,"finding is read-only");
    rel(536,64); check(!find(),"wrong image-base LEA refused"); image=original;
    rel(541,6000); check(!find(),"outside-code WinMain refused"); image=original;
    image[2048]=0xe9; check(find().has_value(),"existing SPL hook at WinMain entry stays compatible"); image=original;
    std::memcpy(image.data()+600,pattern.data(),33); rel(601,256); rel(609,300); rel(624,0); rel(629,2048);
    check(!find(),"ambiguous WinMain call refused"); image=original;
    check(!al::phases::find_win_main(image,code,{4090,33},reason),"truncated common-main refused");
    check(!al::phases::find_win_main(image,code,{480,4097},reason),"unbounded common-main refused");
    auto slot=al::phases::find_import(GetModuleHandleW(nullptr),"kernel32.dll","VirtualProtect");
    check(slot.has_value() && !slot->delayed,"normal import found");
    check(!al::phases::find_import(nullptr,"kernel32.dll","VirtualProtect"),"null PE refused");
    check(!al::phases::find_import(image.data(),"kernel32.dll","VirtualProtect"),"bad PE refused");
    void* value=reinterpret_cast<void*>(1);
    check(!al::phases::swap_slot(&value,reinterpret_cast<void*>(2),nullptr) && value==reinterpret_cast<void*>(1),"slot conflict preserved");
    check(al::phases::swap_slot(&value,reinterpret_cast<void*>(1),nullptr) && !value,"owned slot exchanged");
    CONTEXT context{}; context.Dr0=0x1111; context.Dr7=0x400;
    auto saved=al::phases::reserve_debug_slot(context,0x2222);
    check(saved && saved->index==0 && context.Dr0==0x2222 && context.Dr7==0x401,"free execution slot reserved, unrelated flags preserved");
    if (saved) {
        context.Dr1=0x3333; context.Dr7|=4;
        check(al::phases::release_debug_slot(context,*saved) && context.Dr0==0x1111 && context.Dr1==0x3333 && context.Dr7==0x404,"only owned debug state restored");
        context.Dr0=0x4444;
        check(!al::phases::release_debug_slot(context,*saved) && context.Dr0==0x4444,"foreign debug update left intact");
    }
    context={}; context.Dr7=0xff;
    check(!al::phases::reserve_debug_slot(context,0x1234) && context.Dr7==0xff,"fully occupied breakpoint registers refused");
    context={}; context.Dr7=0xf0000;
    saved=al::phases::reserve_debug_slot(context,0x1234);
    check(saved && saved->index==1 && (context.Dr7&0xf0000)==0xf0000,"inactive foreign control settings preserved");
    context={}; context.Dr7=2;
    check(!al::phases::reserve_debug_slot(context,0x1234) && context.Dr7==2,"foreign global breakpoint state refused unchanged");
    context.Dr7=0x2000;
    check(!al::phases::reserve_debug_slot(context,0x1234) && context.Dr7==0x2000,"general-detect state refused unchanged");
    context={}; context.EFlags=0x100;
    check(!al::phases::reserve_debug_slot(context,0x1234) && context.EFlags==0x100,"existing single-step state refused unchanged");
    return failures?1:0;
}
