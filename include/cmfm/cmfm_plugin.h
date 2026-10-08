/* CMFM plugin API, version 1.0. C ABI, 64-bit Windows.
 *
 * A plugin is one DLL in its own folder:  <game>\cmfm\plugins\<name>\<name>.dll
 * Other files in that folder (its own DLLs, config, data) belong to the
 * plugin; CMFM loads only <name>.dll.
 *
 * A plugin exports two functions:
 *   const cmfm_plugin_info_t* cmfm_plugin_info(void);
 *   cmfm_result_t cmfm_plugin_init(const cmfm_api_t* api, const cmfm_init_t* init);
 *
 * Start-up, in this order:
 *   1. CMFM loads every plugin DLL and calls cmfm_plugin_info. Keep DllMain
 *      and cmfm_plugin_info free of work: a refused plugin is unloaded again.
 *   2. CMFM refuses plugins it cannot run (wrong API version, wrong game exe,
 *      name not matching the folder, load-order cycle), with a reason the user
 *      can see.
 *   3. CMFM calls cmfm_plugin_init in load order: `order` (lower first), then
 *      the load_after / load_before names, then the name.
 *
 * Rules:
 *   - Strings are UTF-8 and NUL-terminated; paths are UTF-16 (wchar_t).
 *   - The info struct and everything it points to stay valid while the
 *     plugin is loaded (use static data).
 *   - No C++ exceptions cross this boundary.
 *   - Once cmfm_plugin_init has been called, CMFM never unloads the plugin.
 *   - The api and init pointers, and the strings they hold, stay valid until
 *     the process exits. API functions may be called from any thread.
 *
 * Versions: major << 16 | minor. Within a major version, changes are
 * additive only (new fields at the end, `size` grows). CMFM refuses a plugin
 * built for another major version, or for a newer minor version than it
 * provides.
 */
#ifndef CMFM_PLUGIN_H
#define CMFM_PLUGIN_H

#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CMFM_MAKE_VERSION(major, minor) ((uint32_t)(((uint32_t)(major) << 16) | (uint32_t)(minor)))
#define CMFM_VERSION_MAJOR(version) ((uint32_t)(version) >> 16)
#define CMFM_VERSION_MINOR(version) ((uint32_t)(version) & 0xffffu)

#define CMFM_API_VERSION_MAJOR 1u
#define CMFM_API_VERSION_MINOR 0u
#define CMFM_API_VERSION CMFM_MAKE_VERSION(CMFM_API_VERSION_MAJOR, CMFM_API_VERSION_MINOR)

/* x64 Windows has one calling convention; this names it for readers. */
#define CMFM_CALL __cdecl
#define CMFM_PLUGIN_EXPORT __declspec(dllexport)

/* Plugin names: 1 to 64 characters from a-z, 0-9, '_', '-' and '.', not
 * starting with '.'. The name is also the folder and DLL name. */
#define CMFM_NAME_MAX 64u

typedef uint32_t cmfm_result_t;
#define CMFM_OK 0u
#define CMFM_ERROR 1u /* the plugin could not start; set_status says why */

/* Moments in the game's start-up a plugin can subscribe to. */
typedef uint32_t cmfm_phase_t;
#define CMFM_PHASE_PRE_MAIN 0u          /* game code unpacked, before its CRT starts */
#define CMFM_PHASE_WIN_MAIN 1u          /* the game's WinMain is about to run */
#define CMFM_PHASE_DEVICE_CREATED 2u    /* the game's D3D12 device exists */
#define CMFM_PHASE_SWAPCHAIN_CREATED 3u /* the game's swap chain exists */
#define CMFM_PHASE_FIRST_PRESENT 4u     /* the first frame is about to be presented */
#define CMFM_PHASE_GAME_READY 5u        /* title screen reached */
#define CMFM_PHASE_COUNT 6u
#define CMFM_PHASE_BIT(phase) (1u << (phase))

typedef uint32_t cmfm_log_level_t;
#define CMFM_LOG_DEBUG 0u
#define CMFM_LOG_INFO 1u
#define CMFM_LOG_WARNING 2u
#define CMFM_LOG_ERROR 3u

/* What the user sees for a plugin. */
typedef uint32_t cmfm_status_t;
#define CMFM_STATUS_ACTIVE 0u
#define CMFM_STATUS_DEGRADED 1u /* running with features off; give a reason */
#define CMFM_STATUS_DISABLED 2u /* not running; give a reason */

/* One per loaded plugin; CMFM owns it. */
typedef struct cmfm_plugin cmfm_plugin_t;

typedef void(CMFM_CALL* cmfm_phase_fn)(cmfm_plugin_t* self, cmfm_phase_t phase, const void* data);

/* Returned by the plugin's cmfm_plugin_info export. */
typedef struct cmfm_plugin_info_t {
    uint32_t size;                 /* sizeof(cmfm_plugin_info_t) */
    uint32_t api_version;          /* CMFM_API_VERSION */
    const char* name;              /* see CMFM_NAME_MAX; must equal the folder name */
    const char* display_name;      /* for menus and popups; NULL means name */
    const char* version;           /* the plugin's own version, e.g. "0.14.0" */
    uint32_t phases;               /* CMFM_PHASE_BIT mask of phases for on_phase */
    int32_t order;                 /* lower loads earlier; 0 is the default */
    const char* const* load_after; /* NULL-terminated plugin names, or NULL */
    const char* const* load_before;
    const char* const* exe_sha256; /* NULL-terminated lower-case SHA-256 hex of
                                      supported game exes, or NULL for any */
    cmfm_phase_fn on_phase;        /* NULL if phases is 0 */
} cmfm_plugin_info_t;

/* Passed to cmfm_plugin_init. */
typedef struct cmfm_init_t {
    uint32_t size; /* sizeof(cmfm_init_t) as CMFM was built */
    uint32_t reserved;
    cmfm_plugin_t* self;        /* pass to every API call */
    const wchar_t* folder;      /* the plugin folder as the game sees it (under
                                   MO2: the virtual game-folder path) */
    const wchar_t* folder_real; /* the same folder resolved through a file
                                   handle (under MO2: the real mod folder) */
} cmfm_init_t;

/* Services CMFM provides. Check `size` before using fields added after 1.0. */
typedef struct cmfm_api_t {
    uint32_t size;    /* sizeof(cmfm_api_t) as CMFM was built */
    uint32_t version; /* CMFM_API_VERSION of this CMFM */
    /* One line into the plugin's own log file in CMFM's log folder. */
    void(CMFM_CALL* log)(cmfm_plugin_t* self, cmfm_log_level_t level, const char* message);
    /* The status shown to the user. reason may be NULL for ACTIVE. */
    void(CMFM_CALL* set_status)(cmfm_plugin_t* self, cmfm_status_t status, const char* reason);
} cmfm_api_t;

typedef const cmfm_plugin_info_t*(CMFM_CALL* cmfm_plugin_info_fn)(void);
typedef cmfm_result_t(CMFM_CALL* cmfm_plugin_init_fn)(const cmfm_api_t* api, const cmfm_init_t* init);

#define CMFM_PLUGIN_INFO_EXPORT "cmfm_plugin_info"
#define CMFM_PLUGIN_INIT_EXPORT "cmfm_plugin_init"

#ifdef __cplusplus
}
#endif

#endif /* CMFM_PLUGIN_H */
