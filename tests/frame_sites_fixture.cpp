// Test-only provider linked in place of sites.cpp. The production DLL never
// contains this resolver or accepts fake exports as game identity evidence.
#include "core/frame/sites.hpp"
#include <windows.h>
#include <bit>
#include <cstdio>

namespace al::frame {
Sites game_sites(const al_game_t*, const void* image) {
    Sites sites;
    const auto module = static_cast<HMODULE>(const_cast<void*>(image));
    sites.update.target = std::bit_cast<void*>(GetProcAddress(module, "al_test_frame"));
    sites.exiting.target = std::bit_cast<void*>(GetProcAddress(module, "al_test_exit_site"));
    sites.title.target = std::bit_cast<void*>(GetProcAddress(module, "al_test_title"));
    sites.main_object = reinterpret_cast<const void* const*>(GetProcAddress(module, "al_test_main_object"));
    if (!sites.update.target || !sites.exiting.target || !sites.title.target || !sites.main_object) { sites.problem = "missing synthetic frame exports"; return sites; }
    for (auto* site : {&sites.update, &sites.exiting, &sites.title}) {
        for (unsigned i = 0; i < 32; ++i) {
            char text[4]{}; std::snprintf(text, sizeof(text), "%02X", static_cast<const unsigned char*>(site->target)[i]);
            if (i) site->pattern += ' '; site->pattern += text;
        }
    }
    return sites;
}
}  // namespace al::frame
