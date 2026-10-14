/* AL plugin API, version 1.8. C ABI, 64-bit Windows.
 *
 * A plugin is one DLL in its own folder:  <game>\AsteraLoader\plugins\<name>\<name>.dll
 * Other files in that folder (its own DLLs, config, data) belong to the
 * plugin; AL loads only <name>.dll.
 *
 * A plugin exports two functions:
 *   const al_plugin_info_t* al_plugin_info(void);
 *   al_result_t al_plugin_init(const al_api_t* api, const al_init_t* init);
 *
 * Start-up, in this order:
 *   1. AL loads every plugin DLL and calls al_plugin_info. Keep DllMain
 *      and al_plugin_info free of work: a refused plugin is unloaded again.
 *   2. AL refuses plugins it cannot run (wrong API version, wrong game exe,
 *      name not matching the folder, a missing or too old required plugin,
 *      load-order cycle), with a reason the user can see.
 *   3. AL calls al_plugin_init in load order: required plugins and the
 *      load_after / load_before names first, then `order` (lower first), then
 *      the name. A plugin whose required plugin failed to start is refused.
 *
 * Rules:
 *   - Strings are UTF-8 and NUL-terminated; paths are UTF-16 (wchar_t).
 *   - The info struct and everything it points to stay valid while the
 *     plugin is loaded (use static data).
 *   - No C++ exceptions cross this boundary.
 *   - Once al_plugin_init has been called, AL never unloads the plugin, so
 *     a hook's detour or a value written through al.patch may point into
 *     the plugin's DLL until the process exits.
 *   - The api and init pointers, and the strings they hold, stay valid until
 *     the process exits. API functions may be called from any thread.
 *
 * Versions: major << 16 | minor. Within a major version, changes are
 * additive only (new fields at the end, `size` grows). AL refuses a plugin
 * built for another major version, or for a newer minor version than it
 * provides; a plugin built for an older minor version keeps working.
 *   - Structs a plugin passes in: AL accepts any `size` at least the size
 *     the struct had in the version that introduced it, and reads a later
 *     field only when `size` covers it.
 *   - Structs AL passes out: check `size` before reading a field added
 *     after the version the plugin was built for.
 *   - Results: any value other than AL_OK is a failure; later versions may
 *     add codes. Functions returning a handle return 0 on failure.
 *
 * Services (1.1): api->get_interface returns versioned tables by name, such
 * as "al.game" and "al.scanner" below, or tables other plugins publish
 * with api->publish_interface. Interface versions follow the same rule: same
 * major, at least the requested minor.
 *
 * Phases (1.2): besides PRE_MAIN, AL now dispatches WIN_MAIN,
 * DEVICE_CREATED and SHUTDOWN, each at most once and in that order, with a
 * payload struct as `data` (see below). 1.3 adds SWAPCHAIN_CREATED and
 * FIRST_PRESENT between DEVICE_CREATED and SHUTDOWN. 1.5 adds EXITING,
 * before SHUTDOWN, and 1.7 GAME_READY, when the title screen starts.
 * Phase numbers are names, not firing order. Phases fire in this order:
 * PRE_MAIN, WIN_MAIN, DEVICE_CREATED, SWAPCHAIN_CREATED, FIRST_PRESENT,
 * GAME_READY, EXITING, SHUTDOWN.
 *
 * Direct3D 12 (1.3): the "al.d3d12" service shares the game's DXGI and
 * D3D12 calls between plugins: events, and detours chained on one hook per
 * method.
 *
 * Game functions (1.4): the "al.hooks" service does the same for
 * functions in the game's own code.
 *
 * Frames (1.5): the "al.frame" service calls plugins on the game's frame
 * thread every frame and runs tasks posted to it from any thread.
 *
 * Input (1.6): the "al.input" service gives hotkeys, key and pad state from
 * the game's own reads, the game window's messages, and input capture for
 * menus.
 *
 * Game ready (1.7): AL_PHASE_GAME_READY fires once the game has booted and
 * its title screen starts.
 *
 * Byte changes (1.8): the "al.patch" service changes bytes in the game's
 * code and data, checked and with one owner per byte, shared with al.hooks.
 */
#ifndef AL_PLUGIN_H
#define AL_PLUGIN_H

#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AL_MAKE_VERSION(major, minor) ((uint32_t)(((uint32_t)(major) << 16) | (uint32_t)(minor)))
#define AL_VERSION_MAJOR(version) ((uint32_t)(version) >> 16)
#define AL_VERSION_MINOR(version) ((uint32_t)(version) & 0xffffu)

#define AL_API_VERSION_MAJOR 1u
#define AL_API_VERSION_MINOR 8u
#define AL_API_VERSION AL_MAKE_VERSION(AL_API_VERSION_MAJOR, AL_API_VERSION_MINOR)

/* x64 Windows has one calling convention; this names it for readers. */
#define AL_CALL __cdecl
#define AL_PLUGIN_EXPORT __declspec(dllexport)

/* Plugin names: 1 to 64 characters from a-z, 0-9, '_', '-' and '.', not
 * starting with '.'. The name is also the folder and DLL name. */
#define AL_NAME_MAX 64u

typedef uint32_t al_result_t;
#define AL_OK 0u
#define AL_ERROR 1u /* failed; for al_plugin_init, the plugin could not start and
                       set_status says why */

/* Moments in the game's life a plugin can subscribe to. `data` is NULL for
 * PRE_MAIN and points to the struct named below for the others; it is valid
 * during the on_phase call only. */
typedef uint32_t al_phase_t;
#define AL_PHASE_PRE_MAIN 0u          /* game code unpacked, before its CRT starts; main thread */
#define AL_PHASE_WIN_MAIN 1u          /* 1.2: the CRT is ready and WinMain is about to run;
                                           main thread; al_phase_win_main_t */
#define AL_PHASE_DEVICE_CREATED 2u    /* 1.2: the game's own D3D12CreateDevice call returned
                                           a device, before the game uses it; the game's
                                           render-setup thread; al_phase_device_t. Not
                                           under DirectX 11. */
#define AL_PHASE_SWAPCHAIN_CREATED 3u /* 1.3: the game created its first swap chain, before
                                           it uses it; the creating thread;
                                           al_d3d12_swapchain_t */
#define AL_PHASE_FIRST_PRESENT 4u     /* 1.3: the game's first Present, before it reaches the
                                           swap chain and before PRE_PRESENT callbacks; the
                                           game's present thread; al_d3d12_present_t */
#define AL_PHASE_GAME_READY 5u        /* 1.7: the game finished booting and its title
                                           screen started; game data and the game's own
                                           systems are set up. Once, on the game's frame
                                           thread at the end of that frame, after al.frame
                                           END; al_phase_game_ready_t. Not on executables
                                           AL does not know, nor if the game quits first. */
#define AL_PHASE_SHUTDOWN 6u          /* 1.2: WinMain returned, before CRT teardown;
                                           save your own data now; main thread;
                                           al_phase_shutdown_t. Does not fire if the game
                                           ends the process another way. */
#define AL_PHASE_EXITING 7u          /* 1.5: the game started its own quit (Exit Game, or a
                                           window close through WM_QUIT), before it tears
                                           anything down; stop your own threads now. Usually
                                           the WinMain thread; no payload (NULL). Does not
                                           fire when the process is ended from outside
                                           (Task Manager) or by a crash. */
#define AL_PHASE_COUNT 8u
#define AL_PHASE_BIT(phase) (1u << (phase))

typedef uint32_t al_log_level_t;
#define AL_LOG_DEBUG 0u
#define AL_LOG_INFO 1u
#define AL_LOG_WARNING 2u
#define AL_LOG_ERROR 3u

/* What the user sees for a plugin. */
typedef uint32_t al_status_t;
#define AL_STATUS_ACTIVE 0u
#define AL_STATUS_DEGRADED 1u /* running with features off; give a reason */
#define AL_STATUS_DISABLED 2u /* not running; give a reason */

/* One per loaded plugin; AL owns it. */
typedef struct al_plugin al_plugin_t;

typedef void(AL_CALL* al_phase_fn)(al_plugin_t* self, al_phase_t phase, const void* data);

/* 1.2 phase payloads. Check `size` before reading fields added later. */
typedef struct al_phase_win_main_t {
    uint32_t size;
    uint32_t reserved;
    void* instance;           /* WinMain's HINSTANCE: the game exe's base */
    const char* command_line; /* WinMain's command line, without the exe */
    int32_t show_command;     /* WinMain's nCmdShow */
    uint32_t reserved2;
} al_phase_win_main_t;

typedef struct al_phase_device_t {
    uint32_t size;
    uint32_t reserved;
    void* device;      /* the device as the game receives it, as the interface the
                          game asked for (riid); AddRef it to keep it */
    const void* riid;  /* the game's requested IID (const GUID*) */
    void* adapter;     /* the adapter the game passed (IUnknown*), or NULL */
    uint32_t minimum_feature_level; /* D3D_FEATURE_LEVEL the game asked for */
    uint32_t reserved2;
} al_phase_device_t;

typedef struct al_phase_shutdown_t {
    uint32_t size;
    int32_t exit_code; /* what WinMain returned */
} al_phase_shutdown_t;

/* 1.7: GAME_READY's payload. */
#define AL_GAME_READY_TITLE 1u /* the title screen started */
typedef struct al_phase_game_ready_t {
    uint32_t size;
    uint32_t source; /* what AL saw: AL_GAME_READY_TITLE */
    uint64_t frame;  /* the al.frame index of the frame it fired at the end of */
} al_phase_game_ready_t;

/* 1.1: a plugin this one needs. */
typedef struct al_dependency_t {
    const char* name;        /* NULL ends the list */
    const char* min_version; /* dotted numbers, e.g. "0.14" or "0.14.0"; NULL for any version */
} al_dependency_t;

/* Returned by the plugin's al_plugin_info export. */
typedef struct al_plugin_info_t {
    uint32_t size;                 /* sizeof(al_plugin_info_t) */
    uint32_t api_version;          /* AL_API_VERSION */
    const char* name;              /* see AL_NAME_MAX; must equal the folder name */
    const char* display_name;      /* for menus and popups; NULL means name */
    const char* version;           /* the plugin's own version, e.g. "0.14.0" */
    uint32_t phases;               /* AL_PHASE_BIT mask of phases for on_phase */
    int32_t order;                 /* lower loads earlier; 0 is the default */
    const char* const* load_after; /* NULL-terminated plugin names, or NULL */
    const char* const* load_before;
    const char* const* exe_sha256; /* NULL-terminated lower-case SHA-256 hex of
                                      supported game exes, or NULL for any */
    al_phase_fn on_phase;        /* NULL if phases is 0 */
    /* Added in 1.1: */
    const al_dependency_t* dependencies; /* plugins that must be installed, new
                                              enough and started before this one;
                                              NULL, or a list ended by an entry
                                              with name NULL */
} al_plugin_info_t;

/* Passed to al_plugin_init. */
typedef struct al_init_t {
    uint32_t size; /* sizeof(al_init_t) as AL was built */
    uint32_t reserved;
    al_plugin_t* self;        /* pass to every API call */
    const wchar_t* folder;      /* the plugin folder as the game sees it (under
                                   MO2: the virtual game-folder path) */
    const wchar_t* folder_real; /* the same folder resolved through a file
                                   handle (under MO2: the real mod folder) */
    /* Added in 1.1: */
    const wchar_t* data_folder; /* a writable folder for this plugin's own data,
                                   AsteraLoader\data\<name> beside AL's logs; NULL if it
                                   could not be created */
} al_init_t;

/* Services AL provides. Check `size` before using fields added after 1.0. */
typedef struct al_api_t {
    uint32_t size;    /* sizeof(al_api_t) as AL was built */
    uint32_t version; /* AL_API_VERSION of this AL */
    /* One line into the plugin's own log file in AL's log folder. */
    void(AL_CALL* log)(al_plugin_t* self, al_log_level_t level, const char* message);
    /* The status shown to the user. reason may be NULL for ACTIVE. */
    void(AL_CALL* set_status)(al_plugin_t* self, al_status_t status, const char* reason);
    /* Added in 1.1: */
    /* A service table by name, or NULL if there is none with the same major
     * version and at least the requested minor version. Tables stay valid
     * until the process exits. */
    const void*(AL_CALL* get_interface)(al_plugin_t* self, const char* name, uint32_t version);
    /* Publishes a table for other plugins. The name starts with this plugin's
     * name and a dot ("mhw_dlss.status"); the table must stay valid until the
     * process exits and should start with uint32_t size and version.
     * AL_ERROR if the name is invalid or taken. */
    al_result_t(AL_CALL* publish_interface)(al_plugin_t* self, const char* name, uint32_t version,
                                                const void* table);
} al_api_t;

/* "al.game" 1.0: which game this is. */
#define AL_GAME_INTERFACE "al.game"
#define AL_GAME_VERSION AL_MAKE_VERSION(1, 0)

typedef uint32_t al_graphics_t;
#define AL_GRAPHICS_UNKNOWN 0u
#define AL_GRAPHICS_DX11 1u
#define AL_GRAPHICS_DX12 2u

typedef struct al_game_t {
    uint32_t size;
    uint32_t version;           /* AL_GAME_VERSION */
    const char* exe_sha256;     /* lower-case hex; NULL if unavailable */
    const char* game_version;   /* MHW's build string, e.g. "421810"; NULL if not found */
    const wchar_t* exe_path;    /* the exe as the game sees it (virtual under MO2) */
    const wchar_t* game_folder; /* the folder holding it */
    uint32_t pe_timestamp;      /* the exe's PE header time stamp */
    al_graphics_t graphics;   /* what graphics_option.ini selects (DirectX12Enable)
                                   at start-up, not what the game created */
} al_game_t;

/* "al.scanner" 1.0: find code in the game exe. */
#define AL_SCANNER_INTERFACE "al.scanner"
#define AL_SCANNER_VERSION AL_MAKE_VERSION(1, 0)

#define AL_SCAN_CODE 0u          /* the exe's executable sections */
#define AL_SCAN_IMAGE 1u         /* every readable part of the exe */
#define AL_SCAN_INVALID 0xFFFFFFFFu
#define AL_SCAN_MAX_MATCHES 1024u

typedef struct al_scanner_t {
    uint32_t size;
    uint32_t version; /* AL_SCANNER_VERSION */
    /* Finds `pattern` in the game exe: hex bytes with "?" or "??" for any byte
     * ("48 8B ?? 05"), or Stracker's bits, eight per token ("01001...
     * 10001101"). Writes the first `capacity` matches, in address order, and
     * returns how many there are (at most AL_SCAN_MAX_MATCHES), or
     * AL_SCAN_INVALID for a pattern it cannot read. Results are cached per
     * exe hash, so later launches skip the search. Call from PRE_MAIN on. */
    uint32_t(AL_CALL* find)(al_plugin_t* self, const char* pattern, uint32_t where, void** matches,
                              uint32_t capacity);
    /* The target of a RIP-relative operand: instruction + length + the signed
     * 32-bit value at instruction + offset. NULL if those bytes are unreadable. */
    void*(AL_CALL* rip_target)(const void* instruction, uint32_t offset, uint32_t length);
} al_scanner_t;

/* "al.d3d12" 1.0 (API 1.3): the game's Direct3D 12 calls, shared.
 *
 * AL puts one hook on each DXGI/D3D12 method a plugin asks for and calls
 * every plugin's detour for it in a defined order, so plugins never patch
 * these functions themselves and never stack patches on each other.
 *
 * Two ways in:
 *   - subscribe: callbacks for common events (swap chain created, before and
 *     after Present, around ResizeBuffers). Enough for overlays, menus and
 *     counters.
 *   - hook: a detour with the method's own signature, as with MinHook. AL
 *     stores the function to continue with in *next before the first call.
 *     Detours on IDXGIFactory and IDXGISwapChain methods need the
 *     presentation role (claim_presentation), which one plugin holds.
 *
 * Register from al_plugin_init or a PRE_MAIN on_phase call. Registration
 * closes when PRE_MAIN ends; AL then hooks only the methods plugins asked
 * for, nothing when none did. Under DirectX 11, or when AL cannot hook
 * safely, nothing is called; status() says why.
 *
 * Order: lower `order` runs first, further from the original method; ties
 * run in load order. AL's own event layer runs before every detour, so
 * PRE_PRESENT callbacks draw before the presentation owner's Present.
 *
 * Internal calls: between begin_internal and end_internal, calls the thread
 * makes to any hooked method go straight to the original, past every detour
 * and event. A detour that routes a call into code which itself calls DXGI
 * or D3D12 (Streamline's proxies, say) wraps that call, so every plugin sees
 * each game call once. AL's events also skip a Present or ResizeBuffers
 * that the same thread makes from inside another one.
 *
 * Threads: everything runs synchronously on the thread making the call.
 * Present events fire for the game's present thread only, the first thread
 * to present; detours see every thread. Present runs every frame: keep
 * callbacks short and do not allocate. Objects in event data are the ones
 * the game holds; AddRef any you keep. A callback that raises an exception
 * disables its plugin, as for on_phase. Detours run directly in the chain,
 * like any hook, so AL cannot catch their faults. */
#define AL_D3D12_INTERFACE "al.d3d12"
#define AL_D3D12_VERSION AL_MAKE_VERSION(1, 0)

typedef uint32_t al_d3d12_event_t;
#define AL_D3D12_SWAPCHAIN_CREATED 0u /* the game created a swap chain; al_d3d12_swapchain_t */
#define AL_D3D12_PRE_PRESENT 1u       /* the game called Present or Present1; draw overlays into
                                           the buffer being presented; al_d3d12_present_t */
#define AL_D3D12_POST_PRESENT 2u      /* it returned; al_d3d12_present_t with result */
#define AL_D3D12_PRE_RESIZE 3u        /* the game called ResizeBuffers or ResizeBuffers1; release
                                           every reference to the swap chain's buffers now;
                                           al_d3d12_resize_t */
#define AL_D3D12_POST_RESIZE 4u       /* it returned; take new references; al_d3d12_resize_t
                                           with result */
#define AL_D3D12_EVENT_COUNT 5u
#define AL_D3D12_EVENT_BIT(event) (1u << (event))

/* The game's swap chain and the objects around it. Event data points to
 * AL's current copy, which changes when the game resizes or recreates. */
typedef struct al_d3d12_swapchain_t {
    uint32_t size;
    uint32_t reserved;
    void* swapchain; /* the IDXGISwapChain the game holds */
    void* queue;     /* the ID3D12CommandQueue the game created it with */
    void* device;    /* the ID3D12Device the game uses (as at DEVICE_CREATED) */
    void* window;    /* HWND: the window the game created the swap chain for */
    uint32_t width;
    uint32_t height;
    uint32_t format; /* DXGI_FORMAT */
    uint32_t buffer_count;
    uint32_t flags;  /* DXGI_SWAP_CHAIN_FLAG bits */
    uint32_t reserved2;
} al_d3d12_swapchain_t;

typedef struct al_d3d12_present_t {
    uint32_t size;
    uint32_t reserved;
    const al_d3d12_swapchain_t* swapchain;
    uint64_t frame;         /* presents before this one: 0 for the game's first */
    uint32_t back_buffer;   /* index of the buffer being presented */
    uint32_t sync_interval;
    uint32_t flags;         /* DXGI_PRESENT_* bits */
    int32_t result;         /* POST_PRESENT: the HRESULT Present returned; 0 before */
    int64_t present_qpc;    /* QueryPerformanceCounter when the game called Present */
    int64_t previous_qpc;   /* the same for the previous present; 0 for the first */
} al_d3d12_present_t;

typedef struct al_d3d12_resize_t {
    uint32_t size;
    uint32_t reserved;
    const al_d3d12_swapchain_t* swapchain; /* PRE: as it is; POST: as it is now */
    uint32_t buffer_count;  /* what the game asked for; 0 keeps the current count */
    uint32_t width;         /* 0: the window's client width */
    uint32_t height;
    uint32_t format;        /* DXGI_FORMAT; 0 keeps the current format */
    uint32_t flags;
    int32_t result;         /* POST_RESIZE: the HRESULT ResizeBuffers returned; 0 before */
} al_d3d12_resize_t;

typedef void(AL_CALL* al_d3d12_event_fn)(al_plugin_t* self, al_d3d12_event_t event, const void* data,
                                             void* user);

/* A method: a COM interface family and the vtable slot, counted from
 * QueryInterface = 0 as in the Windows SDK headers. */
typedef uint32_t al_d3d12_method_t;
#define AL_D3D12_METHOD(family, slot) ((al_d3d12_method_t)(((uint32_t)(family) << 16) | (uint32_t)(slot)))
#define AL_D3D12_METHOD_FAMILY(method) ((uint32_t)(method) >> 16)
#define AL_D3D12_METHOD_SLOT(method) ((uint32_t)(method) & 0xffffu)
#define AL_D3D12_FACTORY 1u      /* IDXGIFactory up to IDXGIFactory7; presentation role */
#define AL_D3D12_SWAPCHAIN 2u    /* IDXGISwapChain up to IDXGISwapChain4; presentation role */
#define AL_D3D12_DEVICE 3u       /* ID3D12Device up to the newest the system has */
#define AL_D3D12_QUEUE 4u        /* ID3D12CommandQueue */
#define AL_D3D12_COMMAND_LIST 5u /* ID3D12GraphicsCommandList up to the newest */
#define AL_D3D12_SLOT_LIMIT 256u /* slots are below this */

#define AL_D3D12_FACTORY_CREATE_SWAPCHAIN AL_D3D12_METHOD(AL_D3D12_FACTORY, 10)
#define AL_D3D12_FACTORY_CREATE_SWAPCHAIN_FOR_HWND AL_D3D12_METHOD(AL_D3D12_FACTORY, 15)
#define AL_D3D12_SWAPCHAIN_PRESENT AL_D3D12_METHOD(AL_D3D12_SWAPCHAIN, 8)
#define AL_D3D12_SWAPCHAIN_GET_BUFFER AL_D3D12_METHOD(AL_D3D12_SWAPCHAIN, 9)
#define AL_D3D12_SWAPCHAIN_SET_FULLSCREEN_STATE AL_D3D12_METHOD(AL_D3D12_SWAPCHAIN, 10)
#define AL_D3D12_SWAPCHAIN_RESIZE_BUFFERS AL_D3D12_METHOD(AL_D3D12_SWAPCHAIN, 13)
#define AL_D3D12_SWAPCHAIN_RESIZE_TARGET AL_D3D12_METHOD(AL_D3D12_SWAPCHAIN, 14)
#define AL_D3D12_SWAPCHAIN_PRESENT1 AL_D3D12_METHOD(AL_D3D12_SWAPCHAIN, 22)
#define AL_D3D12_SWAPCHAIN_GET_CURRENT_BACK_BUFFER_INDEX AL_D3D12_METHOD(AL_D3D12_SWAPCHAIN, 36)
#define AL_D3D12_SWAPCHAIN_RESIZE_BUFFERS1 AL_D3D12_METHOD(AL_D3D12_SWAPCHAIN, 39)
#define AL_D3D12_DEVICE_CREATE_COMMAND_QUEUE AL_D3D12_METHOD(AL_D3D12_DEVICE, 8)
#define AL_D3D12_QUEUE_EXECUTE_COMMAND_LISTS AL_D3D12_METHOD(AL_D3D12_QUEUE, 10)
#define AL_D3D12_QUEUE_SIGNAL AL_D3D12_METHOD(AL_D3D12_QUEUE, 14)

typedef struct al_d3d12_hook_t {
    al_d3d12_method_t method;
    int32_t order;  /* lower runs first; ties in load order */
    void* detour;   /* the plugin's function, with the method's exact signature */
    void** next;    /* where AL stores the function the detour continues with:
                       the next detour, or the original method */
} al_d3d12_hook_t;

typedef uint32_t al_d3d12_state_t;
#define AL_D3D12_WAITING 0u     /* registration open, or no hooked object seen yet */
#define AL_D3D12_RUNNING 1u     /* hooks are in place */
#define AL_D3D12_UNAVAILABLE 2u /* nothing will be called; reason says why */

typedef struct al_d3d12_status_t {
    uint32_t size;            /* set to sizeof(al_d3d12_status_t) before the call */
    al_d3d12_state_t state;
    const char* reason;       /* UNAVAILABLE: why; otherwise NULL */
    const char* owner;        /* the plugin holding the presentation role, or NULL */
    uint64_t frames;          /* presents seen on the game's present thread */
    int64_t last_present_qpc; /* 0 before the first */
} al_d3d12_status_t;

typedef struct al_d3d12_t {
    uint32_t size;
    uint32_t version; /* AL_D3D12_VERSION */
    /* Calls `callback` for each event in `events` (AL_D3D12_EVENT_BIT
     * mask). A plugin may subscribe more than once. AL_ERROR, with the
     * reason in the plugin's log, after registration closed, or for an empty
     * or unknown mask or no callback. */
    al_result_t(AL_CALL* subscribe)(al_plugin_t* self, uint32_t events, int32_t order,
                                        al_d3d12_event_fn callback, void* user);
    /* Registers `count` detours, all or none: AL_ERROR, with the reason in
     * the plugin's log, if any is invalid, if registration closed, or if a
     * FACTORY or SWAPCHAIN method is asked for without the presentation role.
     * The same plugin may not hook a method twice. *next stays NULL if AL
     * never hooks the method (DirectX 11, say). */
    al_result_t(AL_CALL* hook)(al_plugin_t* self, const al_d3d12_hook_t* hooks, uint32_t count);
    /* Takes the presentation role: the right to detour IDXGIFactory and
     * IDXGISwapChain methods, for a plugin that routes presentation, such as
     * frame generation. The first claim wins; a later one gets AL_ERROR
     * and its log names the holder. */
    al_result_t(AL_CALL* claim_presentation)(al_plugin_t* self);
    /* Marks the calling thread's following calls as internal (see above),
     * until the matching end_internal. Pairs nest. A thread-local counter:
     * cheap enough for every frame. */
    void(AL_CALL* begin_internal)(al_plugin_t* self);
    void(AL_CALL* end_internal)(al_plugin_t* self);
    /* Fills *status up to status->size. Any thread; the strings stay valid
     * until the process exits. */
    void(AL_CALL* status)(al_plugin_t* self, al_d3d12_status_t* status);
} al_d3d12_t;

/* "al.hooks" 1.0 (API 1.4): game functions, shared.
 *
 * AL puts one hook on each game function plugins ask for and calls their
 * detours in order, so plugins need no hooking library of their own and two
 * plugins on one function no longer fight. A detour has the function's own
 * signature and continues with *next, as with MinHook.
 *
 * Targets are functions in the game exe's code, checked by their bytes
 * before AL touches them: either `pattern` is searched in the game's code
 * and must match exactly once (target = match + offset), or `address` is
 * given and `pattern` must match at address - offset.
 *
 * Hooks may be registered at any time from al_plugin_init on. Those made
 * before PRE_MAIN ends take effect when it ends, before the game's own code
 * starts; later ones take effect at once. *next is written before the
 * detour can be called; it stays NULL while the hook is pending or failed.
 * A detour can join or leave a running chain safely: AL relinks the
 * chain, it never patches the function twice.
 *
 * Order: lower `order` runs first, further from the original; ties run in
 * load order. Detours run directly in the chain, like any hook: AL cannot
 * catch their faults. A plugin stays loaded, so a detour may still be
 * running on another thread just after it is disabled. */
#define AL_HOOKS_INTERFACE "al.hooks"
#define AL_HOOKS_VERSION AL_MAKE_VERSION(1, 0)

typedef struct al_hook_t {
    uint32_t size;       /* sizeof(al_hook_t) */
    int32_t order;       /* lower runs first; ties in load order */
    const char* name;    /* for logs and reports, e.g. "Quest:EnterQuest"; may be NULL */
    const char* pattern; /* the function's bytes in al.scanner's syntax, at least 5 */
    int32_t offset;      /* target = where the pattern matches + offset */
    uint32_t reserved;
    void* address;       /* the target, or NULL to search for pattern */
    void* detour;        /* the plugin's function, with the target's exact signature */
    void** next;         /* where AL stores the function to continue with */
} al_hook_t;

typedef uint32_t al_hook_state_t;
#define AL_HOOK_PENDING 0u  /* registered before PRE_MAIN ended; not active yet */
#define AL_HOOK_ACTIVE 1u
#define AL_HOOK_DISABLED 2u /* switched off with set_enabled; *next still valid */
#define AL_HOOK_FAILED 3u   /* will never be called; the plugin's log says why */

typedef struct al_hooks_t {
    uint32_t size;
    uint32_t version; /* AL_HOOKS_VERSION */
    /* Registers one detour and returns its handle, or 0 with the reason in
     * the plugin's log: a pattern that does not match exactly once, a target
     * outside the game's code, bytes that differ, no detour or next, the
     * same plugin hooking the same function twice, or (1.8) entry bytes a
     * plugin has changed through al.patch. Patterns are checked against the
     * game's original bytes where al.patch changed some (1.8). */
    uint32_t(AL_CALL* hook)(al_plugin_t* self, const al_hook_t* hook);
    /* Takes the detour out of its chain, or puts it back in its place.
     * Calls already inside it finish normally. */
    al_result_t(AL_CALL* set_enabled)(al_plugin_t* self, uint32_t handle, uint32_t enabled);
    /* The hook's state; AL_HOOK_FAILED for an unknown handle. */
    al_hook_state_t(AL_CALL* state)(al_plugin_t* self, uint32_t handle);
} al_hooks_t;

/* "al.frame" 1.0 (API 1.5): the game's frames and its game thread.
 *
 * Callbacks run on the game's frame thread (not the WinMain thread), once
 * per frame, before (BEGIN) and after (END) the game's frame update. Game
 * state is consistent there, so work that reads or changes game objects
 * belongs here or in a posted task, not on a plugin's own thread.
 *
 * At BEGIN, posted tasks run first, in posting order across plugins; then
 * BEGIN callbacks in load order. END callbacks run in load order. Callbacks
 * and tasks run under AL's fault guard: a fault disables the plugin, the
 * frame goes on. With nothing subscribed or queued, AL costs a few atomic
 * reads per frame and allocates nothing.
 *
 * When AL sees the game leave its frame loop, it runs the tasks still queued
 * on the game thread and the state becomes STOPPED. In rare cases (about 0.6%
 * of exits) it only learns that at SHUTDOWN; tasks still queued then are
 * dropped, and the plugin's log says how many.
 *
 * Only for game versions AL knows; elsewhere the state is UNAVAILABLE with
 * the reason, post fails, and subscribe still succeeds. */
#define AL_FRAME_INTERFACE "al.frame"
#define AL_FRAME_VERSION AL_MAKE_VERSION(1, 0)

typedef uint32_t al_frame_event_t;
#define AL_FRAME_BEGIN 0u /* before the game's frame update */
#define AL_FRAME_END 1u   /* after it */

typedef struct al_frame_info_t {
    uint32_t size; /* sizeof(al_frame_info_t) as AL was built */
    uint32_t reserved;
    uint64_t index; /* frames since the first one AL saw, from 0 */
    int64_t qpc;    /* QueryPerformanceCounter when this event began */
} al_frame_info_t;

/* `frame` is valid during the call only. */
typedef void(AL_CALL* al_frame_fn)(al_plugin_t* self, al_frame_event_t event, const al_frame_info_t* frame,
                                   void* user);
typedef void(AL_CALL* al_task_fn)(al_plugin_t* self, void* user);

typedef uint32_t al_frame_state_t;
#define AL_FRAME_WAITING 0u     /* no frame yet */
#define AL_FRAME_RUNNING 1u
#define AL_FRAME_STOPPED 2u     /* the game left its frame loop; no more frames */
#define AL_FRAME_UNAVAILABLE 3u /* AL cannot follow this game's frames; see reason */

typedef struct al_frame_status_t {
    uint32_t size; /* the caller sets sizeof(al_frame_status_t) */
    al_frame_state_t state;
    uint64_t frames;      /* frames begun so far */
    uint32_t game_thread; /* the frame thread's id; 0 before the first frame */
    uint32_t reserved;
    const char* reason; /* why UNAVAILABLE, else ""; valid until the process exits */
} al_frame_status_t;

typedef struct al_frame_t {
    uint32_t size;
    uint32_t version; /* AL_FRAME_VERSION */
    /* Calls `callback` at every `event` from the next frame on. Returns a
     * handle, or 0 with the reason in the plugin's log. Any thread, also
     * inside a callback. */
    uint32_t(AL_CALL* subscribe)(al_plugin_t* self, al_frame_event_t event, al_frame_fn callback, void* user);
    al_result_t(AL_CALL* unsubscribe)(al_plugin_t* self, uint32_t handle);
    /* Runs `task` once on the game thread at the next BEGIN. Any thread;
     * from the game thread it runs next frame. AL_ERROR once frames have
     * stopped or are unavailable. */
    al_result_t(AL_CALL* post)(al_plugin_t* self, al_task_fn task, void* user);
    /* Fills *status up to status->size. Any thread. */
    void(AL_CALL* status)(al_plugin_t* self, al_frame_status_t* status);
} al_frame_t;

/* "al.input" 1.0 (API 1.6): hotkeys, key and pad state, the game window's
 * messages, and input capture for menus.
 *
 * MHW updates its keyboard, mouse and pad state once per frame on its frame
 * thread (from DirectInput and XInput). AL hooks those game functions and
 * keeps a copy of what each produced, before capture changes it; hotkeys,
 * key_down and pad_state use those copies. AL never polls input itself,
 * and writes no input values or typed text to its logs. While the game
 * window is not in the foreground, queries return nothing held and hotkeys
 * do not fire.
 *
 * Request the interface from al_plugin_init or PRE_MAIN, so AL watches for
 * the game window and installs its input layer.
 *
 * Threads: hotkey callbacks run on the game's frame thread, at the end of
 * the frame, just before al.frame END callbacks. Message callbacks run on
 * the window's thread (the WinMain thread). Everything else may be called
 * from any thread. A callback that faults disables its plugin and releases
 * its capture; the game goes on.
 *
 * Capture: while any plugin holds capture of a device, the game receives
 * that device as idle (nothing held, mouse still, pad neutral); plugins
 * still see the real state. Mouse capture includes the mouse buttons the
 * game also keeps among its keys. Input still held when capture ends stays
 * hidden from the game until released, so it never arrives as a new press.
 * AL_INPUT_PAD covers XInput pads. Capture is counted per plugin: each
 * capture call is undone by one release, the latest first. Another
 * plugin's capture suppresses this plugin's hotkeys; its own do not. A
 * disabled plugin, focus loss and EXITING never leave the game captured. */
#define AL_INPUT_INTERFACE "al.input"
#define AL_INPUT_VERSION AL_MAKE_VERSION(1, 0)
#define AL_MOD_CTRL 1u
#define AL_MOD_SHIFT 2u
#define AL_MOD_ALT 4u
#define AL_INPUT_KEYBOARD 1u
#define AL_INPUT_MOUSE 2u
#define AL_INPUT_PAD 4u
#define AL_INPUT_ALL 7u

typedef struct al_hotkey_t {
    uint32_t size, key, modifiers, reserved; /* key is a Windows VK, including mouse buttons */
    const char* name;
} al_hotkey_t;
/* XInput button masks; all specified buttons must be held. */
#define AL_PAD_UP 0x0001u
#define AL_PAD_DOWN 0x0002u
#define AL_PAD_LEFT 0x0004u
#define AL_PAD_RIGHT 0x0008u
#define AL_PAD_START 0x0010u
#define AL_PAD_BACK 0x0020u
#define AL_PAD_LEFT_THUMB 0x0040u
#define AL_PAD_RIGHT_THUMB 0x0080u
#define AL_PAD_LEFT_SHOULDER 0x0100u
#define AL_PAD_RIGHT_SHOULDER 0x0200u
#define AL_PAD_A 0x1000u
#define AL_PAD_B 0x2000u
#define AL_PAD_X 0x4000u
#define AL_PAD_Y 0x8000u
typedef struct al_pad_hotkey_t {
    uint32_t size, user_index, buttons, modifiers; /* slot 0..3; modifiers are keyboard modifiers */
    const char* name;
} al_pad_hotkey_t;
typedef struct al_pad_state_t {
    uint32_t size, connected, packet;
    uint16_t buttons;
    uint8_t left_trigger, right_trigger;
    int16_t left_x, left_y, right_x, right_y;
} al_pad_state_t;
typedef void(AL_CALL* al_hotkey_fn)(al_plugin_t* self, uint32_t handle, void* user);
/* Non-zero consumes this message before later callbacks and the game. */
typedef uint32_t(AL_CALL* al_message_fn)(al_plugin_t* self, void* hwnd, uint32_t message,
                                      uintptr_t wparam, intptr_t lparam, void* user);
#define AL_INPUT_WAITING 0u     /* the input layer is not installed yet */
#define AL_INPUT_RUNNING 1u
#define AL_INPUT_UNAVAILABLE 2u /* AL could not install it; reason says why */
#define AL_INPUT_STOPPED 3u     /* the game is exiting (EXITING) */
typedef struct al_input_status_t {
    uint32_t size, state, devices, captured; /* devices: AL_INPUT_* bits AL can capture;
                                                captured: the bits captured now */
    void* window;       /* the game window (HWND), or NULL before it exists */
    const char* reason; /* why UNAVAILABLE, else ""; valid until the process exits */
} al_input_status_t;
typedef struct al_input_t {
    uint32_t size, version;
    /* Calls `callback` in the frame the combination becomes held: the key
     * plus every modifier given (others may be held too). Returns a handle,
     * or 0 with the reason in the plugin's log; reserved must be 0. A
     * combination another plugin uses too is allowed, and both plugins' logs
     * say so. */
    uint32_t(AL_CALL* add_hotkey)(al_plugin_t*, const al_hotkey_t*, al_hotkey_fn, void* user);
    al_result_t(AL_CALL* remove_hotkey)(al_plugin_t*, uint32_t handle);
    /* Non-zero if the key (a VK code, 0..255) was held in the game's latest
     * read. */
    uint32_t(AL_CALL* key_down)(al_plugin_t*, uint32_t key);
    /* Window messages, before the game sees them; lower `order` first. A
     * callback returning non-zero keeps the message from later callbacks and
     * the game; messages that destroy the window always reach it. One
     * callback per plugin; subscribing again replaces it. */
    al_result_t(AL_CALL* subscribe_messages)(al_plugin_t*, int32_t order, al_message_fn, void* user);
    /* Captures the AL_INPUT_* devices given (see above). AL_ERROR, with the
     * reason in the plugin's log, unless status.devices has every one. */
    al_result_t(AL_CALL* capture)(al_plugin_t*, uint32_t devices);
    /* Undoes this plugin's latest capture. */
    al_result_t(AL_CALL* release)(al_plugin_t*);
    /* The game window (HWND), or NULL before it exists. */
    void*(AL_CALL* window)(al_plugin_t*);
    /* The pad in slot 0..3 as the game's latest read returned it, before
     * capture. Fills *out up to out->size; AL_ERROR for another slot. */
    al_result_t(AL_CALL* pad_state)(al_plugin_t*, uint32_t user_index, al_pad_state_t* out);
    /* As add_hotkey, for pad buttons: every button in `buttons` held, plus
     * the keyboard modifiers given. */
    uint32_t(AL_CALL* add_pad_hotkey)(al_plugin_t*, const al_pad_hotkey_t*, al_hotkey_fn, void* user);
    al_result_t(AL_CALL* unsubscribe_messages)(al_plugin_t*);
    /* Fills *out up to out->size. */
    void(AL_CALL* status)(al_plugin_t*, al_input_status_t* out);
} al_input_t;

/* "al.patch" 1.0 (API 1.8): byte changes in the game, checked and owned.
 *
 * For changes that are not a hook at a function's entry: NOPs, a flipped
 * jump, a constant, a pointer in a table, a call sent elsewhere. AL checks
 * the bytes before it changes them, records which plugin owns them, and
 * refuses a change that overlaps another plugin's change or a hook (and
 * al.hooks refuses a hook over a change). Logs and crash reports list the
 * changes and their owners.
 *
 * Only the game exe: CODE and CALL in its executable sections, DATA in its
 * other sections (read-only or writable). Never its headers, its data
 * directories (imports, exports, exception data, relocations, resources,
 * TLS, load config, debug), the DRM stub's section, or another module.
 *
 * The bytes: `address`, or `pattern` searched as in al.hooks (exactly one
 * match, in the code for CODE and CALL, in the data sections for DATA;
 * target = match + offset). With both, the pattern must match at
 * address - offset. Either way the `length` bytes at the target must equal
 * `expected`, the final check. A plugin that gives only an address should
 * name its game exes in exe_sha256. Patterns are checked against the
 * game's original bytes where a hook or another change covers some.
 *
 * Changes made before PRE_MAIN ends are checked and claimed at once and
 * written when it ends, before the game's own code starts; later ones are
 * written before apply returns. Naturally aligned DATA of 1, 2, 4 or 8
 * bytes is written in one atomic step; everything else with the game's
 * other threads paused, never while one is inside the changed code. Change
 * whole instructions, and keep an instruction boundary where a call inside
 * the range returns: a thread inside the called function comes back there.
 * AL copies what it needs during apply; only *original is written to.
 *
 * AL_PATCH_CALL: the target holds E8 (call) or E9 (jmp) and a rel32, and
 * `expected` is those 5 bytes. AL points it at a stub within reach that
 * jumps to `function`; *original gets the old target first, so the plugin
 * can call through.
 *
 * When a plugin is disabled after a fault, its changes stay. At SHUTDOWN,
 * after the plugins' own SHUTDOWN, AL puts back DATA changes that are still
 * active; code and call changes stay, as hooks do. The game checks parts
 * of its own code; AL does nothing about that, and a change there is
 * visible to it, as with any mod. */
#define AL_PATCH_INTERFACE "al.patch"
#define AL_PATCH_VERSION AL_MAKE_VERSION(1, 0)

#define AL_PATCH_CODE 0u /* bytes in the game's code: NOPs, a flipped jump, any same-length change */
#define AL_PATCH_DATA 1u /* bytes in the game's data: a pointer, a constant, a table entry */
#define AL_PATCH_CALL 2u /* a call or jmp rel32 in the game's code, sent to `function` */

#define AL_PATCH_MAX_LENGTH 64u

typedef struct al_patch_t {
    uint32_t size;           /* sizeof(al_patch_t) */
    uint32_t kind;           /* AL_PATCH_CODE, AL_PATCH_DATA or AL_PATCH_CALL */
    const char* name;        /* for logs and reports, e.g. "Start menu: System tab items"; may be NULL */
    const char* pattern;     /* in al.scanner's syntax, or NULL with an address */
    int32_t offset;          /* target = where the pattern matches + offset */
    uint32_t length;         /* bytes changed: 1 to AL_PATCH_MAX_LENGTH; 5 for AL_PATCH_CALL */
    void* address;           /* the target, or NULL to search for pattern */
    const void* expected;    /* the `length` bytes that must be there now */
    const void* replacement; /* the `length` bytes to write; NULL for AL_PATCH_CALL */
    void* function;          /* AL_PATCH_CALL: where the call should go; else NULL */
    void** original;         /* AL_PATCH_CALL: where AL stores the old target; may be NULL */
} al_patch_t;

typedef uint32_t al_patch_state_t;
#define AL_PATCH_PENDING 0u  /* made before PRE_MAIN ended; written when it ends */
#define AL_PATCH_ACTIVE 1u   /* the replacement is in place */
#define AL_PATCH_DISABLED 2u /* switched off with set_enabled: the expected bytes are back */
#define AL_PATCH_FAILED 3u   /* AL will not touch it again; the plugin's log says why */

typedef struct al_patches_t {
    uint32_t size;
    uint32_t version; /* AL_PATCH_VERSION */
    /* Checks and claims one change and returns its handle, or 0 with the
     * reason in the plugin's log: a pattern that does not match once, bytes
     * that differ from `expected`, a place AL does not change, a length out
     * of range, a CALL target without E8/E9, or bytes another plugin's
     * change or hook holds (the log names it). Any thread, from
     * al_plugin_init on. */
    uint32_t(AL_CALL* apply)(al_plugin_t* self, const al_patch_t* patch);
    /* 0 puts the expected bytes back if the replacement is still there; 1
     * writes the replacement again if the expected bytes are still there. If
     * something outside AL changed the bytes since, AL leaves them and the
     * state becomes FAILED. Only the plugin that made the change. */
    al_result_t(AL_CALL* set_enabled)(al_plugin_t* self, uint32_t handle, uint32_t enabled);
    /* The change's state; AL_PATCH_FAILED for an unknown handle. */
    al_patch_state_t(AL_CALL* state)(al_plugin_t* self, uint32_t handle);
} al_patches_t;

typedef const al_plugin_info_t*(AL_CALL* al_plugin_info_fn)(void);
typedef al_result_t(AL_CALL* al_plugin_init_fn)(const al_api_t* api, const al_init_t* init);

#define AL_PLUGIN_INFO_EXPORT "al_plugin_info"
#define AL_PLUGIN_INIT_EXPORT "al_plugin_init"

#ifdef __cplusplus
}
#endif

#endif /* AL_PLUGIN_H */
