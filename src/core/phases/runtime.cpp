#include "core/phases/runtime.hpp"
#include "core/phases/imports.hpp"
#include "core/phases/sites.hpp"
#include "core/phases/debug_slot.hpp"
#include "core/graphics/installer.hpp"
#include "core/game_hooks/installer.hpp"
#include "core/patch/writer.hpp"
#include "core/frame/layer.hpp"
#include "core/frame/clock.hpp"
#include "core/input/native.hpp"
#include "common/memory.hpp"
#include "common/windows.hpp"
#include <windows.h>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace al::phases {
namespace {
using WinMain = int (WINAPI*)(HINSTANCE, HINSTANCE, char*, int);
struct Image {
    const std::uint8_t* base{};
    std::uint32_t size{};
    std::vector<startup::CodeRange> code;
    startup::CodeRange common{};
};
struct DeviceFrame {
    std::uint64_t r11, r10, out, iid, level, adapter, rax, flags;
};
// Written by the worker before signalling ready; read by its requesting
// thread after the event wait. No formatting/allocation while it is suspended.
struct ObserverTiming {
    std::uint64_t begin{}, signal{}, awake{}, worker_ready{}, end{};
    std::uint64_t reset{}, notify{}, open{}, suspend{}, get{}, reserve{}, set{}, resume{};
};
struct Pending {
    bool win_main{};
    DWORD thread{};
    void* caller{};
    void* resume{};
    void** out{};
    GUID iid{};
    void* adapter{};
    std::uint32_t level{};
    DebugSlot debug{};
    bool armed{};
    ObserverTiming timing;
    unsigned call{};
    std::uint64_t forward_ready{}, forward_log{};
};
struct State {
    plugins::Host* host{};
    Log log{};
    Warning warning{};
    graphics::NativeInstaller* graphics{};
    game_hooks::NativeInstaller* game_hooks{};
    patch::NativeWriter* patches{};
    frame::Layer* frame{};
    input::Native* input{};
    Image image;
    WinMain win_original{};
    std::atomic<bool> win_entered{false}, stopped{false}, device_done{false};
    bool win_armed=false;
    std::atomic<bool> observer_active{false};
    std::atomic<DWORD> observer_thread{0};
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    ImportSlot device_import;
    void* device_stub{};
    std::atomic<void*> forward{};
    std::atomic<void*> handler{nullptr};
    std::recursive_mutex dispatch_mutex;
    Handle request, ready, worker;
    Pending pending;
    unsigned device_attempts=0;
    std::atomic<unsigned> device_calls{0};
    std::uint64_t qpc_frequency{}, worker_create_begin{}, worker_create{};
    std::atomic<std::uint64_t> worker_started{0};
};
State* g{}; // Pinned core / host and callback addresses live until exit.

void log(std::wstring_view text) noexcept { try { if (g->log) g->log(text); } catch (...) {} }
std::wstring widen(std::string_view text) {
    const auto count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}
void report_game_hooks() {
    for (const auto& line : g->host->game_hooks().summary()) log(widen(line));
    for (const auto& line : g->host->patches().summary()) log(widen(line));
    for (const auto& line : g->host->frame_clock().summary()) log(widen(line));
    if (g->frame) log(widen(g->frame->ready_summary()));
    for (const auto& line : g->host->input_service().summary()) log(widen(line));
}
void unavailable(std::wstring_view phase, std::wstring_view reason) noexcept {
    try { log(L"phase_" + std::wstring(phase) + L"_skipped: " + std::wstring(reason));
        if (g->warning) g->warning(L"Astera Loader phase " + std::wstring(phase) + L" is unavailable: " + std::wstring(reason));
    } catch (...) {}
}
std::uint64_t ticks() { LARGE_INTEGER n{}; QueryPerformanceCounter(&n); return n.QuadPart; }
std::uint64_t microseconds(std::uint64_t count) noexcept {
    return g->qpc_frequency ? count*1000000/g->qpc_frequency : 0;
}
std::uint64_t interval(std::uint64_t begin,std::uint64_t end) noexcept {
    return begin && end>=begin ? end-begin : 0;
}
void observer_timing(const Pending& request) noexcept {
    if (!request.win_main && request.call>3) return;
    try {
        const auto& t=request.timing;
        log(L"startup_hook_timing operation="+std::wstring(request.win_main?L"win_main_arm":L"device_return_arm")+
            L" elapsed_us="+std::to_wstring(microseconds(interval(t.begin,t.end)))+
            L" thread_id="+std::to_wstring(request.thread)+L" call="+std::to_wstring(request.call)+
            L" armed="+std::to_wstring(request.armed)+L" begin_qpc="+std::to_wstring(t.begin)+
            L" end_qpc="+std::to_wstring(t.end)+L" reset_us="+std::to_wstring(microseconds(t.reset))+
            L" signal_us="+std::to_wstring(microseconds(t.notify))+
            L" worker_wake_us="+std::to_wstring(microseconds(interval(t.signal,t.awake)))+
            L" thread_open_us="+std::to_wstring(microseconds(t.open))+
            L" suspend_us="+std::to_wstring(microseconds(t.suspend))+
            L" get_context_us="+std::to_wstring(microseconds(t.get))+
            L" reserve_us="+std::to_wstring(microseconds(t.reserve))+
            L" set_context_us="+std::to_wstring(microseconds(t.set))+
            L" resume_us="+std::to_wstring(microseconds(t.resume))+
            L" caller_wake_us="+std::to_wstring(microseconds(interval(t.worker_ready,t.end)))+
            L" worker_create_us="+std::to_wstring(microseconds(g->worker_create))+
            L" worker_start_from_create_us="+std::to_wstring(microseconds(interval(g->worker_create_begin,g->worker_started.load())))+
            L" worker_precreated=1");
    } catch (...) { /* Optional diagnostics never affect forwarding. */ }
}
void dispatch(al_phase_t phase, std::wstring_view name, const void* data) noexcept {
    try {
        std::lock_guard serial(g->dispatch_mutex);
        const auto begin = ticks();
        log(L"phase_" + std::wstring(name) + L"_enter thread_id=" + std::to_wstring(GetCurrentThreadId()));
        g->host->dispatch(phase, data);
        if (phase == AL_PHASE_SHUTDOWN && g->graphics) g->graphics->report(g->host->graphics());
        if (phase == AL_PHASE_SHUTDOWN) report_game_hooks();
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
        if (frequency.QuadPart) log(L"phase_" + std::wstring(name) + L"_complete elapsed_us=" +
            std::to_wstring((ticks()-begin)*1000000/frequency.QuadPart));
        for (const auto& plugin : g->host->report()) {
            log(L"plugin_phase name=" + plugin.name + L" phase=" + std::wstring(name) + L" elapsed_us=" +
                std::to_wstring(plugin.phase_us[phase]) + L" status=" + std::to_wstring(plugin.status));
            if (plugin.status != AL_STATUS_ACTIVE && g->warning) g->warning(L"Astera Loader reports a plugin phase failure; see the core log.");
        }
    } catch (...) { unavailable(name, L"plugin-host dispatch failed"); }
}

Image image_of(const void* wrapper) {
    Image image;
    image.base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!readable(image.base, sizeof(IMAGE_DOS_HEADER))) return {};
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 64 || dos->e_lfanew > 0x100000) return {};
    auto pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.base + dos->e_lfanew);
    if (!readable(pe, sizeof(*pe)) || pe->Signature != IMAGE_NT_SIGNATURE ||
        pe->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || pe->FileHeader.NumberOfSections > 96) return {};
    image.size = pe->OptionalHeader.SizeOfImage;
    const auto sections = IMAGE_FIRST_SECTION(pe);
    if (!readable(sections, pe->FileHeader.NumberOfSections*sizeof(*sections))) return {};
    for (unsigned i=0; i<pe->FileHeader.NumberOfSections; ++i) {
        const auto& s=sections[i];
        if (s.VirtualAddress >= image.size || s.Misc.VirtualSize > image.size-s.VirtualAddress) return {};
        if ((s.Characteristics & IMAGE_SCN_MEM_EXECUTE) && readable(image.base+s.VirtualAddress,s.Misc.VirtualSize))
            image.code.push_back({s.VirtualAddress,s.Misc.VirtualSize});
    }
    const auto start = reinterpret_cast<std::uintptr_t>(wrapper)-reinterpret_cast<std::uintptr_t>(image.base);
    if (start > UINT32_MAX) return {};
    std::string reason;
    auto crt = startup::verify_crt_entry({image.base,image.size},image.code,static_cast<std::uint32_t>(start),reason);
    if (!crt) return {};
    const auto directory = pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (directory.VirtualAddress >= image.size || directory.Size > image.size-directory.VirtualAddress ||
        directory.Size%sizeof(RUNTIME_FUNCTION) || !readable(image.base+directory.VirtualAddress,directory.Size)) return {};
    const auto functions = reinterpret_cast<const RUNTIME_FUNCTION*>(image.base+directory.VirtualAddress);
    for (std::size_t i=0; i<directory.Size/sizeof(*functions); ++i) {
        if (functions[i].BeginAddress == crt->common_main && functions[i].EndAddress > crt->common_main &&
            functions[i].EndAddress <= image.size) {
            image.common={crt->common_main,functions[i].EndAddress-crt->common_main}; break;
        }
    }
    return image;
}

// RW during construction, RX before publication. Bridges are retained for
// in-flight calls, never RWX pages or code in a game/mod export.
void* executable(const std::vector<std::uint8_t>& bytes, void* address=nullptr) {
    auto memory=VirtualAlloc(address,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (!memory) return nullptr;
    std::memcpy(memory,bytes.data(),bytes.size());
    DWORD old=0;
    if (!VirtualProtect(memory,4096,PAGE_EXECUTE_READ,&old)) { VirtualFree(memory,0,MEM_RELEASE); return nullptr; }
    FlushInstructionCache(GetCurrentProcess(),memory,bytes.size());
    return memory;
}
void append64(std::vector<std::uint8_t>& bytes, const void* pointer) {
    const auto value=reinterpret_cast<std::uintptr_t>(pointer);
    const auto data=reinterpret_cast<const std::uint8_t*>(&value); bytes.insert(bytes.end(),data,data+8);
}
std::vector<std::uint8_t> jump_to(const void* target) {
    std::vector<std::uint8_t> bytes{0xff,0x25,0,0,0,0}; append64(bytes,target); return bytes;
}
void stop_device() noexcept {
    g->device_done.store(true);
    if (g->device_import.slot && g->device_stub && readable(g->device_import.slot,sizeof(void*)))
        swap_slot(g->device_import.slot,g->device_stub,g->forward.load());
    if (!g->busy.test_and_set()) {
        if (!g->observer_active.load()) if (auto handler=g->handler.exchange(nullptr)) RemoveVectoredExceptionHandler(handler);
        g->busy.clear();
    }
}
int WINAPI win_main(HINSTANCE instance,HINSTANCE previous,char* command,int show) {
    if (g->win_entered.exchange(true)) return g->win_original(instance,previous,command,show);
    const auto error=GetLastError();
    if (g->frame) g->frame->win_main(GetCurrentThreadId());
    log(L"phase_win_main_observer_removed; CRT and WinMain code unchanged");
    const al_phase_win_main_t main_data{sizeof(main_data),0,instance,command,show,0};
    dispatch(AL_PHASE_WIN_MAIN,L"win_main",&main_data);
    SetLastError(error);
    const auto code=g->win_original(instance,previous,command,show);
    const auto returned_error=GetLastError();
    g->stopped.store(true);
    stop_device();
    if (g->request) SetEvent(g->request.get());
    const al_phase_shutdown_t data{sizeof(data),code};
    dispatch(AL_PHASE_SHUTDOWN,L"shutdown",&data);
    SetLastError(returned_error);
    return code;
}

DWORD WINAPI context_worker(void*) noexcept {
    g->worker_started.store(ticks());
    while (WaitForSingleObject(g->request.get(),INFINITE)==WAIT_OBJECT_0) {
        const auto awake=ticks();
        const bool stopping=g->stopped.load();
        if (stopping && !g->observer_active.load()) break;
        auto& pending=g->pending;
        auto& timing=pending.timing;
        timing.awake=awake;
        if (!stopping) pending.armed=false;
        auto begin=ticks();
        Handle thread(OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,pending.thread));
        timing.open=ticks()-begin;
        DWORD suspended=DWORD(-1);
        if (thread) { begin=ticks(); suspended=SuspendThread(thread.get()); timing.suspend=ticks()-begin; }
        if (thread && suspended!=DWORD(-1)) {
            CONTEXT context{}; context.ContextFlags=CONTEXT_DEBUG_REGISTERS|CONTEXT_CONTROL;
            begin=ticks(); const auto got=GetThreadContext(thread.get(),&context); timing.get=ticks()-begin;
            if (got) {
                context.ContextFlags=CONTEXT_DEBUG_REGISTERS; // Never write control/stack/flags.
                if (stopping) {
                    if (release_debug_slot(context,pending.debug)) {
                        begin=ticks(); const auto set=SetThreadContext(thread.get(),&context); timing.set=ticks()-begin;
                        if (set) g->observer_active.store(false);
                    }
                } else {
                    begin=ticks(); const auto saved=reserve_debug_slot(context,reinterpret_cast<DWORD64>(pending.caller)); timing.reserve=ticks()-begin;
                    if (saved) {
                        pending.debug=*saved;
                        begin=ticks(); pending.armed=SetThreadContext(thread.get(),&context)!=FALSE; timing.set=ticks()-begin;
                    }
                }
            }
            begin=ticks();
            ResumeThread(thread.get()); // Exactly balances our one suspension.
            timing.resume=ticks()-begin;
        }
        timing.worker_ready=ticks();
        SetEvent(g->ready.get());
        if (stopping) {
            if (!g->observer_active.load()) if (auto handler=g->handler.exchange(nullptr)) RemoveVectoredExceptionHandler(handler);
            break;
        }
    }
    return 0;
}
LONG CALLBACK device_exception(EXCEPTION_POINTERS* exception) noexcept {
    if (!g || !g->observer_active.load(std::memory_order_acquire) ||
        GetCurrentThreadId()!=g->observer_thread.load() || exception->ExceptionRecord->ExceptionCode!=EXCEPTION_SINGLE_STEP ||
        exception->ExceptionRecord->ExceptionAddress!=g->pending.caller)
        return EXCEPTION_CONTINUE_SEARCH;
    auto& c=*exception->ContextRecord;
    const auto slot=g->pending.debug.index;
    if (!(c.Dr6 & (DWORD64{1}<<slot)) || (c.Dr6 & ((0xf ^ (DWORD64{1}<<slot))|0xe000u)) ||
        !release_debug_slot(c,g->pending.debug)) return EXCEPTION_CONTINUE_SEARCH;
    c.Rip=reinterpret_cast<DWORD64>(g->pending.resume);
    g->pending.armed=false;
    g->observer_active.store(false,std::memory_order_release);
    return EXCEPTION_CONTINUE_EXECUTION;
}

bool observe(Pending pending) noexcept {
    pending.timing.begin=ticks();
    g->pending=pending;
    auto begin=ticks();
    ResetEvent(g->ready.get());
    g->pending.timing.reset=ticks()-begin;
    g->pending.timing.signal=ticks();
    const auto notified=SetEvent(g->request.get());
    // This field belongs to the caller; the worker never reads it.
    g->pending.timing.notify=ticks()-g->pending.timing.signal;
    const auto ready=notified && WaitForSingleObject(g->ready.get(),INFINITE)==WAIT_OBJECT_0;
    g->pending.timing.end=ticks();
    if (!ready || !g->pending.armed) return false;
    g->observer_thread.store(pending.thread);
    g->observer_active.store(true,std::memory_order_release);
    return true;
}

void device_return(std::uint64_t result) noexcept {
    const auto error=GetLastError();
    const auto returned=ticks();
    try {
        auto& p=g->pending;
        if (p.call && p.call<=3) log(L"phase_device_call_return thread_id="+std::to_wstring(GetCurrentThreadId())+
            L" call="+std::to_wstring(p.call)+L" return_qpc="+std::to_wstring(returned)+
            L" forward_ready_qpc="+std::to_wstring(p.forward_ready)+
            L" forward_to_return_bridge_us="+std::to_wstring(microseconds(interval(p.forward_ready,returned)))+
            L" forward_log_us="+std::to_wstring(microseconds(p.forward_log)));
        const bool success=static_cast<std::int32_t>(result)>=0 && p.out && readable(p.out,sizeof(void*)) && *p.out;
        auto forward=g->forward.load();
        if (g->device_import.delayed) {
            // Only accept the actual resolved export, never replace a foreign
            // slot update. Existing inline hooks remain at this same address.
            const auto module=GetModuleHandleW(L"d3d12.dll");
            auto resolved=module?std::bit_cast<void*>(GetProcAddress(module,"D3D12CreateDevice")):nullptr;
            if (resolved && *g->device_import.slot==resolved) { forward=resolved; g->forward.store(resolved); }
        }
        if (success && !g->stopped.load()) {
            g->device_done.store(true);
            const auto value=*g->device_import.slot;
            if ((value==g->device_stub && !swap_slot(g->device_import.slot,g->device_stub,forward)) ||
                (value!=g->device_stub && value!=forward))
                unavailable(L"device_created",L"import-slot restoration failed");
            else {
                log(L"phase_device_import_restored; game code unchanged");
                log(L"phase_device_payload device="+std::to_wstring(reinterpret_cast<std::uintptr_t>(*p.out))+
                    L" adapter="+std::to_wstring(reinterpret_cast<std::uintptr_t>(p.adapter))+
                    L" minimum_feature_level="+std::to_wstring(p.level));
                const al_phase_device_t data{sizeof(data),0,*p.out,&p.iid,p.adapter,p.level,0};
                dispatch(AL_PHASE_DEVICE_CREATED,L"device_created",&data);
            }
        } else if (!g->stopped.load() && *g->device_import.slot!=g->device_stub &&
                   !swap_slot(g->device_import.slot,forward,g->device_stub)) {
            g->device_done.store(true); unavailable(L"device_created",L"delay import changed by another owner");
        }
        if (g->device_done.load()) if (auto handler=g->handler.exchange(nullptr)) RemoveVectoredExceptionHandler(handler);
    } catch (...) { g->device_done.store(true); unavailable(L"device_created",L"device observation failed"); }
    g->busy.clear();
    SetLastError(error);
}

// Both bridges save the seven volatile GP registers, flags and XMM0..5.
// C helpers preserve nonvolatile registers. Entry is 8 mod 16; a return site
// is 0 mod 16. Shadow space is separate from saved registers/XMM state.
std::vector<std::uint8_t> save_frame(bool entry) {
    std::vector<std::uint8_t> b{0x9c,0x50,0x51,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,
        0x48,0x81,0xec,static_cast<std::uint8_t>(entry?0x88:0x80),0,0,0};
    for (unsigned xmm=0; xmm<6; ++xmm) b.insert(b.end(),{0xf3,0x0f,0x7f,static_cast<std::uint8_t>(0x44+xmm*8),0x24,static_cast<std::uint8_t>(0x20+xmm*16)});
    return b;
}
void restore_frame(std::vector<std::uint8_t>& b,bool entry) {
    for (unsigned xmm=0; xmm<6; ++xmm) b.insert(b.end(),{0xf3,0x0f,0x6f,static_cast<std::uint8_t>(0x44+xmm*8),0x24,static_cast<std::uint8_t>(0x20+xmm*16)});
    b.insert(b.end(),{0x48,0x81,0xc4,static_cast<std::uint8_t>(entry?0x88:0x80),0,0,0,0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5a,0x59,0x58,0x9d});
}
void call_helper(std::vector<std::uint8_t>& b,void* helper) { b.insert(b.end(),{0x48,0xb8}); append64(b,helper); b.insert(b.end(),{0xff,0xd0}); }
void* return_bridge(void* caller) {
    auto b=save_frame(false);
    b.insert(b.end(),{0x48,0x8b,0x8c,0x24,0xb0,0,0,0}); // saved RAX -> RCX
    call_helper(b,std::bit_cast<void*>(&device_return));
    restore_frame(b,false);
    const auto jump=jump_to(caller); b.insert(b.end(),jump.begin(),jump.end());
    return executable(b);
}
void* device_enter(void* caller, const DeviceFrame* frame) noexcept {
    const auto error=GetLastError();
    // First timestamp in the C helper, before logging, validation, allocation
    // or the worker handshake. The forwarding bridge only saved registers.
    const auto entered=ticks();
    const auto call=g->device_calls.fetch_add(1)+1;
    const bool measured=call<=3;
    std::uint64_t diagnostic_log=0, bridge_time=0, observer_time=0;
    bool observed=false;
    if (measured) {
        const auto begin=ticks();
        try { log(L"phase_device_call_enter thread_id="+std::to_wstring(GetCurrentThreadId())+
            L" call="+std::to_wstring(call)+L" entry_qpc="+std::to_wstring(entered)); } catch (...) {}
        diagnostic_log+=ticks()-begin;
    }
    auto forward=g->forward.load();
    bool claimed=false;
    try {
        const auto rva=reinterpret_cast<std::uintptr_t>(caller)-reinterpret_cast<std::uintptr_t>(g->image.base);
        if (!g->device_done.load() && !g->stopped.load() && g->win_entered.load() && rva < g->image.size &&
            calls_slot(caller,g->device_import.slot) && !(claimed=g->busy.test_and_set())) {
            // test_and_set returns the OLD value; this branch owns the request.
            claimed=true;
            if (++g->device_attempts>16) throw 0;
            auto& p=g->pending; p={}; p.thread=GetCurrentThreadId(); p.caller=caller; p.call=call;
            p.out=reinterpret_cast<void**>(frame->out); p.adapter=reinterpret_cast<void*>(frame->adapter);
            p.level=static_cast<std::uint32_t>(frame->level);
            const auto iid=reinterpret_cast<const GUID*>(frame->iid);
            if (!readable(iid,sizeof(GUID))) throw 0;
            p.iid=*iid;
            auto begin=ticks(); p.resume=return_bridge(caller); bridge_time=ticks()-begin;
            if (!p.resume) throw 0;
            const auto armed=observe(p);
            observer_time=interval(g->pending.timing.begin,g->pending.timing.end);
            begin=ticks(); observer_timing(g->pending); diagnostic_log+=ticks()-begin;
            if (!armed) throw 0;
            observed=true;
            begin=ticks();
            log(L"phase_device_return_observer_armed thread_id="+std::to_wstring(p.thread));
            diagnostic_log+=ticks()-begin;
            claimed=false; // Released by the return bridge, after the API.
        } else claimed=false;
    } catch (...) {
        g->device_done.store(true);
        if (claimed) { g->busy.clear(); claimed=false; }
        stop_device(); unavailable(L"device_created",L"could not reserve an execution breakpoint");
    }
    if (measured) {
        const auto before_log=ticks();
        try { log(L"phase_device_call_forward thread_id="+std::to_wstring(GetCurrentThreadId())+
            L" call="+std::to_wstring(call)+L" observed="+std::to_wstring(observed)+
            L" entry_qpc="+std::to_wstring(entered)+L" forward_log_begin_qpc="+std::to_wstring(before_log)+
            L" entry_to_forward_log_us="+std::to_wstring(microseconds(interval(entered,before_log)))+
            L" return_bridge_us="+std::to_wstring(microseconds(bridge_time))+
            L" observer_us="+std::to_wstring(microseconds(observer_time))+
            L" diagnostic_log_us="+std::to_wstring(microseconds(diagnostic_log))); } catch (...) {}
        if (observed) {
            g->pending.forward_ready=ticks();
            g->pending.forward_log=g->pending.forward_ready-before_log;
        }
    }
    SetLastError(error);
    return forward;
}
void* entry_bridge() {
    auto b=save_frame(true);
    b.insert(b.end(),{0x48,0x8b,0x8c,0x24,0xc8,0,0,0,0x48,0x8d,0x94,0x24,0x88,0,0,0});
    call_helper(b,std::bit_cast<void*>(&device_enter));
    // Restore R11 to the forwarding target. Its incoming value is volatile,
    // while all D3D12 arguments, the stack and original return address survive.
    b.insert(b.end(),{0x48,0x89,0x84,0x24,0x88,0,0,0});
    restore_frame(b,true); b.insert(b.end(),{0x41,0xff,0xe3});
    return executable(b);
}
}

void arm(plugins::Host& host,Log logger,Warning warning,const void* crt_wrapper) noexcept {
    if (g) return;
    try {
        g=new State; g->host=&host; g->log=logger; g->warning=warning;
        g->input=new input::Native(host.input_service(),[](std::string_view text){log(widen(text));},
            [](std::string_view text){if(g->warning) g->warning(L"Astera Loader input is unavailable: "+widen(text));});
        host.input_service().set_native([](void* window){g->input->attach_window(window);},[](std::uint32_t thread){g->input->begin_frame(thread);},
            []{g->input->end_frame();});
        g->frame = new frame::Layer(host, [](std::string_view text) { log(widen(text)); },
            [](std::string_view text) { if (g->warning) g->warning(L"Astera Loader frames are unavailable: " + widen(text)); });
        const auto identity = host.game_identity();
        g->frame->prepare(frame::game_sites(identity, GetModuleHandleW(nullptr)),
            host.frame_requested() || (identity && identity->game_version));
        g->input->prepare(host.game_hooks(), input::game_sites(identity, GetModuleHandleW(nullptr)));
        g->game_hooks = new game_hooks::NativeInstaller([](std::string_view text) { log(widen(text)); });
        host.game_hooks().set_installer(g->game_hooks);
        // al.patch (1.8): changes made so far are written now, before the
        // game's own code starts; later ones at once.
        g->patches = new patch::NativeWriter();
        host.patches().set_writer(g->patches);
        g->frame->installed();
        if (!g->frame->ready_problem().empty() && host.subscribed(AL_PHASE_GAME_READY) && g->warning)
            g->warning(L"Astera Loader cannot announce GAME_READY: " + widen(g->frame->ready_problem()));
        g->input->installed();
        report_game_hooks();
        if (!host.graphics().chains().empty()) {
            g->graphics = new graphics::NativeInstaller(
                [](std::string_view text) { log(widen(text)); },
                [](std::string_view text) { if (g->warning) g->warning(L"Astera Loader graphics are unavailable: " + widen(text)); });
            host.graphics().set_installer(g->graphics);
        }
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); g->qpc_frequency=frequency.QuadPart;
        g->image=image_of(crt_wrapper);
        std::string reason;
        const auto site=find_win_main({g->image.base,g->image.size},g->image.code,g->image.common,reason);
        if (!site) { unavailable(L"win_main",std::wstring(reason.begin(),reason.end())); unavailable(L"shutdown",L"WinMain unavailable"); return; }
        g->win_original=std::bit_cast<WinMain>(g->image.base+site->target);
        g->request.reset(CreateEventW(nullptr,FALSE,FALSE,nullptr)); g->ready.reset(CreateEventW(nullptr,TRUE,FALSE,nullptr));
        g->handler=AddVectoredExceptionHandler(1,device_exception);
        if (g->request && g->ready && g->handler) {
            g->worker_create_begin=ticks(); g->worker.reset(CreateThread(nullptr,0,context_worker,nullptr,0,nullptr));
            g->worker_create=ticks()-g->worker_create_begin;
        }
        Pending win{}; win.win_main=true; win.thread=GetCurrentThreadId();
        win.caller=std::bit_cast<void*>(g->win_original); win.resume=std::bit_cast<void*>(&win_main);
        const bool win_observed=g->worker && observe(win);
        if (g->worker) observer_timing(g->pending);
        if (!win_observed) {
            g->stopped.store(true); if (g->request) SetEvent(g->request.get()); stop_device();
            unavailable(L"win_main",L"could not reserve an execution breakpoint"); return;
        }
        g->win_armed=true;
        log(L"phase_win_main_armed call_rva="+std::to_wstring(site->call)+L" target_rva="+std::to_wstring(site->target));
        auto slot=find_import(g->image.base,"d3d12.dll","D3D12CreateDevice");
        if (!slot) slot=find_import(g->image.base,"d3d12.dll","",101);
        if (!slot) { log(L"phase_device_created_skipped: no D3D12 device import (DX11 or unsupported image)"); return; }
        g->device_import=*slot; g->forward.store(*slot->slot);
        g->device_stub=entry_bridge();
        if (!g->device_stub || !swap_slot(slot->slot,g->forward.load(),g->device_stub)) {
            // Keep WinMain's already armed observer/handler alive.
            g->device_done.store(true); unavailable(L"device_created",L"device import observer preparation failed"); return;
        }
        log(L"phase_device_created_armed; import only, return address and game code unchanged");
    } catch (...) { if (g) unavailable(g->win_armed?L"device_created":L"win_main",L"phase preparation failed"); }
}
}  // namespace al::phases
