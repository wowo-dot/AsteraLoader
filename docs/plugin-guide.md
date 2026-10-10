# Writing an Astera Loader plugin

For plugin API 1.7. The header `include/al/al_plugin.h` is the reference;
this guide explains how its parts fit together. The smallest working plugin
is `examples/hello/hello.c`.

## What a plugin is

A plugin is one 64-bit DLL in its own folder:

```
<game>\AsteraLoader\plugins\<name>\<name>.dll
```

AL loads only `<name>.dll`. Anything else in that folder (the plugin's own
DLLs, config, data) belongs to the plugin. Under MO2 a plugin is its own mod
with that layout; AL sees it through MO2's virtual game folder.

Names are 1 to 64 characters from `a-z`, `0-9`, `_`, `-` and `.`, and do
not start with `.`. The name is also the folder name and the DLL name.

Stracker's Loader plugins (`nativePC\plugins\*.dll`) keep working unchanged:
AL loads them itself. New plugins should use AL's API instead.

## The smallest plugin

A plugin exports two functions, `al_plugin_info` and `al_plugin_init`:

```c
#include "al/al_plugin.h"

static const al_api_t* g_api;
static al_plugin_t* g_self;

static const al_plugin_info_t info = {
    sizeof(al_plugin_info_t),
    AL_API_VERSION,
    "my_plugin",    /* name: the folder and DLL name */
    "My Plugin",    /* display name, or NULL */
    "1.0.0",        /* this plugin's version */
    0,              /* phases: none */
    0,              /* order */
    NULL, NULL,     /* load_after, load_before */
    NULL,           /* exe_sha256: any game exe */
    NULL,           /* on_phase */
};

AL_PLUGIN_EXPORT const al_plugin_info_t* AL_CALL al_plugin_info(void) {
    return &info;
}

AL_PLUGIN_EXPORT al_result_t AL_CALL al_plugin_init(const al_api_t* api, const al_init_t* init) {
    g_api = api;
    g_self = init->self;
    g_api->log(g_self, AL_LOG_INFO, "hello");
    return AL_OK;
}
```

Build it as an x64 DLL with the `include` folder on the include path. The
plugin may be written in C or C++; only plain C types cross the boundary.
AL needs no import library: it finds the two exports by name.

## Start-up

AL starts plugins at the PRE_MAIN moment: the game's code is unpacked, and
its C runtime has not started yet. On the game's main thread, in this
order:

1. AL loads every plugin DLL and calls `al_plugin_info`.
2. AL refuses plugins it cannot run, each with a reason the user can see:
   - built for another major API version, or a newer minor one than this AL;
   - `exe_sha256` given and the game exe not on the list;
   - name not matching the folder;
   - a reserved name: `al` or `al.<anything>` (AL's own service names), or
     a DLL name of Windows, the game or another loader (`dinput8`,
     `loader`, `dxgi` and the like);
   - a required plugin (`dependencies`) missing, too old, or failing to start;
   - a cycle in `load_after` / `load_before`.

   A refused plugin is unloaded again. So keep `DllMain` and
   `al_plugin_info` free of work: no threads, no hooks, no file access.
3. AL calls `al_plugin_init` in load order: required plugins and the
   `load_after` / `load_before` names first, then `order` (lower first),
   then the name.
4. AL dispatches PRE_MAIN to plugins that asked for it, in the same order.

`al_plugin_init` returns `AL_OK`, or `AL_ERROR` after calling `set_status`
with the reason. Once `al_plugin_init` has been called, AL never unloads
the plugin.

What a plugin gets in `al_init_t`:

| Field | What it is |
|---|---|
| `self` | Pass it to every API call. |
| `folder` | The plugin folder as the game sees it (under MO2, the virtual game-folder path). |
| `folder_real` | The same folder resolved through a file handle (under MO2, the real mod folder). |
| `data_folder` | A writable folder for the plugin's own data, `AsteraLoader\data\<name>`; NULL if it could not be created. |

Under MO2 the two folder paths differ. Never compare a path from one with a
path from the other; pick one and derive everything from it.

## The info struct

| Field | Notes |
|---|---|
| `size`, `api_version` | `sizeof(al_plugin_info_t)` and `AL_API_VERSION`. |
| `name`, `display_name`, `version` | `version` is dotted numbers ("1.2.0"); other plugins' `min_version` compares against it. |
| `phases`, `on_phase` | An `AL_PHASE_BIT` mask, and the callback for them. |
| `order` | Lower starts earlier; 0 is the default. |
| `load_after`, `load_before` | NULL-terminated name lists. Plugins named here need not be installed. |
| `exe_sha256` | NULL-terminated lower-case SHA-256 hex strings of supported game exes, or NULL for any. |
| `dependencies` | Plugins that must be installed, new enough, and started first; the list ends with a NULL name. |

The struct and everything it points to must stay valid while the plugin is
loaded: use static data.

## Phases

Each phase fires at most once, in this order. `data` is valid only during
the `on_phase` call; every payload starts with `size`, so check it before
reading a field a later version added.

| Phase | When | Thread | Payload |
|---|---|---|---|
| PRE_MAIN | Game code unpacked, before its C runtime starts. Right after `al_plugin_init`. | main | none |
| WIN_MAIN | The C runtime is ready; WinMain is about to run. | main | `al_phase_win_main_t` |
| DEVICE_CREATED | The game's own `D3D12CreateDevice` returned, before the game uses the device. Not under DirectX 11. | the game's render setup thread | `al_phase_device_t` |
| SWAPCHAIN_CREATED | The game created its first swap chain. | the creating thread | `al_d3d12_swapchain_t` |
| FIRST_PRESENT | The game's first Present, before PRE_PRESENT callbacks. | the present thread | `al_d3d12_present_t` |
| GAME_READY | 1.7: the game finished booting and its title screen started; the game's data and systems are set up. Once, at the end of that frame, after al.frame END. Not on executables AL does not know. | the game's frame thread | `al_phase_game_ready_t` |
| EXITING | The game started its own quit (Exit Game, or a window close), before it tears anything down. **Stop your threads here.** | usually the WinMain thread | none |
| SHUTDOWN | WinMain returned, before the C runtime tears down. Last chance to save. | main | `al_phase_shutdown_t` |

Neither EXITING nor SHUTDOWN fires when the process is ended from outside
(Task Manager) or by a crash. Save settings when they change, not only at
exit.

Plugin threads that keep running past EXITING can touch game objects that
are being freed. Several Stracker plugins crash that way at every exit.

## Logs, status and what the user sees

- `api->log(self, level, message)` writes one line to
  `AsteraLoader\logs\plugins\<name>.log`. AL's own log is
  `AsteraLoader\logs\core.<pid>.log`. Both sit in AL's own folder (under
  MO2, AL's mod folder).
- `api->set_status(self, status, reason)` sets what the user sees for the
  plugin: ACTIVE, DEGRADED (running with features off) or DISABLED (not
  running). Give a reason for the last two. When any plugin is refused or
  not fully active, AL shows the user a warning at start-up that points to
  the logs; the CMFM menu will show each plugin's status.
- API calls that fail (`AL_ERROR`, a 0 handle) write the reason into the
  plugin's log.
- If the game crashes, AL writes a report to `AsteraLoader\logs\crashes\`
  naming the plugin whose code faulted (or that called the code which
  faulted), and tells the user at the next launch.

Fail visibly and keep the game running: if a feature cannot start (a pattern
not found, an unsupported game version), log why, set DEGRADED or DISABLED
with a short reason, and return. A plugin failing must never take the game
down.

## Services

`api->get_interface(self, name, version)` returns a service table, or NULL
if there is none with the same major version and at least the requested
minor. Tables stay valid until the process exits, and their functions may
be called from any thread unless the header says otherwise.

```c
const al_frame_t* frame = (const al_frame_t*)api->get_interface(init->self, AL_FRAME_INTERFACE, AL_FRAME_VERSION);
if (!frame) { /* this AL has no al.frame 1.0 or newer: run without it */ }
```

| Service | Since | For |
|---|---|---|
| `al.game` | 1.1 | Which game this is: exe hash, MHW build string, paths, DirectX 11 or 12. |
| `al.scanner` | 1.1 | Find byte patterns in the game exe; results cached per exe. |
| `al.d3d12` | 1.3 | The game's DXGI and Direct3D 12 calls, shared between plugins. |
| `al.hooks` | 1.4 | Detours on functions in the game's own code, shared between plugins. |
| `al.frame` | 1.5 | Callbacks on the game thread every frame; tasks posted to it. |
| `al.input` | 1.6 | Hotkeys, key and pad state, the game window's messages, input capture for menus. |

### al.game

A struct of facts, read once: `exe_sha256`, `game_version` (MHW's build
string, such as "421810"), `exe_path`, `game_folder`, `pe_timestamp`, and
`graphics` (what `graphics_option.ini` selects at start-up).

### al.scanner

`find(self, pattern, where, matches, capacity)` searches the game exe.
Patterns are hex bytes with `?` or `??` for any byte (`"48 8B ?? 05"`), or
Stracker's bit notation. It returns the number of matches and writes the
first `capacity` of them. `rip_target` resolves a RIP-relative operand.
Results are cached per exe hash, so later launches skip the search.

### al.frame: the game thread

MHW runs its game update on its own frame thread, not on the WinMain
thread. Game state is consistent there between frames, so work that reads
or changes game objects belongs on that thread:

- `subscribe(self, AL_FRAME_BEGIN or AL_FRAME_END, callback, user)` calls
  the callback once per frame, before or after the game's update. It
  returns a handle for `unsubscribe`.
- `post(self, task, user)` runs `task` once on the game thread at the next
  BEGIN. Call it from any thread: this is how a plugin's own thread, a
  window message or a network reply hands work to the game thread.
- `status` tells whether frames are WAITING, RUNNING, STOPPED (the game
  left its frame loop) or UNAVAILABLE (a game version AL does not know;
  `reason` says why).

```c
static void AL_CALL apply_setting(al_plugin_t* self, void* user) {
    /* game thread: safe to touch game objects */
}

/* on the plugin's own thread: */
if (g_frame->post(g_self, apply_setting, NULL) != AL_OK) {
    /* frames stopped or unavailable */
}
```

Callbacks and tasks run under AL's fault guard: one that faults disables
its plugin, and the game goes on. They run every frame, so keep them short
and do not allocate in them.

### al.hooks: game functions

Instead of bringing a hooking library, a plugin asks AL for a detour on a
game function. AL puts one hook on each function and calls every plugin's
detour in order, so two plugins on one function do not fight.

```c
typedef void(__fastcall* update_fn)(void* self, float delta);
static update_fn g_update_next;

static void __fastcall on_update(void* self, float delta) {
    /* before the game's function */
    g_update_next(self, delta);
    /* after it */
}

al_hook_t hook = {sizeof(al_hook_t)};
hook.name = "Example:Update";
hook.pattern = "48 89 5C 24 ?? 57 48 83 EC 30"; /* illustrative bytes */
hook.offset = 0;
hook.detour = (void*)on_update;
hook.next = (void**)&g_update_next;
const uint32_t handle = hooks->hook(self, &hook);
if (!handle) { /* the log says why; set DEGRADED */ }
```

- AL checks the target's bytes before it touches anything. Either the
  pattern is searched and must match exactly once (target = match +
  offset), or `address` is given and the pattern must match there.
- Register from `al_plugin_init` on. Hooks registered before PRE_MAIN ends
  take effect when it ends, before the game's code runs; later ones at once.
- `*next` is written before the detour can be called. Always continue
  through it, never through a saved address of the original.
- `set_enabled` takes a detour out of its chain and puts it back. `order`
  decides the position: lower runs first, further from the original.
- Detours run directly in the chain, so AL cannot catch their faults.

### al.d3d12: Direct3D 12

Two ways in:

- **Events** (`subscribe`): swap chain created, before and after Present,
  before and after ResizeBuffers. Enough for overlays, menus and counters.
  Draw overlays at PRE_PRESENT; release every reference to the swap chain's
  buffers at PRE_RESIZE and take new ones at POST_RESIZE.
- **Detours** (`hook`): a detour with the method's own signature on a DXGI
  or D3D12 method, named by interface family and vtable slot
  (`AL_D3D12_SWAPCHAIN_PRESENT` and the like). Detours on IDXGIFactory and
  IDXGISwapChain methods need the presentation role (`claim_presentation`).
  Only one plugin holds it: the one that routes presentation, such as frame
  generation.

Registration closes when PRE_MAIN ends, so subscribe and hook from
`al_plugin_init` or PRE_MAIN. AL then hooks only the methods plugins asked
for. Under DirectX 11 nothing is called, and `status` says why.

A detour that passes a call into code which itself calls DXGI or D3D12
(Streamline's proxies, say) wraps that call in `begin_internal` /
`end_internal`, so every plugin sees each game call once.

### al.input: hotkeys, key state and menus

MHW updates its keyboard, mouse and pad state once per frame on its frame
thread. AL hooks those game functions and keeps a copy of what each
produced, and everything below works from those copies: a plugin sees what
the game saw, and nothing held while the game window is in the background.
AL never polls input itself, and logs no keys or text.

- **Hotkeys:** `add_hotkey` takes a key (a Windows `VK_` code, mouse
  buttons included) and `AL_MOD_*` modifiers; `add_pad_hotkey` takes
  `AL_PAD_*` buttons on a pad slot (0 to 3). The callback runs on the game
  thread in the frame the combination becomes held. Two plugins may use the
  same combination; both their logs say so.
- **State:** `key_down` and `pad_state`, from any thread.
- **Window messages:** `subscribe_messages` delivers the game window's
  messages on the window's thread, before the game; a callback returning
  non-zero keeps the message from the game. That is how a menu's UI gets
  mouse moves, clicks and typed characters (`WM_CHAR`). Keeping keyboard
  messages from the game does not stop the hunter: the game reads its
  controls elsewhere. That is what capture is for.
- **Capture:** `capture(self, AL_INPUT_KEYBOARD | AL_INPUT_MOUSE)` makes
  the game see those devices idle while plugins keep the real state;
  `release` undoes the latest capture. A menu captures when it opens and
  releases when it closes. Capture fails until `status` shows the device in
  `devices`. Another plugin's capture suppresses your hotkeys, your own
  does not. AL never leaves the game captured: a plugin that faults or is
  disabled, the game losing focus, and EXITING all end capture. Keys and
  buttons still held when capture ends stay hidden from the game until
  released, so closing a menu with Esc does not also reach the game.
  `AL_INPUT_PAD` covers XInput pads.

Request `al.input` from `al_plugin_init` or PRE_MAIN, so AL installs its
input layer and watches for the game window.

```c
static const al_input_t* g_input;
static int g_menu_open; /* touched on the game thread only */

static void AL_CALL toggle_menu(al_plugin_t* self, uint32_t handle, void* user) {
    if (!g_menu_open) {
        g_menu_open = g_input->capture(self, AL_INPUT_KEYBOARD | AL_INPUT_MOUSE) == AL_OK;
    } else {
        g_input->release(self);
        g_menu_open = 0;
    }
}

/* in al_plugin_init: */
g_input = (const al_input_t*)api->get_interface(init->self, AL_INPUT_INTERFACE, AL_INPUT_VERSION);
if (g_input) {
    const al_hotkey_t key = {sizeof(al_hotkey_t), VK_F1, AL_MOD_CTRL, 0, "Open menu"};
    if (!g_input->add_hotkey(init->self, &key, toggle_menu, NULL)) { /* the log says why */ }
}
```

## Threads

| Code | Thread |
|---|---|
| `al_plugin_init`, PRE_MAIN, WIN_MAIN, SHUTDOWN | the game's main thread |
| EXITING | usually the WinMain thread |
| al.frame callbacks and tasks | the game's frame thread |
| al.input hotkey callbacks | the game's frame thread, at the end of the frame |
| al.input message callbacks | the window's thread (the WinMain thread) |
| al.d3d12 Present events | the game's present thread |
| al.d3d12 detours, al.hooks detours | whatever thread calls the function |
| The API's functions | callable from any thread |

## Rules

- Strings are UTF-8 and NUL-terminated; paths are UTF-16 (`wchar_t`).
- No C++ exceptions cross the boundary. Catch them inside the plugin.
- Set `size` to `sizeof` in every struct you pass to AL. In structs AL
  passes to you, check `size` before reading a field added after the
  version you built for (see `hello.c`).
- Any result other than `AL_OK` is a failure (test `!= AL_OK`, not
  `== AL_ERROR`); a handle of 0 is a failure.
- Do not patch the game exe or its files on disk. Use `al.hooks` and
  `al.d3d12` rather than hooking functions yourself. Never stack a second
  inline patch on a function AL or another plugin hooks.
- Do not put proxy DLLs (`dinput8.dll`, `dxgi.dll`, `winmm.dll` and the
  like) in the game folder. AL needs none.
- Nothing per frame that allocates, blocks or logs. Count, and log totals
  at exit.
- Do not touch game objects from the plugin's own threads; post to the game
  thread.
- Stop your threads at EXITING.

## Versions

API versions are `major << 16 | minor`. Within a major version every
change is additive: new fields at the end of structs, so `size` grows. AL
refuses a plugin built for another major version, or for a newer minor
version than it provides; a plugin built for an older minor version keeps
working. Service tables follow the same rule, each with its own version.

A plugin can publish its own service for other plugins with
`publish_interface`. The name starts with the plugin's name and a dot
("mhw_dlss.status"), and the table starts with `uint32_t size, version`.

## Coming next

API 1.x is what v1 ships. After v1: game events (quests, areas, loading
screens) in a separate MHW SDK, and CMFM, the settings menu. The full plan
is in `api-roadmap.md`.
