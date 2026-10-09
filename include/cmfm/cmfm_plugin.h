/* CMFM plugin API, version 1.3. C ABI, 64-bit Windows.
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
 *      name not matching the folder, a missing or too old required plugin,
 *      load-order cycle), with a reason the user can see.
 *   3. CMFM calls cmfm_plugin_init in load order: required plugins and the
 *      load_after / load_before names first, then `order` (lower first), then
 *      the name. A plugin whose required plugin failed to start is refused.
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
 * provides; a plugin built for an older minor version keeps working.
 *
 * Services (1.1): api->get_interface returns versioned tables by name, such
 * as "cmfm.game" and "cmfm.scanner" below, or tables other plugins publish
 * with api->publish_interface. Interface versions follow the same rule: same
 * major, at least the requested minor.
 *
 * Phases (1.2): besides PRE_MAIN, CMFM now dispatches WIN_MAIN,
 * DEVICE_CREATED and SHUTDOWN, each at most once and in that order, with a
 * payload struct as `data` (see below). 1.3 adds SWAPCHAIN_CREATED and
 * FIRST_PRESENT between DEVICE_CREATED and SHUTDOWN. GAME_READY is reserved
 * and does not fire yet.
 *
 * Direct3D 12 (1.3): the "cmfm.d3d12" service shares the game's DXGI and
 * D3D12 calls between plugins: events, and detours chained on one hook per
 * method.
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
#define CMFM_API_VERSION_MINOR 3u
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

/* Moments in the game's life a plugin can subscribe to. `data` is NULL for
 * PRE_MAIN and points to the struct named below for the others; it is valid
 * during the on_phase call only. */
typedef uint32_t cmfm_phase_t;
#define CMFM_PHASE_PRE_MAIN 0u          /* game code unpacked, before its CRT starts; main thread */
#define CMFM_PHASE_WIN_MAIN 1u          /* 1.2: the CRT is ready and WinMain is about to run;
                                           main thread; cmfm_phase_win_main_t */
#define CMFM_PHASE_DEVICE_CREATED 2u    /* 1.2: the game's own D3D12CreateDevice call returned
                                           a device, before the game uses it; the game's
                                           render-setup thread; cmfm_phase_device_t. Not
                                           under DirectX 11. */
#define CMFM_PHASE_SWAPCHAIN_CREATED 3u /* 1.3: the game created its first swap chain, before
                                           it uses it; the creating thread;
                                           cmfm_d3d12_swapchain_t */
#define CMFM_PHASE_FIRST_PRESENT 4u     /* 1.3: the game's first Present, before it reaches the
                                           swap chain and before PRE_PRESENT callbacks; the
                                           game's present thread; cmfm_d3d12_present_t */
#define CMFM_PHASE_GAME_READY 5u        /* reserved: does not fire yet */
#define CMFM_PHASE_SHUTDOWN 6u          /* 1.2: WinMain returned, before CRT teardown;
                                           save your own data now; main thread;
                                           cmfm_phase_shutdown_t. Does not fire if the game
                                           ends the process another way. */
#define CMFM_PHASE_COUNT 7u
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

/* 1.2 phase payloads. Check `size` before reading fields added later. */
typedef struct cmfm_phase_win_main_t {
    uint32_t size;
    uint32_t reserved;
    void* instance;           /* WinMain's HINSTANCE: the game exe's base */
    const char* command_line; /* WinMain's command line, without the exe */
    int32_t show_command;     /* WinMain's nCmdShow */
    uint32_t reserved2;
} cmfm_phase_win_main_t;

typedef struct cmfm_phase_device_t {
    uint32_t size;
    uint32_t reserved;
    void* device;      /* the device as the game receives it, as the interface the
                          game asked for (riid); AddRef it to keep it */
    const void* riid;  /* the game's requested IID (const GUID*) */
    void* adapter;     /* the adapter the game passed (IUnknown*), or NULL */
    uint32_t minimum_feature_level; /* D3D_FEATURE_LEVEL the game asked for */
    uint32_t reserved2;
} cmfm_phase_device_t;

typedef struct cmfm_phase_shutdown_t {
    uint32_t size;
    int32_t exit_code; /* what WinMain returned */
} cmfm_phase_shutdown_t;

/* 1.1: a plugin this one needs. */
typedef struct cmfm_dependency_t {
    const char* name;        /* NULL ends the list */
    const char* min_version; /* dotted numbers, e.g. "0.14" or "0.14.0"; NULL for any version */
} cmfm_dependency_t;

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
    /* Added in 1.1: */
    const cmfm_dependency_t* dependencies; /* plugins that must be installed, new
                                              enough and started before this one;
                                              NULL, or a list ended by an entry
                                              with name NULL */
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
    /* Added in 1.1: */
    const wchar_t* data_folder; /* a writable folder for this plugin's own data,
                                   cmfm\data\<name> beside CMFM's logs; NULL if it
                                   could not be created */
} cmfm_init_t;

/* Services CMFM provides. Check `size` before using fields added after 1.0. */
typedef struct cmfm_api_t {
    uint32_t size;    /* sizeof(cmfm_api_t) as CMFM was built */
    uint32_t version; /* CMFM_API_VERSION of this CMFM */
    /* One line into the plugin's own log file in CMFM's log folder. */
    void(CMFM_CALL* log)(cmfm_plugin_t* self, cmfm_log_level_t level, const char* message);
    /* The status shown to the user. reason may be NULL for ACTIVE. */
    void(CMFM_CALL* set_status)(cmfm_plugin_t* self, cmfm_status_t status, const char* reason);
    /* Added in 1.1: */
    /* A service table by name, or NULL if there is none with the same major
     * version and at least the requested minor version. Tables stay valid
     * until the process exits. */
    const void*(CMFM_CALL* get_interface)(cmfm_plugin_t* self, const char* name, uint32_t version);
    /* Publishes a table for other plugins. The name starts with this plugin's
     * name and a dot ("mhw_dlss.status"); the table must stay valid until the
     * process exits and should start with uint32_t size and version.
     * CMFM_ERROR if the name is invalid or taken. */
    cmfm_result_t(CMFM_CALL* publish_interface)(cmfm_plugin_t* self, const char* name, uint32_t version,
                                                const void* table);
} cmfm_api_t;

/* "cmfm.game" 1.0: which game this is. */
#define CMFM_GAME_INTERFACE "cmfm.game"
#define CMFM_GAME_VERSION CMFM_MAKE_VERSION(1, 0)

typedef uint32_t cmfm_graphics_t;
#define CMFM_GRAPHICS_UNKNOWN 0u
#define CMFM_GRAPHICS_DX11 1u
#define CMFM_GRAPHICS_DX12 2u

typedef struct cmfm_game_t {
    uint32_t size;
    uint32_t version;           /* CMFM_GAME_VERSION */
    const char* exe_sha256;     /* lower-case hex; NULL if unavailable */
    const char* game_version;   /* MHW's build string, e.g. "421810"; NULL if not found */
    const wchar_t* exe_path;    /* the exe as the game sees it (virtual under MO2) */
    const wchar_t* game_folder; /* the folder holding it */
    uint32_t pe_timestamp;      /* the exe's PE header time stamp */
    cmfm_graphics_t graphics;   /* what graphics_option.ini selects (DirectX12Enable)
                                   at start-up, not what the game created */
} cmfm_game_t;

/* "cmfm.scanner" 1.0: find code in the game exe. */
#define CMFM_SCANNER_INTERFACE "cmfm.scanner"
#define CMFM_SCANNER_VERSION CMFM_MAKE_VERSION(1, 0)

#define CMFM_SCAN_CODE 0u          /* the exe's executable sections */
#define CMFM_SCAN_IMAGE 1u         /* every readable part of the exe */
#define CMFM_SCAN_INVALID 0xFFFFFFFFu
#define CMFM_SCAN_MAX_MATCHES 1024u

typedef struct cmfm_scanner_t {
    uint32_t size;
    uint32_t version; /* CMFM_SCANNER_VERSION */
    /* Finds `pattern` in the game exe: hex bytes with "?" or "??" for any byte
     * ("48 8B ?? 05"), or Stracker's bits, eight per token ("01001...
     * 10001101"). Writes the first `capacity` matches, in address order, and
     * returns how many there are (at most CMFM_SCAN_MAX_MATCHES), or
     * CMFM_SCAN_INVALID for a pattern it cannot read. Results are cached per
     * exe hash, so later launches skip the search. Call from PRE_MAIN on. */
    uint32_t(CMFM_CALL* find)(cmfm_plugin_t* self, const char* pattern, uint32_t where, void** matches,
                              uint32_t capacity);
    /* The target of a RIP-relative operand: instruction + length + the signed
     * 32-bit value at instruction + offset. NULL if those bytes are unreadable. */
    void*(CMFM_CALL* rip_target)(const void* instruction, uint32_t offset, uint32_t length);
} cmfm_scanner_t;

/* "cmfm.d3d12" 1.0 (API 1.3): the game's Direct3D 12 calls, shared.
 *
 * CMFM puts one hook on each DXGI/D3D12 method a plugin asks for and calls
 * every plugin's detour for it in a defined order, so plugins never patch
 * these functions themselves and never stack patches on each other.
 *
 * Two ways in:
 *   - subscribe: callbacks for common events (swap chain created, before and
 *     after Present, around ResizeBuffers). Enough for overlays, menus and
 *     counters.
 *   - hook: a detour with the method's own signature, as with MinHook. CMFM
 *     stores the function to continue with in *next before the first call.
 *     Detours on IDXGIFactory and IDXGISwapChain methods need the
 *     presentation role (claim_presentation), which one plugin holds.
 *
 * Register from cmfm_plugin_init or a PRE_MAIN on_phase call. Registration
 * closes when PRE_MAIN ends; CMFM then hooks only the methods plugins asked
 * for, nothing when none did. Under DirectX 11, or when CMFM cannot hook
 * safely, nothing is called; status() says why.
 *
 * Order: lower `order` runs first, further from the original method; ties
 * run in load order. CMFM's own event layer runs before every detour, so
 * PRE_PRESENT callbacks draw before the presentation owner's Present.
 *
 * Internal calls: between begin_internal and end_internal, calls the thread
 * makes to any hooked method go straight to the original, past every detour
 * and event. A detour that routes a call into code which itself calls DXGI
 * or D3D12 (Streamline's proxies, say) wraps that call, so every plugin sees
 * each game call once. CMFM's events also skip a Present or ResizeBuffers
 * that the same thread makes from inside another one.
 *
 * Threads: everything runs synchronously on the thread making the call.
 * Present events fire for the game's present thread only, the first thread
 * to present; detours see every thread. Present runs every frame: keep
 * callbacks short and do not allocate. Objects in event data are the ones
 * the game holds; AddRef any you keep. A callback that raises an exception
 * disables its plugin, as for on_phase. Detours run directly in the chain,
 * like any hook, so CMFM cannot catch their faults. */
#define CMFM_D3D12_INTERFACE "cmfm.d3d12"
#define CMFM_D3D12_VERSION CMFM_MAKE_VERSION(1, 0)

typedef uint32_t cmfm_d3d12_event_t;
#define CMFM_D3D12_SWAPCHAIN_CREATED 0u /* the game created a swap chain; cmfm_d3d12_swapchain_t */
#define CMFM_D3D12_PRE_PRESENT 1u       /* the game called Present or Present1; draw overlays into
                                           the buffer being presented; cmfm_d3d12_present_t */
#define CMFM_D3D12_POST_PRESENT 2u      /* it returned; cmfm_d3d12_present_t with result */
#define CMFM_D3D12_PRE_RESIZE 3u        /* the game called ResizeBuffers or ResizeBuffers1; release
                                           every reference to the swap chain's buffers now;
                                           cmfm_d3d12_resize_t */
#define CMFM_D3D12_POST_RESIZE 4u       /* it returned; take new references; cmfm_d3d12_resize_t
                                           with result */
#define CMFM_D3D12_EVENT_COUNT 5u
#define CMFM_D3D12_EVENT_BIT(event) (1u << (event))

/* The game's swap chain and the objects around it. Event data points to
 * CMFM's current copy, which changes when the game resizes or recreates. */
typedef struct cmfm_d3d12_swapchain_t {
    uint32_t size;
    uint32_t reserved;
    void* swapchain; /* the IDXGISwapChain the game holds */
    void* queue;     /* the ID3D12CommandQueue the game created it with */
    void* device;    /* the ID3D12Device the game uses (as at DEVICE_CREATED) */
    void* window;    /* HWND */
    uint32_t width;
    uint32_t height;
    uint32_t format; /* DXGI_FORMAT */
    uint32_t buffer_count;
    uint32_t flags;  /* DXGI_SWAP_CHAIN_FLAG bits */
    uint32_t reserved2;
} cmfm_d3d12_swapchain_t;

typedef struct cmfm_d3d12_present_t {
    uint32_t size;
    uint32_t reserved;
    const cmfm_d3d12_swapchain_t* swapchain;
    uint64_t frame;         /* presents before this one: 0 for the game's first */
    uint32_t back_buffer;   /* index of the buffer being presented */
    uint32_t sync_interval;
    uint32_t flags;         /* DXGI_PRESENT_* bits */
    int32_t result;         /* POST_PRESENT: the HRESULT Present returned; 0 before */
    int64_t present_qpc;    /* QueryPerformanceCounter when the game called Present */
    int64_t previous_qpc;   /* the same for the previous present; 0 for the first */
} cmfm_d3d12_present_t;

typedef struct cmfm_d3d12_resize_t {
    uint32_t size;
    uint32_t reserved;
    const cmfm_d3d12_swapchain_t* swapchain; /* PRE: as it is; POST: as it is now */
    uint32_t buffer_count;  /* what the game asked for; 0 keeps the current count */
    uint32_t width;         /* 0: the window's client width */
    uint32_t height;
    uint32_t format;        /* DXGI_FORMAT; 0 keeps the current format */
    uint32_t flags;
    int32_t result;         /* POST_RESIZE: the HRESULT ResizeBuffers returned; 0 before */
} cmfm_d3d12_resize_t;

typedef void(CMFM_CALL* cmfm_d3d12_event_fn)(cmfm_plugin_t* self, cmfm_d3d12_event_t event, const void* data,
                                             void* user);

/* A method: a COM interface family and the vtable slot, counted from
 * QueryInterface = 0 as in the Windows SDK headers. */
typedef uint32_t cmfm_d3d12_method_t;
#define CMFM_D3D12_METHOD(family, slot) ((cmfm_d3d12_method_t)(((uint32_t)(family) << 16) | (uint32_t)(slot)))
#define CMFM_D3D12_METHOD_FAMILY(method) ((uint32_t)(method) >> 16)
#define CMFM_D3D12_METHOD_SLOT(method) ((uint32_t)(method) & 0xffffu)
#define CMFM_D3D12_FACTORY 1u      /* IDXGIFactory up to IDXGIFactory7; presentation role */
#define CMFM_D3D12_SWAPCHAIN 2u    /* IDXGISwapChain up to IDXGISwapChain4; presentation role */
#define CMFM_D3D12_DEVICE 3u       /* ID3D12Device up to the newest the system has */
#define CMFM_D3D12_QUEUE 4u        /* ID3D12CommandQueue */
#define CMFM_D3D12_COMMAND_LIST 5u /* ID3D12GraphicsCommandList up to the newest */
#define CMFM_D3D12_SLOT_LIMIT 256u /* slots are below this */

#define CMFM_D3D12_FACTORY_CREATE_SWAPCHAIN CMFM_D3D12_METHOD(CMFM_D3D12_FACTORY, 10)
#define CMFM_D3D12_FACTORY_CREATE_SWAPCHAIN_FOR_HWND CMFM_D3D12_METHOD(CMFM_D3D12_FACTORY, 15)
#define CMFM_D3D12_SWAPCHAIN_PRESENT CMFM_D3D12_METHOD(CMFM_D3D12_SWAPCHAIN, 8)
#define CMFM_D3D12_SWAPCHAIN_GET_BUFFER CMFM_D3D12_METHOD(CMFM_D3D12_SWAPCHAIN, 9)
#define CMFM_D3D12_SWAPCHAIN_SET_FULLSCREEN_STATE CMFM_D3D12_METHOD(CMFM_D3D12_SWAPCHAIN, 10)
#define CMFM_D3D12_SWAPCHAIN_RESIZE_BUFFERS CMFM_D3D12_METHOD(CMFM_D3D12_SWAPCHAIN, 13)
#define CMFM_D3D12_SWAPCHAIN_RESIZE_TARGET CMFM_D3D12_METHOD(CMFM_D3D12_SWAPCHAIN, 14)
#define CMFM_D3D12_SWAPCHAIN_PRESENT1 CMFM_D3D12_METHOD(CMFM_D3D12_SWAPCHAIN, 22)
#define CMFM_D3D12_SWAPCHAIN_GET_CURRENT_BACK_BUFFER_INDEX CMFM_D3D12_METHOD(CMFM_D3D12_SWAPCHAIN, 36)
#define CMFM_D3D12_SWAPCHAIN_RESIZE_BUFFERS1 CMFM_D3D12_METHOD(CMFM_D3D12_SWAPCHAIN, 39)
#define CMFM_D3D12_DEVICE_CREATE_COMMAND_QUEUE CMFM_D3D12_METHOD(CMFM_D3D12_DEVICE, 8)
#define CMFM_D3D12_QUEUE_EXECUTE_COMMAND_LISTS CMFM_D3D12_METHOD(CMFM_D3D12_QUEUE, 10)
#define CMFM_D3D12_QUEUE_SIGNAL CMFM_D3D12_METHOD(CMFM_D3D12_QUEUE, 14)

typedef struct cmfm_d3d12_hook_t {
    cmfm_d3d12_method_t method;
    int32_t order;  /* lower runs first; ties in load order */
    void* detour;   /* the plugin's function, with the method's exact signature */
    void** next;    /* where CMFM stores the function the detour continues with:
                       the next detour, or the original method */
} cmfm_d3d12_hook_t;

typedef uint32_t cmfm_d3d12_state_t;
#define CMFM_D3D12_WAITING 0u     /* registration open, or no hooked object seen yet */
#define CMFM_D3D12_RUNNING 1u     /* hooks are in place */
#define CMFM_D3D12_UNAVAILABLE 2u /* nothing will be called; reason says why */

typedef struct cmfm_d3d12_status_t {
    uint32_t size;            /* set to sizeof(cmfm_d3d12_status_t) before the call */
    cmfm_d3d12_state_t state;
    const char* reason;       /* UNAVAILABLE: why; otherwise NULL */
    const char* owner;        /* the plugin holding the presentation role, or NULL */
    uint64_t frames;          /* presents seen on the game's present thread */
    int64_t last_present_qpc; /* 0 before the first */
} cmfm_d3d12_status_t;

typedef struct cmfm_d3d12_t {
    uint32_t size;
    uint32_t version; /* CMFM_D3D12_VERSION */
    /* Calls `callback` for each event in `events` (CMFM_D3D12_EVENT_BIT
     * mask). A plugin may subscribe more than once. CMFM_ERROR, with the
     * reason in the plugin's log, after registration closed, or for an empty
     * or unknown mask or no callback. */
    cmfm_result_t(CMFM_CALL* subscribe)(cmfm_plugin_t* self, uint32_t events, int32_t order,
                                        cmfm_d3d12_event_fn callback, void* user);
    /* Registers `count` detours, all or none: CMFM_ERROR, with the reason in
     * the plugin's log, if any is invalid, if registration closed, or if a
     * FACTORY or SWAPCHAIN method is asked for without the presentation role.
     * The same plugin may not hook a method twice. *next stays NULL if CMFM
     * never hooks the method (DirectX 11, say). */
    cmfm_result_t(CMFM_CALL* hook)(cmfm_plugin_t* self, const cmfm_d3d12_hook_t* hooks, uint32_t count);
    /* Takes the presentation role: the right to detour IDXGIFactory and
     * IDXGISwapChain methods, for a plugin that routes presentation, such as
     * frame generation. The first claim wins; a later one gets CMFM_ERROR
     * and its log names the holder. */
    cmfm_result_t(CMFM_CALL* claim_presentation)(cmfm_plugin_t* self);
    /* Marks the calling thread's following calls as internal (see above),
     * until the matching end_internal. Pairs nest. A thread-local counter:
     * cheap enough for every frame. */
    void(CMFM_CALL* begin_internal)(cmfm_plugin_t* self);
    void(CMFM_CALL* end_internal)(cmfm_plugin_t* self);
    /* Fills *status up to status->size. Any thread; the strings stay valid
     * until the process exits. */
    void(CMFM_CALL* status)(cmfm_plugin_t* self, cmfm_d3d12_status_t* status);
} cmfm_d3d12_t;

typedef const cmfm_plugin_info_t*(CMFM_CALL* cmfm_plugin_info_fn)(void);
typedef cmfm_result_t(CMFM_CALL* cmfm_plugin_init_fn)(const cmfm_api_t* api, const cmfm_init_t* init);

#define CMFM_PLUGIN_INFO_EXPORT "cmfm_plugin_info"
#define CMFM_PLUGIN_INIT_EXPORT "cmfm_plugin_init"

#ifdef __cplusplus
}
#endif

#endif /* CMFM_PLUGIN_H */
