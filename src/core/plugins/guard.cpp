#include "core/plugins/guard.hpp"

namespace al::plugins {
namespace {
constexpr unsigned long kCppException = 0xe06d7363;

bool manifest_cpp(al_plugin_info_fn info, std::wstring_view name, std::wstring_view hash,
                  Manifest& manifest, std::string& reason, unsigned long& fault) noexcept {
    try { return read_manifest(info(), name, hash, manifest, reason); }
    catch (...) { fault = kCppException; return false; }
}
bool init_cpp(al_plugin_init_fn init, const al_api_t* api, const al_init_t* block,
              al_result_t& result, unsigned long& fault) noexcept {
    try { result = init(api, block); return true; }
    catch (...) { fault = kCppException; return false; }
}
bool phase_cpp(al_phase_fn call, al_plugin_t* plugin, al_phase_t phase, const void* data,
               unsigned long& fault) noexcept {
    try { call(plugin, phase, data); return true; }
    catch (...) { fault = kCppException; return false; }
}
bool d3d12_event_cpp(al_d3d12_event_fn call, al_plugin_t* plugin, al_d3d12_event_t event,
                     const void* data, void* user, unsigned long& fault) noexcept {
    try { call(plugin, event, data, user); return true; }
    catch (...) { fault = kCppException; return false; }
}
bool frame_event_cpp(al_frame_fn call, al_plugin_t* plugin, al_frame_event_t event, const al_frame_info_t* frame,
                     void* user, unsigned long& fault) noexcept {
    try { call(plugin, event, frame, user); return true; }
    catch (...) { fault = kCppException; return false; }
}
bool task_cpp(al_task_fn call, al_plugin_t* plugin, void* user, unsigned long& fault) noexcept {
    try { call(plugin, user); return true; }
    catch (...) { fault = kCppException; return false; }
}
}  // namespace

int recoverable_fault(unsigned long code) noexcept {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return EXCEPTION_EXECUTE_HANDLER;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

bool guarded_manifest(al_plugin_info_fn info, std::wstring_view name, std::wstring_view hash,
                      Manifest& manifest, std::string& reason, unsigned long& fault) noexcept {
    __try { return manifest_cpp(info, name, hash, manifest, reason, fault); }
    __except (recoverable_fault(GetExceptionCode())) { fault = GetExceptionCode(); return false; }
}

bool guarded_init(al_plugin_init_fn init, const al_api_t* api, const al_init_t* block,
                  al_result_t& result, unsigned long& fault) noexcept {
    __try { return init_cpp(init, api, block, result, fault); }
    __except (recoverable_fault(GetExceptionCode())) { fault = GetExceptionCode(); return false; }
}

bool guarded_phase(al_phase_fn call, al_plugin_t* plugin, al_phase_t phase, const void* data,
                   unsigned long& fault) noexcept {
    __try { return phase_cpp(call, plugin, phase, data, fault); }
    __except (recoverable_fault(GetExceptionCode())) { fault = GetExceptionCode(); return false; }
}

bool guarded_d3d12_event(al_d3d12_event_fn call, al_plugin_t* plugin, al_d3d12_event_t event,
                         const void* data, void* user, unsigned long& fault) noexcept {
    __try { return d3d12_event_cpp(call, plugin, event, data, user, fault); }
    __except (recoverable_fault(GetExceptionCode())) { fault = GetExceptionCode(); return false; }
}

bool guarded_frame_event(al_frame_fn call, al_plugin_t* plugin, al_frame_event_t event, const al_frame_info_t* frame,
                         void* user, unsigned long& fault) noexcept {
    __try { return frame_event_cpp(call, plugin, event, frame, user, fault); }
    __except (recoverable_fault(GetExceptionCode())) { fault = GetExceptionCode(); return false; }
}

bool guarded_task(al_task_fn call, al_plugin_t* plugin, void* user, unsigned long& fault) noexcept {
    __try { return task_cpp(call, plugin, user, fault); }
    __except (recoverable_fault(GetExceptionCode())) { fault = GetExceptionCode(); return false; }
}
}  // namespace al::plugins
