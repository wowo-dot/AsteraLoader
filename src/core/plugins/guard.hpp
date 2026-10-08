#pragma once

#include "core/plugins/manifest.hpp"

#include <windows.h>

namespace cmfm::plugins {
// Native faults are isolated at the plugin call boundary. Fail-fast corruption
// and stack overflow cannot safely be recovered in the shared game process.
int recoverable_fault(unsigned long code) noexcept;
bool guarded_manifest(cmfm_plugin_info_fn info, std::wstring_view name, std::wstring_view hash,
                      Manifest& manifest, std::string& reason, unsigned long& fault) noexcept;
bool guarded_init(cmfm_plugin_init_fn init, const cmfm_api_t* api, const cmfm_init_t* block,
                  cmfm_result_t& result, unsigned long& fault) noexcept;
bool guarded_phase(cmfm_phase_fn call, cmfm_plugin_t* plugin, cmfm_phase_t phase, const void* data,
                   unsigned long& fault) noexcept;
}  // namespace cmfm::plugins
