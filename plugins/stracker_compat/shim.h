#pragma once

// Private C interface between stracker_compat.dll (the AL plugin) and the
// loader.dll shim beside it. Both ship together; this is not a public API.

#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AL_STRACKER_HOST_VERSION 2u
#define AL_STRACKER_START_EXPORT "al_stracker_start"

/* Receives every log line that passes the level filter, without the line end.
 * level: 0 DEBUG, 1 INFO, 2 WARN, 3 ERR. */
typedef void(__cdecl* al_stracker_log_fn)(void* context, int level, const char* line);

typedef struct al_stracker_host {
    uint32_t size;
    uint32_t version;          /* AL_STRACKER_HOST_VERSION */
    al_stracker_log_fn log;  /* may be NULL */
    void* log_context;
    const wchar_t* game_folder; /* holds loader-config.json, loader.log, nativePC */
    const void* game_image;     /* module searched for the game version; NULL: the exe */
    /* Version 2: */
    const char* game_version;   /* the version AL already found (al.game), so the
                                   shim need not search again; NULL: search */
} al_stracker_host;

typedef enum al_stracker_config_state {
    AL_STRACKER_CONFIG_FOUND = 0,
    AL_STRACKER_CONFIG_MISSING = 1, /* Stracker's defaults */
    AL_STRACKER_CONFIG_INVALID = 2, /* not a JSON object: Stracker's defaults */
} al_stracker_config_state;

typedef struct al_stracker_result {
    uint32_t size;
    uint32_t already_started;
    uint32_t config_state;      /* al_stracker_config_state */
    uint32_t config_problems;   /* keys with an unexpected type or value */
    uint32_t plugin_loader_enabled;
    uint32_t plugins_found;
    uint32_t plugins_loaded;
    uint32_t plugins_failed;
    uint32_t version_found;
    char game_version[64];
    char first_failure[260];    /* UTF-8 file name of the first plugin that failed */
} al_stracker_result;

/* Runs Stracker's start-up once: config, old-file warning, game version,
 * nativePC\plugins. Returns 0, or 1 if it had already run. */
typedef uint32_t(__cdecl* al_stracker_start_fn)(const al_stracker_host* host, al_stracker_result* result);

#ifdef __cplusplus
}
#endif
