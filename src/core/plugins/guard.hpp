#pragma once

#include "core/plugins/manifest.hpp"

#include <windows.h>

namespace al::plugins {
// Native faults are isolated at the plugin call boundary. Fail-fast corruption
// and stack overflow cannot safely be recovered in the shared game process.
int recoverable_fault(unsigned long code) noexcept;
bool guarded_manifest(al_plugin_info_fn info, std::wstring_view name, std::wstring_view hash,
                      Manifest& manifest, std::string& reason, unsigned long& fault) noexcept;
bool guarded_init(al_plugin_init_fn init, const al_api_t* api, const al_init_t* block,
                  al_result_t& result, unsigned long& fault) noexcept;
bool guarded_phase(al_phase_fn call, al_plugin_t* plugin, al_phase_t phase, const void* data,
                   unsigned long& fault) noexcept;
bool guarded_d3d12_event(al_d3d12_event_fn call, al_plugin_t* plugin, al_d3d12_event_t event,
                         const void* data, void* user, unsigned long& fault) noexcept;
}  // namespace al::plugins
