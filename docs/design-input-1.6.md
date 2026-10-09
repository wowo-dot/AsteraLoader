# Design: input, hotkeys and input capture (API 1.6, draft)

Status 2026-10-09: research and draft API. Roadmap "path to v1" step 2.
The hook side is built after API 1.5's frame layer lands; one probe run
settles the open questions below first.

## Who needs it

- **CMFM (the menu plugin):** a key to open it; mouse and keyboard for its
  widgets (ImGui), including typed text; the game must not react to input
  while the menu is open.
- **Plugins with hotkeys:** FextyMod, LuaEngine, SPL keybinds (plugin
  survey #8). Today each polls on its own thread or reads game memory.

## What the game uses (known so far)

- **Loaded input DLLs** (watcher, crash-mo2-2): DINPUT8 (System32), HID and
  inputhost at about 8.0 s, then XINPUT9_1_0 with XInput1_4. IMM32 and the
  text input framework load early (typing, chat).
- **The game's own input objects** (SharpPluginLoader 2.0, `Core/IO/Input.cs`,
  MIT; read-only use there):
  - `sMhKeyboard`: a virtual-key table at +0x38; at +0x138 six 256-bit
    maps (held, previous, pressed, released, changed, repeat) and repeat
    timers. The game does its own edge detection.
  - `sMhSteamController`: pad button bits at +0x198 (held), +0x19C
    (previous), +0x1A0 (pressed), +0x1A4 (released), +0x1A8 (changed). Pads
    go through Steam Input, so XInput alone may not see what the game sees.
- **Other frameworks:** SPL subclasses the game window
  (`SetWindowLongPtr(GWLP_WNDPROC)`), feeds ImGui, and always passes every
  message on; it finds the window from the swap chain's description, or by
  its title. Neither SPL nor any surveyed mod blocks game input while a
  menu is open. Better Input Detection patches the game's own input code
  (it imports only `VirtualProtect`).

## Found in frame-input-1 (input_probe 0.1.0)

- **Keyboard and mouse do not come from the game window's messages.**
  Keeping 29 keyboard messages from the game for 4.5 s changed nothing in
  play.
- **They come through DirectInput 8's raw-input path.** The process
  registers raw input for the mouse (usage page 1, usage 2) and the
  keyboard (usage 6) with `RIDEV_INPUTSINK`, aimed at a hidden window, not
  the game window ("MT FRAMEWORK"). That is how DirectInput 8 reads
  devices on current Windows. Capture must block at DirectInput (or at the
  game's own input objects), not at the game window.
- **The game window still gets what ImGui needs:** mouse moves (9922),
  clicks, `WM_CHAR` (43) and `WM_SETCURSOR`. Its procedure is in the exe and
  runs on the WinMain thread, so subclassing it serves CMFM's widgets.
- **Exit Game:** `WM_CLOSE`, then the window is destroyed, then EXITING
  from the exit site about 150 ms later.
- F10 never toggled: it is a system key (`WM_SYSKEYDOWN`). A probe bug.

## Found in input-probe-3 (input_probe 0.2.1)

- The game reads two DirectInput devices, "Keyboard" (`GetDeviceState`,
  256 bytes) and "Mouse" (`GetDeviceState`, 20 bytes: `DIMOUSESTATE2`),
  once per frame each, and never the buffered `GetDeviceData`.
- It calls `XInputGetState` for all four pad slots every frame.
- All of it on the game's frame thread (the al.frame game thread).
- **Blocking there works:** zeroing the keyboard's and the mouse's
  `GetDeviceState` data froze the hunter and the camera, and a neutral
  `XInputGetState` froze the pad; all recovered when unblocked.
- The keyboard's and the mouse's device classes have separate method
  implementations, so each needs its own hook.

## Open questions (one probe run)

Questions 1 and 3 are answered above (DirectInput; raw input through
DirectInput's own window). Question 5 too, by the user: **Alt+F4 does
nothing in MHW.** In frame-input-1 launch 2 (and earlier in astera-mo2-2)
the user pressed it, saw no reaction, and ended the process from outside;
the probe logged no `WM_SYSKEYDOWN F4` before that. So there is no Alt+F4
path to warn plugins about. A process ended from outside or by a crash
gets no notice, and plugins save settings when they change. Still open: 2
(devices and methods) and 4 (pads).

1. Where `sMhKeyboard`'s maps come from: DirectInput's keyboard device
   (`GetDeviceState` / `GetDeviceData`), window messages, or Win32 key
   state. This decides where capture must block.
2. Which DirectInput devices the game creates (keyboard, mouse, joysticks),
   their cooperative level and data format, and on which thread they are
   read.
3. Whether the game uses raw input (`WM_INPUT`) or cursor messages for the
   mouse, in menus and in play.
4. How pads reach `sMhSteamController`: Steam Input only, or XInput as
   well; what happens with Steam Input off.
5. What Alt+F4 does (answered: nothing).

## The API: "al.input" 1.0 (draft)

```c
#define AL_INPUT_INTERFACE "al.input"
#define AL_INPUT_VERSION AL_MAKE_VERSION(1, 0)

/* A key: a Windows virtual-key code (VK_*), mouse buttons included. */
typedef struct al_hotkey_t {
    uint32_t size;       /* sizeof(al_hotkey_t) */
    uint32_t key;        /* VK_F1, 'M', VK_XBUTTON1, ... */
    uint32_t modifiers;  /* AL_MOD_CTRL | AL_MOD_SHIFT | AL_MOD_ALT, all required */
    uint32_t reserved;
    const char* name;    /* for the status page and conflict reports */
} al_hotkey_t;

typedef void(AL_CALL* al_hotkey_fn)(al_plugin_t* self, uint32_t handle, void* user);

/* A window message; return non-zero to keep it from the game (and from
 * later callbacks). Runs on the window's thread (the WinMain thread). */
typedef uint32_t(AL_CALL* al_message_fn)(al_plugin_t* self, void* hwnd, uint32_t message,
                                         uintptr_t wparam, intptr_t lparam, void* user);

typedef struct al_input_t {
    uint32_t size, version;
    /* Calls `callback` on the game thread in the frame the combination is
     * pressed, while the game window has focus and no other plugin holds
     * capture. A combination two plugins use is allowed and reported. */
    uint32_t(AL_CALL* add_hotkey)(al_plugin_t* self, const al_hotkey_t* hotkey, al_hotkey_fn callback, void* user);
    al_result_t(AL_CALL* remove_hotkey)(al_plugin_t* self, uint32_t handle);
    /* Whether the key is held now (any thread). */
    uint32_t(AL_CALL* key_down)(al_plugin_t* self, uint32_t key);
    /* Window messages, in `order` (lower first), before the game sees them. */
    al_result_t(AL_CALL* subscribe_messages)(al_plugin_t* self, int32_t order, al_message_fn callback, void* user);
    /* While held, the game gets no keyboard, mouse or pad input (the flags
     * pick which); message callbacks still do. Counted per plugin. */
    al_result_t(AL_CALL* capture)(al_plugin_t* self, uint32_t devices);
    al_result_t(AL_CALL* release)(al_plugin_t* self);
    /* The game window, once it exists (NULL before). */
    void*(AL_CALL* window)(al_plugin_t* self);
} al_input_t;
```

input-probe-3 showed pads reach the game through `XInputGetState` (with a
controller connected and Steam running), so a pad state query and pad
button hotkeys can be part of 1.0; the API above gets them before it is
built. Steam Input on/off is still to be checked.

## How it works (core side)

- **The window** comes from the swap chain the game creates (al.d3d12
  already sees it), checked against the process's top-level windows.
- **Window messages:** AL subclasses the game window, as SPL does
  (`SetWindowLongPtr`), at the first swap chain. Message callbacks run in
  order; a callback that returns non-zero keeps the message from the game.
  If another module subclasses the window later, AL stays in the chain
  behind it; AL never restores the old procedure while plugins are loaded.
- **The input layer** (settled by input-probe-3): AL hooks the methods the
  game reads its input through, once DINPUT8 and XInput are loaded:
  DirectInput's `GetDeviceState` for the keyboard's and the mouse's device
  classes (each has its own implementation; `GetDeviceData` too, though
  the game does not use it), and `XInputGetState`. The method addresses
  come from throwaway devices of AL's own, as al.d3d12 does for DXGI.
- **Snapshots, not polling:** each time the game reads a device, the layer
  keeps a copy of the real data (keyboard 256 bytes, mouse
  `DIMOUSESTATE2`, four pads) before capture changes it. The game reads
  everything once per frame on its frame thread, inside Main:Update, so
  the snapshots are fresh at al.frame's END. Hotkeys, `key_down` and pad
  queries use them: no extra system calls, the same data the game sees,
  and DirectInput's own focus rules (no data while the game is in the
  background).
- **Hotkeys** are edge-detected per frame from the snapshots and called at
  al.frame END, on the game thread.
- **Capture** changes what the game receives: zeroed keyboard and mouse
  state from `GetDeviceState`, no events from `GetDeviceData`, and a
  neutral pad from `XInputGetState`. Window messages are not involved: the
  game does not read input from them (frame-input-1). Plugins still see
  the real state through the snapshots and their message callbacks.
- **Cost:** a flag check and one copy (256, 20 or 16 bytes) per game read,
  six reads per frame; with no message callbacks the window procedure
  adds one call and a branch per message.

## Tests

- Pure: hotkey edge detection and modifiers, conflicts, capture counting,
  message callback order and consumption.
- Fake game: a window with its own procedure, a fake DirectInput device and
  XInput stand-in; capture on/off and what the game sees.
- In game: the probe run first, then one run with a test plugin (a hotkey,
  capture toggled by it, a message log).
