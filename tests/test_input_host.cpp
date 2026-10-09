#include "core/plugins/host.hpp"
#include "core/input/service.hpp"
#include "work_dir.hpp"
#include <windows.h>
#include <bit>
#include <filesystem>
#include <iostream>
#include <stdexcept>
namespace {void check(bool v,const char* why){if(!v) throw std::runtime_error(why);}}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc!=3) return 2;
        const auto work=al_test::fresh_work_dir(argv[2]);const auto folder=work/L"plugins"/L"input_fixture";std::filesystem::create_directories(folder);
        std::filesystem::copy_file(argv[1],folder/L"input_fixture.dll");
        al::plugins::Host host;host.load_all({(work/L"plugins").wstring(),(work/L"logs").wstring(),L""});
        const auto reports=host.report();check(reports.size()==1 && reports[0].status!=AL_STATUS_DISABLED,"C API plugin initialised");
        auto& input=host.input_service();check(input.wanted() && host.frame_requested(),"input requests the frame layer");
        bool attached=false;input.set_native([&](void* window){attached=true;input.window(window);},{});
        host.dispatch(AL_PHASE_PRE_MAIN,nullptr);
        check(!host.graphics().chains().empty(),"input alone requests swap-chain observation");
        al_d3d12_swapchain_t chain{};chain.size=sizeof(chain);chain.window=reinterpret_cast<void*>(1);host.dispatch(AL_PHASE_SWAPCHAIN_CREATED,&chain);
        check(attached && input.window()==chain.window,"window reaches input before plugin phase");
        input.ready(AL_INPUT_ALL);input.focus(true);input.end_frame();
        std::array<std::uint8_t,256> map{},raw{};for(unsigned i=0;i<256;++i) map[i]=static_cast<std::uint8_t>(i);
        raw[VK_F7]=raw[VK_LCONTROL]=0x80;input.keyboard(raw.data(),map);
        al_pad_state_t pad{};pad.size=sizeof(pad);pad.connected=1;pad.buttons=AL_PAD_A;input.pad(0,pad);input.end_frame();
        const auto dll=GetModuleHandleW(L"input_fixture.dll");
        const auto validate=std::bit_cast<BOOL(*)(unsigned,unsigned,unsigned)>(GetProcAddress(dll,"al_test_input_validate"));
        const auto capture=std::bit_cast<BOOL(*)()>(GetProcAddress(dll,"al_test_input_capture"));
        const auto release=std::bit_cast<BOOL(*)()>(GetProcAddress(dll,"al_test_input_release"));
        check(validate && capture && release && validate(1,1,0),"ABI hotkey and pad dispatch");
        check(input.message(chain.window,WM_CHAR,'a',0) && validate(1,1,1),"ABI window message");
        check(capture() && input.capture_mask()==AL_INPUT_ALL && release() && !input.capture_mask(),"ABI capture and release");
        check(capture() && host.raise_exiting() && !input.capture_mask() && !capture(),"EXITING retires input");
        host.dispatch(AL_PHASE_SHUTDOWN,nullptr);
        std::cout<<"Input C ABI plugin, version checks, graphics request, window dispatch and EXITING passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
