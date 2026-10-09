#include "core/plugins/guard.hpp"

namespace cmfm::plugins {
namespace {
constexpr unsigned long kCppException = 0xe06d7363;

bool manifest_cpp(cmfm_plugin_info_fn info, std::wstring_view name, std::wstring_view hash,
                  Manifest& manifest, std::string& reason, unsigned long& fault) noexcept {
    try { return read_manifest(info(), name, hash, manifest, reason); }
    catch (...) { fault = kCppException; return false; }
}
bool init_cpp(cmfm_plugin_init_fn init, const cmfm_api_t* api, const cmfm_init_t* block,
              cmfm_result_t& result, unsigned long& fault) noexcept {
    try { result = init(api, block); return true; }
    catch (...) { fault = kCppException; return false; }
}
bool phase_cpp(cmfm_phase_fn call, cmfm_plugin_t* plugin, cmfm_phase_t phase, const void* data,
               unsigned long& fault) noexcept {
    try { call(plugin, phase, data); return true; }
    catch (...) { fault = kCppException; return false; }
}
bool d3d12_event_cpp(cmfm_d3d12_event_fn call, cmfm_plugin_t* plugin, cmfm_d3d12_event_t event,
                     const void* data, void* user, unsigned long& fault) noexcept {
    try { call(plugin, event, data, user); return true; }
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

bool guarded_manifest(cmfm_plugin_info_fn info, std::wstring_view name, std::wstring_view hash,
                      Manifest& manifest, std::string& reason, unsigned long& fault) noexcept {
    __try { return manifest_cpp(info, name, hash, manifest, reason, fault); }
    __except (recoverable_fault(GetExceptionCode())) { fault = GetExceptionCode(); return false; }
}

bool guarded_init(cmfm_plugin_init_fn init, const cmfm_api_t* api, const cmfm_init_t* block,
                  cmfm_result_t& result, unsigned long& fault) noexcept {
    __try { return init_cpp(init, api, block, result, fault); }
    __except (recoverable_fault(GetExceptionCode())) { fault = GetExceptionCode(); return false; }
}

bool guarded_phase(cmfm_phase_fn call, cmfm_plugin_t* plugin, cmfm_phase_t phase, const void* data,
                   unsigned long& fault) noexcept {
    __try { return phase_cpp(call, plugin, phase, data, fault); }
    __except (recoverable_fault(GetExceptionCode())) { fault = GetExceptionCode(); return false; }
}

bool guarded_d3d12_event(cmfm_d3d12_event_fn call, cmfm_plugin_t* plugin, cmfm_d3d12_event_t event,
                         const void* data, void* user, unsigned long& fault) noexcept {
    __try { return d3d12_event_cpp(call, plugin, event, data, user, fault); }
    __except (recoverable_fault(GetExceptionCode())) { fault = GetExceptionCode(); return false; }
}
}  // namespace cmfm::plugins
