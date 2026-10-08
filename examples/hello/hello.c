/* hello: the smallest CMFM plugin, in plain C.
 *
 * It logs the folder paths CMFM hands it, which game this is (the "cmfm.game"
 * service, API 1.1) and when PRE_MAIN runs. Install
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

static void CMFM_CALL on_phase(cmfm_plugin_t* self, cmfm_phase_t phase, const void* data) {
    (void)self;
    (void)data;
    say(CMFM_LOG_INFO, "phase %u on thread %lu, %.1f ms after process start", phase, GetCurrentThreadId(),
        ms_since_process_start());
    if (phase == CMFM_PHASE_PRE_MAIN) say_module("game exe as the game sees it", NULL);
}

static const cmfm_plugin_info_t info = {
    sizeof(cmfm_plugin_info_t),
    CMFM_API_VERSION,
    "hello",      /* name: also the folder and DLL name */
    "CMFM Hello", /* display name */
    "0.2.0",      /* this plugin's version */
    CMFM_PHASE_BIT(CMFM_PHASE_PRE_MAIN),
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
    }
    return CMFM_OK;
}
