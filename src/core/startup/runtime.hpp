#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace cmfm::startup {

using Log = void (*)(std::wstring_view);

// Process-lifetime startup state. All calls occur outside DllMain. The API and
// private launcher/core handshake are unchanged.
void initialise_hash_result(std::wstring_view logs_folder, Log log);
void publish_hash_result(std::wstring hash) noexcept;
void arm_pre_main(HMODULE self, const std::wstring& logs_folder, Log log) noexcept;

}  // namespace cmfm::startup
