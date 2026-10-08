/* The plugin ABI layout, pinned. Compiled as C (test_abi.c) and as C++
 * (test_abi.cpp), so both languages must agree on these exact numbers.
 * Changing one is an ABI break: it needs a new major API version. */
#ifndef CMFM_TESTS_ABI_LAYOUT_H
#define CMFM_TESTS_ABI_LAYOUT_H

#include "cmfm/cmfm_plugin.h"

#include <stddef.h>

#ifdef __cplusplus
#define CMFM_ABI_CHECK(expression) static_assert(expression, #expression)
#else
#define CMFM_ABI_CHECK(expression) _Static_assert(expression, #expression)
#endif

CMFM_ABI_CHECK(CMFM_API_VERSION == 0x00010000u);

CMFM_ABI_CHECK(sizeof(cmfm_plugin_info_t) == 72);
CMFM_ABI_CHECK(offsetof(cmfm_plugin_info_t, size) == 0);
CMFM_ABI_CHECK(offsetof(cmfm_plugin_info_t, api_version) == 4);
CMFM_ABI_CHECK(offsetof(cmfm_plugin_info_t, name) == 8);
CMFM_ABI_CHECK(offsetof(cmfm_plugin_info_t, display_name) == 16);
CMFM_ABI_CHECK(offsetof(cmfm_plugin_info_t, version) == 24);
CMFM_ABI_CHECK(offsetof(cmfm_plugin_info_t, phases) == 32);
CMFM_ABI_CHECK(offsetof(cmfm_plugin_info_t, order) == 36);
CMFM_ABI_CHECK(offsetof(cmfm_plugin_info_t, load_after) == 40);
CMFM_ABI_CHECK(offsetof(cmfm_plugin_info_t, load_before) == 48);
CMFM_ABI_CHECK(offsetof(cmfm_plugin_info_t, exe_sha256) == 56);
CMFM_ABI_CHECK(offsetof(cmfm_plugin_info_t, on_phase) == 64);

CMFM_ABI_CHECK(sizeof(cmfm_init_t) == 32);
CMFM_ABI_CHECK(offsetof(cmfm_init_t, size) == 0);
CMFM_ABI_CHECK(offsetof(cmfm_init_t, reserved) == 4);
CMFM_ABI_CHECK(offsetof(cmfm_init_t, self) == 8);
CMFM_ABI_CHECK(offsetof(cmfm_init_t, folder) == 16);
CMFM_ABI_CHECK(offsetof(cmfm_init_t, folder_real) == 24);

CMFM_ABI_CHECK(sizeof(cmfm_api_t) == 24);
CMFM_ABI_CHECK(offsetof(cmfm_api_t, size) == 0);
CMFM_ABI_CHECK(offsetof(cmfm_api_t, version) == 4);
CMFM_ABI_CHECK(offsetof(cmfm_api_t, log) == 8);
CMFM_ABI_CHECK(offsetof(cmfm_api_t, set_status) == 16);

CMFM_ABI_CHECK(CMFM_PHASE_PRE_MAIN == 0 && CMFM_PHASE_WIN_MAIN == 1 && CMFM_PHASE_DEVICE_CREATED == 2 &&
               CMFM_PHASE_SWAPCHAIN_CREATED == 3 && CMFM_PHASE_FIRST_PRESENT == 4 &&
               CMFM_PHASE_GAME_READY == 5 && CMFM_PHASE_COUNT == 6);
CMFM_ABI_CHECK(CMFM_STATUS_ACTIVE == 0 && CMFM_STATUS_DEGRADED == 1 && CMFM_STATUS_DISABLED == 2);
CMFM_ABI_CHECK(CMFM_LOG_DEBUG == 0 && CMFM_LOG_INFO == 1 && CMFM_LOG_WARNING == 2 && CMFM_LOG_ERROR == 3);

#endif
