/* hello: the smallest CMFM plugin, in plain C.
 *
 * It logs the folder paths CMFM hands it, which game this is (the "cmfm.game"
 * service, API 1.1), each phase it sees with its payload (API 1.2 and 1.3),
 * and counts the game's presents through "cmfm.d3d12" (API 1.3). Install
 * it as <game>\cmfm\plugins\hello\hello.dll (under MO2: its own mod with that
 * layout). CMFM writes its log to cmfm\logs\plugins\hello.log.
 */
#include "cmfm/cmfm_plugin.h"

#include <windows.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

extern IMAGE_DOS_HEADER __ImageBase; /* this DLL */

static const cmfm_api_t* g_api;
static cmfm_plugin_t* g_self;
static const cmfm_d3d12_t* g_d3d12;
static uint64_t g_presents; /* PRE_PRESENT runs every frame: count, do not log */

static void say(cmfm_log_level_t level, const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    g_api->log(g_self, level, text);
}

static void say_path(const char* label, const wchar_t* path) {
    char utf8[1024];
    const int length = path ? WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), NULL, NULL) : 0;
    say(CMFM_LOG_INFO, "%s: %s", label, length > 0 ? utf8 : "(unavailable)");
}

static void say_module(const char* label, HMODULE module) {
    wchar_t path[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(module, path, sizeof(path) / sizeof(path[0]));
    say_path(label, length && length < sizeof(path) / sizeof(path[0]) ? path : NULL);
}

static double ms_since_process_start(void) {
    FILETIME created, exited, kernel, user, now;
    ULARGE_INTEGER start, current;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1.0;
    GetSystemTimePreciseAsFileTime(&now);
    start.LowPart = created.dwLowDateTime;
    start.HighPart = created.dwHighDateTime;
    current.LowPart = now.dwLowDateTime;
    current.HighPart = now.dwHighDateTime;
    return (double)(current.QuadPart - start.QuadPart) / 10000.0;
}

static void say_guid(const char* label, const GUID* guid) {
    if (!guid) {
        say(CMFM_LOG_INFO, "%s: (none)", label);
        return;
    }
    say(CMFM_LOG_INFO, "%s: {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", label, guid->Data1, guid->Data2,
        guid->Data3, guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3], guid->Data4[4], guid->Data4[5],
        guid->Data4[6], guid->Data4[7]);
}

static void CMFM_CALL on_phase(cmfm_plugin_t* self, cmfm_phase_t phase, const void* data) {
    (void)self;
    say(CMFM_LOG_INFO, "phase %u on thread %lu, %.1f ms after process start", phase, GetCurrentThreadId(),
        ms_since_process_start());
    /* Payloads are valid during this call only. Each starts with its size:
     * check it before reading a field a later API version added. */
    if (phase == CMFM_PHASE_PRE_MAIN) {
        say_module("game exe as the game sees it", NULL);
    } else if (phase == CMFM_PHASE_WIN_MAIN && data) {
        const cmfm_phase_win_main_t* win_main = (const cmfm_phase_win_main_t*)data;
        if (win_main->size < sizeof(*win_main)) return;
        say(CMFM_LOG_INFO, "WinMain: instance is the game exe: %s, command line \"%s\", show command %d",
            win_main->instance == (void*)GetModuleHandleW(NULL) ? "yes" : "no",
            win_main->command_line ? win_main->command_line : "(none)", win_main->show_command);
    } else if (phase == CMFM_PHASE_DEVICE_CREATED && data) {
        const cmfm_phase_device_t* device = (const cmfm_phase_device_t*)data;
        if (device->size < sizeof(*device)) return;
        say(CMFM_LOG_INFO, "device %s, adapter %s, minimum feature level 0x%x", device->device ? "present" : "missing",
            device->adapter ? "given" : "default", device->minimum_feature_level);
        say_guid("device interface the game asked for", (const GUID*)device->riid);
    } else if (phase == CMFM_PHASE_SWAPCHAIN_CREATED && data) {
        const cmfm_d3d12_swapchain_t* chain = (const cmfm_d3d12_swapchain_t*)data;
        if (chain->size < sizeof(*chain)) return;
        say(CMFM_LOG_INFO, "swap chain %ux%u, format %u, %u buffers, queue %s", chain->width, chain->height,
            chain->format, chain->buffer_count, chain->queue ? "given" : "missing");
    } else if (phase == CMFM_PHASE_FIRST_PRESENT && data) {
        const cmfm_d3d12_present_t* present = (const cmfm_d3d12_present_t*)data;
        if (present->size < sizeof(*present)) return;
        say(CMFM_LOG_INFO, "first present: frame %llu, back buffer %u, sync interval %u",
            (unsigned long long)present->frame, present->back_buffer, present->sync_interval);
    } else if (phase == CMFM_PHASE_SHUTDOWN && data) {
        const cmfm_phase_shutdown_t* shutdown = (const cmfm_phase_shutdown_t*)data;
        if (shutdown->size < sizeof(*shutdown)) return;
        say(CMFM_LOG_INFO, "WinMain returned %d", shutdown->exit_code);
        if (g_d3d12) {
            cmfm_d3d12_status_t status = {sizeof(cmfm_d3d12_status_t)};
            g_d3d12->status(g_self, &status);
            say(CMFM_LOG_INFO, "presents counted %llu; cmfm.d3d12 counted %llu, state %u%s%s",
                (unsigned long long)g_presents, (unsigned long long)status.frames, status.state,
                status.reason ? ": " : "", status.reason ? status.reason : "");
        }
    }
}

static void CMFM_CALL on_d3d12(cmfm_plugin_t* self, cmfm_d3d12_event_t event, const void* data, void* user) {
    (void)self;
    (void)user;
    if (event == CMFM_D3D12_PRE_PRESENT) {
        ++g_presents;
    } else if (event == CMFM_D3D12_POST_RESIZE) {
        const cmfm_d3d12_resize_t* resize = (const cmfm_d3d12_resize_t*)data;
        say(CMFM_LOG_INFO, "resized to %ux%u, result 0x%08x", resize->swapchain->width, resize->swapchain->height,
            (unsigned)resize->result);
    }
}

static const cmfm_plugin_info_t info = {
    sizeof(cmfm_plugin_info_t),
    CMFM_API_VERSION,
    "hello",      /* name: also the folder and DLL name */
    "CMFM Hello", /* display name */
    "0.4.0",      /* this plugin's version */
    CMFM_PHASE_BIT(CMFM_PHASE_PRE_MAIN) | CMFM_PHASE_BIT(CMFM_PHASE_WIN_MAIN) |
        CMFM_PHASE_BIT(CMFM_PHASE_DEVICE_CREATED) | CMFM_PHASE_BIT(CMFM_PHASE_SWAPCHAIN_CREATED) |
        CMFM_PHASE_BIT(CMFM_PHASE_FIRST_PRESENT) | CMFM_PHASE_BIT(CMFM_PHASE_SHUTDOWN),
    0,    /* order */
    NULL, /* load_after */
    NULL, /* load_before */
    NULL, /* exe_sha256: any game exe */
    on_phase,
};

CMFM_PLUGIN_EXPORT const cmfm_plugin_info_t* CMFM_CALL cmfm_plugin_info(void) {
    return &info;
}

CMFM_PLUGIN_EXPORT cmfm_result_t CMFM_CALL cmfm_plugin_init(const cmfm_api_t* api, const cmfm_init_t* init) {
    g_api = api;
    g_self = init->self;
    say(CMFM_LOG_INFO, "%s %s initialised on thread %lu, %.1f ms after process start (CMFM API %u.%u)", info.name,
        info.version, GetCurrentThreadId(), ms_since_process_start(), CMFM_VERSION_MAJOR(api->version),
        CMFM_VERSION_MINOR(api->version));
    say_path("folder as the game sees it", init->folder);
    say_path("folder resolved through a file handle", init->folder_real);
    say_module("this DLL", (HMODULE)&__ImageBase);

    /* API 1.1 additions; a CMFM older than this plugin's header would have
     * refused it, but checking the sizes is the habit to copy. */
    if (init->size > offsetof(cmfm_init_t, data_folder)) say_path("data folder", init->data_folder);
    if (api->size > offsetof(cmfm_api_t, get_interface)) {
        const cmfm_game_t* game = (const cmfm_game_t*)api->get_interface(init->self, CMFM_GAME_INTERFACE, CMFM_GAME_VERSION);
        if (game) {
            static const char* const graphics[] = {"unknown", "DX11", "DX12"};
            say(CMFM_LOG_INFO, "game version %s, exe SHA-256 %.12s..., graphics %s",
                game->game_version ? game->game_version : "unknown", game->exe_sha256 ? game->exe_sha256 : "unknown",
                game->graphics <= CMFM_GRAPHICS_DX12 ? graphics[game->graphics] : "unknown");
        }
        /* The code that loads MHW's version string, in Stracker's bit notation.
         * CMFM logs how long the search took, or that the cache answered. */
        const cmfm_scanner_t* scanner =
            (const cmfm_scanner_t*)api->get_interface(init->self, CMFM_SCANNER_INTERFACE, CMFM_SCANNER_VERSION);
        if (scanner) {
            void* match = NULL;
            const uint32_t count = scanner->find(
                init->self, "01001... 10001101 01001100 ..100100 00110000 01001... 10001101 01000010 11110101",
                CMFM_SCAN_CODE, &match, 1);
            say(CMFM_LOG_INFO, "version code found %u time(s), first at exe+0x%llx", count,
                match ? (unsigned long long)((const char*)match - (const char*)GetModuleHandleW(NULL)) : 0ull);
        }
        /* API 1.3: events from the game's Direct3D 12 calls. Subscribe now:
         * registration closes when PRE_MAIN ends. CMFM logs the outcome. */
        g_d3d12 = (const cmfm_d3d12_t*)api->get_interface(init->self, CMFM_D3D12_INTERFACE, CMFM_D3D12_VERSION);
        if (g_d3d12) {
            g_d3d12->subscribe(init->self,
                               CMFM_D3D12_EVENT_BIT(CMFM_D3D12_PRE_PRESENT) |
                                   CMFM_D3D12_EVENT_BIT(CMFM_D3D12_POST_RESIZE),
                               0, on_d3d12, NULL);
        }
    }
    return CMFM_OK;
}
