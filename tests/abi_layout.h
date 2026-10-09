/* The plugin ABI layout, pinned. Compiled as C (test_abi.c) and as C++
 * (test_abi.cpp), so both languages must agree on these exact numbers.
 * Changing one is an ABI break: it needs a new major API version. */
#ifndef AL_TESTS_ABI_LAYOUT_H
#define AL_TESTS_ABI_LAYOUT_H

#include "al/al_plugin.h"

#include <stddef.h>

#ifdef __cplusplus
#define AL_ABI_CHECK(expression) static_assert(expression, #expression)
#else
#define AL_ABI_CHECK(expression) _Static_assert(expression, #expression)
#endif

AL_ABI_CHECK(AL_API_VERSION == 0x00010007u);

/* 1.0 fields: unchanged since 1.0, so 1.0 plugins keep working. */
AL_ABI_CHECK(sizeof(al_plugin_info_t) == 80);
AL_ABI_CHECK(offsetof(al_plugin_info_t, size) == 0);
AL_ABI_CHECK(offsetof(al_plugin_info_t, api_version) == 4);
AL_ABI_CHECK(offsetof(al_plugin_info_t, name) == 8);
AL_ABI_CHECK(offsetof(al_plugin_info_t, display_name) == 16);
AL_ABI_CHECK(offsetof(al_plugin_info_t, version) == 24);
AL_ABI_CHECK(offsetof(al_plugin_info_t, phases) == 32);
AL_ABI_CHECK(offsetof(al_plugin_info_t, order) == 36);
AL_ABI_CHECK(offsetof(al_plugin_info_t, load_after) == 40);
AL_ABI_CHECK(offsetof(al_plugin_info_t, load_before) == 48);
AL_ABI_CHECK(offsetof(al_plugin_info_t, exe_sha256) == 56);
AL_ABI_CHECK(offsetof(al_plugin_info_t, on_phase) == 64);
AL_ABI_CHECK(offsetof(al_plugin_info_t, dependencies) == 72); /* 1.1 */
AL_ABI_CHECK(sizeof(al_dependency_t) == 16);
AL_ABI_CHECK(offsetof(al_dependency_t, min_version) == 8);

AL_ABI_CHECK(sizeof(al_init_t) == 40);
AL_ABI_CHECK(offsetof(al_init_t, size) == 0);
AL_ABI_CHECK(offsetof(al_init_t, reserved) == 4);
AL_ABI_CHECK(offsetof(al_init_t, self) == 8);
AL_ABI_CHECK(offsetof(al_init_t, folder) == 16);
AL_ABI_CHECK(offsetof(al_init_t, folder_real) == 24);
AL_ABI_CHECK(offsetof(al_init_t, data_folder) == 32); /* 1.1 */

AL_ABI_CHECK(sizeof(al_api_t) == 40);
AL_ABI_CHECK(offsetof(al_api_t, size) == 0);
AL_ABI_CHECK(offsetof(al_api_t, version) == 4);
AL_ABI_CHECK(offsetof(al_api_t, log) == 8);
AL_ABI_CHECK(offsetof(al_api_t, set_status) == 16);
AL_ABI_CHECK(offsetof(al_api_t, get_interface) == 24); /* 1.1 */
AL_ABI_CHECK(offsetof(al_api_t, publish_interface) == 32);

AL_ABI_CHECK(AL_GAME_VERSION == 0x00010000u && AL_SCANNER_VERSION == 0x00010000u);
AL_ABI_CHECK(sizeof(al_game_t) == 48);
AL_ABI_CHECK(offsetof(al_game_t, exe_sha256) == 8);
AL_ABI_CHECK(offsetof(al_game_t, game_version) == 16);
AL_ABI_CHECK(offsetof(al_game_t, exe_path) == 24);
AL_ABI_CHECK(offsetof(al_game_t, game_folder) == 32);
AL_ABI_CHECK(offsetof(al_game_t, pe_timestamp) == 40);
AL_ABI_CHECK(offsetof(al_game_t, graphics) == 44);
AL_ABI_CHECK(sizeof(al_scanner_t) == 24);
AL_ABI_CHECK(offsetof(al_scanner_t, find) == 8);
AL_ABI_CHECK(offsetof(al_scanner_t, rip_target) == 16);
AL_ABI_CHECK(AL_SCAN_CODE == 0 && AL_SCAN_IMAGE == 1 && AL_SCAN_INVALID == 0xFFFFFFFFu);
AL_ABI_CHECK(AL_GRAPHICS_UNKNOWN == 0 && AL_GRAPHICS_DX11 == 1 && AL_GRAPHICS_DX12 == 2);

AL_ABI_CHECK(AL_PHASE_PRE_MAIN == 0 && AL_PHASE_WIN_MAIN == 1 && AL_PHASE_DEVICE_CREATED == 2 &&
               AL_PHASE_SWAPCHAIN_CREATED == 3 && AL_PHASE_FIRST_PRESENT == 4 &&
               AL_PHASE_GAME_READY == 5 && AL_PHASE_SHUTDOWN == 6 && AL_PHASE_EXITING == 7 &&
               AL_PHASE_COUNT == 8);

/* 1.2 phase payloads. */
AL_ABI_CHECK(sizeof(al_phase_win_main_t) == 32);
AL_ABI_CHECK(offsetof(al_phase_win_main_t, instance) == 8);
AL_ABI_CHECK(offsetof(al_phase_win_main_t, command_line) == 16);
AL_ABI_CHECK(offsetof(al_phase_win_main_t, show_command) == 24);
AL_ABI_CHECK(sizeof(al_phase_device_t) == 40);
AL_ABI_CHECK(offsetof(al_phase_device_t, device) == 8);
AL_ABI_CHECK(offsetof(al_phase_device_t, riid) == 16);
AL_ABI_CHECK(offsetof(al_phase_device_t, adapter) == 24);
AL_ABI_CHECK(offsetof(al_phase_device_t, minimum_feature_level) == 32);
AL_ABI_CHECK(sizeof(al_phase_shutdown_t) == 8);
AL_ABI_CHECK(offsetof(al_phase_shutdown_t, exit_code) == 4);
AL_ABI_CHECK(AL_STATUS_ACTIVE == 0 && AL_STATUS_DEGRADED == 1 && AL_STATUS_DISABLED == 2);

/* 1.3: "al.d3d12". */
AL_ABI_CHECK(AL_D3D12_VERSION == 0x00010000u);
AL_ABI_CHECK(AL_D3D12_SWAPCHAIN_CREATED == 0 && AL_D3D12_PRE_PRESENT == 1 && AL_D3D12_POST_PRESENT == 2 &&
               AL_D3D12_PRE_RESIZE == 3 && AL_D3D12_POST_RESIZE == 4 && AL_D3D12_EVENT_COUNT == 5);
AL_ABI_CHECK(AL_D3D12_FACTORY == 1 && AL_D3D12_SWAPCHAIN == 2 && AL_D3D12_DEVICE == 3 &&
               AL_D3D12_QUEUE == 4 && AL_D3D12_COMMAND_LIST == 5 && AL_D3D12_SLOT_LIMIT == 256);
AL_ABI_CHECK(AL_D3D12_SWAPCHAIN_PRESENT == 0x00020008u && AL_D3D12_SWAPCHAIN_PRESENT1 == 0x00020016u &&
               AL_D3D12_SWAPCHAIN_RESIZE_BUFFERS == 0x0002000Du && AL_D3D12_SWAPCHAIN_RESIZE_BUFFERS1 == 0x00020027u &&
               AL_D3D12_FACTORY_CREATE_SWAPCHAIN == 0x0001000Au &&
               AL_D3D12_FACTORY_CREATE_SWAPCHAIN_FOR_HWND == 0x0001000Fu &&
               AL_D3D12_SWAPCHAIN_GET_CURRENT_BACK_BUFFER_INDEX == 0x00020024u &&
               AL_D3D12_DEVICE_CREATE_COMMAND_QUEUE == 0x00030008u &&
               AL_D3D12_QUEUE_EXECUTE_COMMAND_LISTS == 0x0004000Au && AL_D3D12_QUEUE_SIGNAL == 0x0004000Eu);
AL_ABI_CHECK(AL_D3D12_WAITING == 0 && AL_D3D12_RUNNING == 1 && AL_D3D12_UNAVAILABLE == 2);
AL_ABI_CHECK(sizeof(al_d3d12_swapchain_t) == 64);
AL_ABI_CHECK(offsetof(al_d3d12_swapchain_t, swapchain) == 8);
AL_ABI_CHECK(offsetof(al_d3d12_swapchain_t, queue) == 16);
AL_ABI_CHECK(offsetof(al_d3d12_swapchain_t, device) == 24);
AL_ABI_CHECK(offsetof(al_d3d12_swapchain_t, window) == 32);
AL_ABI_CHECK(offsetof(al_d3d12_swapchain_t, width) == 40);
AL_ABI_CHECK(offsetof(al_d3d12_swapchain_t, flags) == 56);
AL_ABI_CHECK(sizeof(al_d3d12_present_t) == 56);
AL_ABI_CHECK(offsetof(al_d3d12_present_t, swapchain) == 8);
AL_ABI_CHECK(offsetof(al_d3d12_present_t, frame) == 16);
AL_ABI_CHECK(offsetof(al_d3d12_present_t, back_buffer) == 24);
AL_ABI_CHECK(offsetof(al_d3d12_present_t, result) == 36);
AL_ABI_CHECK(offsetof(al_d3d12_present_t, present_qpc) == 40);
AL_ABI_CHECK(offsetof(al_d3d12_present_t, previous_qpc) == 48);
AL_ABI_CHECK(sizeof(al_d3d12_resize_t) == 40);
AL_ABI_CHECK(offsetof(al_d3d12_resize_t, swapchain) == 8);
AL_ABI_CHECK(offsetof(al_d3d12_resize_t, buffer_count) == 16);
AL_ABI_CHECK(offsetof(al_d3d12_resize_t, result) == 36);
AL_ABI_CHECK(sizeof(al_d3d12_hook_t) == 24);
AL_ABI_CHECK(offsetof(al_d3d12_hook_t, order) == 4);
AL_ABI_CHECK(offsetof(al_d3d12_hook_t, detour) == 8);
AL_ABI_CHECK(offsetof(al_d3d12_hook_t, next) == 16);
AL_ABI_CHECK(sizeof(al_d3d12_status_t) == 40);
AL_ABI_CHECK(offsetof(al_d3d12_status_t, reason) == 8);
AL_ABI_CHECK(offsetof(al_d3d12_status_t, owner) == 16);
AL_ABI_CHECK(offsetof(al_d3d12_status_t, frames) == 24);
AL_ABI_CHECK(offsetof(al_d3d12_status_t, last_present_qpc) == 32);
AL_ABI_CHECK(sizeof(al_d3d12_t) == 56);
AL_ABI_CHECK(offsetof(al_d3d12_t, subscribe) == 8);
AL_ABI_CHECK(offsetof(al_d3d12_t, hook) == 16);
AL_ABI_CHECK(offsetof(al_d3d12_t, claim_presentation) == 24);
AL_ABI_CHECK(offsetof(al_d3d12_t, begin_internal) == 32);
AL_ABI_CHECK(offsetof(al_d3d12_t, end_internal) == 40);
AL_ABI_CHECK(offsetof(al_d3d12_t, status) == 48);

/* 1.4: "al.hooks". */
AL_ABI_CHECK(AL_HOOKS_VERSION == 0x00010000u);
AL_ABI_CHECK(AL_HOOK_PENDING == 0 && AL_HOOK_ACTIVE == 1 && AL_HOOK_DISABLED == 2 && AL_HOOK_FAILED == 3);
AL_ABI_CHECK(sizeof(al_hook_t) == 56);
AL_ABI_CHECK(offsetof(al_hook_t, order) == 4);
AL_ABI_CHECK(offsetof(al_hook_t, name) == 8);
AL_ABI_CHECK(offsetof(al_hook_t, pattern) == 16);
AL_ABI_CHECK(offsetof(al_hook_t, offset) == 24);
AL_ABI_CHECK(offsetof(al_hook_t, address) == 32);
AL_ABI_CHECK(offsetof(al_hook_t, detour) == 40);
AL_ABI_CHECK(offsetof(al_hook_t, next) == 48);
AL_ABI_CHECK(sizeof(al_hooks_t) == 32);
AL_ABI_CHECK(offsetof(al_hooks_t, hook) == 8);
AL_ABI_CHECK(offsetof(al_hooks_t, set_enabled) == 16);
AL_ABI_CHECK(offsetof(al_hooks_t, state) == 24);

/* 1.5: "al.frame". */
AL_ABI_CHECK(AL_FRAME_VERSION == 0x00010000u);
AL_ABI_CHECK(AL_FRAME_BEGIN == 0 && AL_FRAME_END == 1);
AL_ABI_CHECK(AL_FRAME_WAITING == 0 && AL_FRAME_RUNNING == 1 && AL_FRAME_STOPPED == 2 && AL_FRAME_UNAVAILABLE == 3);
AL_ABI_CHECK(sizeof(al_frame_info_t) == 24);
AL_ABI_CHECK(offsetof(al_frame_info_t, index) == 8);
AL_ABI_CHECK(offsetof(al_frame_info_t, qpc) == 16);
AL_ABI_CHECK(sizeof(al_frame_status_t) == 32);
AL_ABI_CHECK(offsetof(al_frame_status_t, state) == 4);
AL_ABI_CHECK(offsetof(al_frame_status_t, frames) == 8);
AL_ABI_CHECK(offsetof(al_frame_status_t, game_thread) == 16);
AL_ABI_CHECK(offsetof(al_frame_status_t, reason) == 24);
AL_ABI_CHECK(sizeof(al_frame_t) == 40);
AL_ABI_CHECK(offsetof(al_frame_t, subscribe) == 8);
AL_ABI_CHECK(offsetof(al_frame_t, unsubscribe) == 16);
AL_ABI_CHECK(offsetof(al_frame_t, post) == 24);
AL_ABI_CHECK(offsetof(al_frame_t, status) == 32);

AL_ABI_CHECK(AL_LOG_DEBUG == 0 && AL_LOG_INFO == 1 && AL_LOG_WARNING == 2 && AL_LOG_ERROR == 3);
/* 1.7: GAME_READY's payload. */
AL_ABI_CHECK(sizeof(al_phase_game_ready_t) == 16);
AL_ABI_CHECK(offsetof(al_phase_game_ready_t, source) == 4);
AL_ABI_CHECK(offsetof(al_phase_game_ready_t, frame) == 8);
AL_ABI_CHECK(AL_GAME_READY_TITLE == 1);

/* 1.6: "al.input" (additive; all earlier tables stay unchanged). */
AL_ABI_CHECK(AL_INPUT_VERSION == 0x00010000u);
AL_ABI_CHECK(sizeof(al_hotkey_t) == 24 && offsetof(al_hotkey_t, name) == 16);
AL_ABI_CHECK(sizeof(al_pad_hotkey_t) == 24 && offsetof(al_pad_hotkey_t, name) == 16);
AL_ABI_CHECK(sizeof(al_pad_state_t) == 24 && offsetof(al_pad_state_t, buttons) == 12);
AL_ABI_CHECK(offsetof(al_pad_state_t, left_x) == 16 && offsetof(al_pad_state_t, right_x) == 20);
AL_ABI_CHECK(sizeof(al_input_status_t) == 32 && offsetof(al_input_status_t, window) == 16);
AL_ABI_CHECK(sizeof(al_input_t) == 96 && offsetof(al_input_t, status) == 88);

#endif
