# Design: input, hotkeys and input capture (API 1.6)

Status 2026-10-10: API, input service and native hook layer implemented.
The probe research below supplied the input sites; validation of the new
layer in MHW remains pending. See `handoff-input-20261010.md` for the
implemented rules, tests and next run. Roadmap "path to v1" step 2.

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
  its title. ~~Neither SPL nor any surveyed mod blocks game input while a
  menu is open.~~ Corrected 2026-10-10: SPL does (`Rendering/Renderer.cs`,
  7be2393). While its ImGui wants the keyboard, it hooks the game's
  `Keyboard:WriteInput` and clears the keyboard state after it; while an
  ImGui window has focus, it hooks the mouse update and zeroes the
  camera delta, clicks and wheel in `sMhMouse`. Better Input Detection
  patches the game's own input code (it imports only `VirtualProtect`).

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
- ~~The keyboard's and the mouse's device classes have separate method
  implementations, so each needs its own hook.~~ Corrected by
  input-check-1: those methods were the Steam overlay's wrappers (see
  below). The probe had hooked the overlay's functions.

## Found in input-check-1: the Steam overlay wraps DirectInput

- With the Steam overlay on, the device methods (`GetDeviceState`,
  `GetDeviceData`) of a keyboard or mouse created in the game process
  point into `gameoverlayrenderer64.dll`, not System32's `dinput8.dll`.
  In input-probe-2 and -3 the keyboard's `GetDeviceState` sat 0x1850 bytes
  below the overlay's D3D12 patch destination both times, wherever Windows
  had loaded the overlay.
- So the game calls the overlay's functions, which pass the reads on to
  Windows. AL's native layer refuses methods outside System32, as
  designed, and al.input reported UNAVAILABLE with a warning.
- Still unknown: whether the overlay changes `dinput8.dll`'s own method
  tables or hands out wrapper objects, and whether it also patches
  `XInputGetState`. With the overlay off, the methods should be System32's.

## Found by static analysis: the game's own input sites

2026-10-10, IDA on a private copy of the decrypted exe (421810, SHA-256
c2ebbbd2...), `local/ida/input-20261010/`. RVAs from the image base. The
game imports DirectInput and XInput delay-loaded (`DINPUT8`
`DirectInput8Create`; `XINPUT9_1_0` `XInputGetState`, `XInputSetState`,
`XInputGetCapabilities`), which is why DINPUT8 loads at about 8 s.

- **Keyboard update, `exe+0x22e74f0`**, `void(sMhKeyboard* kb)`, slot 28
  of the keyboard's vtable (`exe+0x2f86c58`), called once per frame. The
  state is at `kb+0x138`, eight dwords per map, indexed by virtual-key
  code: held (`+0x138`), previous (`+0x158`), pressed (`+0x178`), released
  (`+0x198`), changed (`+0x1B8`), repeat (`+0x1D8`), then per-key repeat
  timers (`+0x1F8`). It moves held to previous, clears held, calls
  `Keyboard:WriteInput` to fill held, then computes the other maps.
  Its prologue is common; the shortest unique pattern (sigscan `make` on
  the decoded image) is its first 35 bytes.
- **`Keyboard:WriteInput`, `exe+0x22e7ad0`**, `(kb, held_map)`: SPL's site.
  Reads DirectInput (device at `kb+0xA10`, `GetDeviceState(256)`) into a
  local buffer, maps each scan code to a virtual key through the table at
  `kb+0x38`, ORs the bits into the held map, adds the five mouse buttons
  (VK 1, 2, 4, 5, 6) from `GetAsyncKeyState`, and derives Shift, Ctrl and
  Alt from their left and right keys. If the window is not in the
  foreground it clears the map. Without a DirectInput device it falls back
  to `GetAsyncKeyState` for all 256 keys.
- **Mouse read, `exe+0x22e9270`**, `(mouse_device* m, mouse_state* out)`,
  called from `exe+0x22e89a0`, called from the mouse update `exe+0x489800`
  (slot 28 of `exe+0x2f86d98`). Reads `GetDeviceState(20)`
  (`DIMOUSESTATE2`, device at `m+0xD0`) and writes `out`: cursor position
  scaled (`+0`, `+4`) and in the window (`+8`, `+12`), movement and wheel
  deltas scaled by the sensitivity at `m+0xBC` (`+20`, `+24`, `+28`), and
  the buttons as bits 0 to 7 (`+32`). If the window is not in the
  foreground it clears the buttons. Unique pattern from its first byte.
- **XInput pads, `exe+0x22a59d0`**, `(pad_slot*)`, per slot and frame:
  `XInputGetState(slot+0x14 user index)`, then
  **`exe+0x22a1280(manager, slot, const XINPUT_STATE*)`**, which copies the
  sticks, triggers and buttons into the slot. Unique pattern from its first
  byte.
- **DirectInput pads**: the same slot function reads `GetDeviceState(272)`
  (`DIJOYSTATE2`) into `slot+0x1A0` and processes it in `exe+0x22a0880`.
  The game supports non-XInput pads too.
- All of these run on the frame thread inside Main:Update (input-probe-3).

### Proposal: capture at the game's own input sites

Hook the game's functions through al.hooks instead of DirectInput's and
XInput's: they are reached the same way with the Steam overlay on or off,
or anything else wrapping DirectInput (ReShade, SPL), and they are checked
by their bytes like the frame layer's sites.

Patterns: the shortest unique ones from `sigscan.py make` on the decoded
image (d78611...), each from the function's first byte:

- keyboard update `0x22e74f0`: `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 33 FF 48 8D B1 38 01 00 00`
- mouse read `0x22e9270`: `40 55 53 56 57 41 56 48 8B EC 48 81 EC 80 00 00 00`
- pad `0x22a1280`: `48 89 5C 24 08 57 44 8B 9A 60 01 00 00`

| Device | Site | Snapshot (after the original) | Capture |
|---|---|---|---|
| keyboard | update `0x22e74f0` | the held map: 256 bits by virtual key, mouse buttons included | clear held, pressed, released, changed and repeat after the original |
| mouse | read `0x22e9270` | buttons (`out+32`), deltas | clear deltas and buttons in `out` after the original; leave the cursor position |
| pad | `0x22a1280` | the `XINPUT_STATE` for the slot's user index | pass a copy with `Gamepad` zeroed, packet number kept |

- No site is SPL's: SPL hooks `Keyboard:WriteInput` (below our keyboard
  site) and the mouse update above our mouse site. Another module that
  patched one of these first makes al.hooks refuse it by its bytes, and
  al.input reports which path is unavailable.
- Snapshots are already in virtual keys, so the scan-code mapping in
  `native.cpp` goes away. `key_down` reads the copied held map.
- Clearing the keyboard maps after the original leaves the game nothing
  pressed or released while captured; the next frame's "previous" is
  empty, so release edges do not fire late.
- Open: DirectInput pads (`0x22a0880`) for v1 or later; whether the
  cursor position should also freeze while the mouse is captured; Steam
  Input (does the game read pads another way with it on?); exe versions
  other than 421810 get UNAVAILABLE with the reason.
- The window subclass stays as it is.

## Open questions (one probe run)

Questions 1 and 3 are answered above (DirectInput; raw input through
DirectInput's own window). Question 5 too, by the user: **Alt+F4 does
nothing in MHW.** In frame-input-1 launch 2 (and earlier in astera-mo2-2)
the user pressed it, saw no reaction, and ended the process from outside;
the probe logged no `WM_SYSKEYDOWN F4` before that. So there is no Alt+F4
path to warn plugins about. A process ended from outside or by a crash
gets no notice, and plugins save settings when they change. Question 2 is
answered by input-probe-3. Question 4's Steam Input on/off comparison remains.

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

## The API: "al.input" 1.0

The canonical C ABI is `include/al/al_plugin.h`. API 1.6 adds this interface;
earlier tables and phases retain their layouts. Request it during plugin init
or PRE_MAIN so the loader can arrange swap-chain/window observation. Window
discovery currently follows AL's DirectX 12 path.

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

typedef struct al_pad_hotkey_t {
    uint32_t size, user_index, buttons, modifiers;
    const char* name;
} al_pad_hotkey_t;

typedef struct al_pad_state_t {
    uint32_t size, connected, packet;
    uint16_t buttons;
    uint8_t left_trigger, right_trigger;
    int16_t left_x, left_y, right_x, right_y;
} al_pad_state_t;

typedef struct al_input_status_t {
    uint32_t size, state, devices, captured;
    void* window;
    const char* reason;
} al_input_status_t;

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
    al_result_t(AL_CALL* pad_state)(al_plugin_t* self, uint32_t slot, al_pad_state_t* out);
    uint32_t(AL_CALL* add_pad_hotkey)(al_plugin_t* self, const al_pad_hotkey_t*, al_hotkey_fn, void* user);
    al_result_t(AL_CALL* unsubscribe_messages)(al_plugin_t* self);
    void(AL_CALL* status)(al_plugin_t* self, al_input_status_t* out);
} al_input_t;
```

Pad slots are 0..3; `AL_PAD_*` use XInput's button masks. A pad hotkey
requires every selected button, plus any keyboard modifiers. Steam Input
on/off still needs checking in MHW; no other pad path is claimed here.

`AL_MOD_CTRL`, `AL_MOD_SHIFT`, `AL_MOD_ALT` are bits 1, 2, 4. Device bits
`AL_INPUT_KEYBOARD`, `AL_INPUT_MOUSE`, `AL_INPUT_PAD` are 1, 2, 4;
`AL_INPUT_ALL` is 7. Capture requires every requested path to be installed;
it returns AL_ERROR and logs why while those paths are missing. Repeated
capture calls nest per plugin; release pops the latest mask. The union of
all live owners is blocked. A disabled owner never blocks the game, and
EXITING retires capture. The owning plugin's hotkeys keep working; another
owner's capture suppresses them. Suppressed edges do not replay on release.

One message subscription per plugin; another replaces it, or unsubscribe
removes it. Callback faults disable the plugin, release its capture and
allow the message/game frame to continue. Focus loss makes queries neutral
and suspends blocking. Old snapshots and held-key edges do not reappear
on focus gain. Pad outputs/status respect the caller's size. No key values
or typed text are written to loader logs; diagnostics contain counts and
registration/refusal information.

## How it works (core side)

- **The window** comes from the swap chain the game creates (al.d3d12
  already sees it), checked against the process's top-level windows.
- **Window messages:** AL subclasses the game window, as SPL does
  (`SetWindowLongPtr`), at the first swap chain. Message callbacks run in
  order; a callback that returns non-zero keeps the message from the game.
  If another module subclasses the window later, AL stays in the chain
  behind it; AL never restores the old procedure while plugins are loaded.
- **The input layer** (settled by input-probe-3, but see input-check-1):
  AL hooks the methods the game reads its input through, once DINPUT8 and
  XInput are loaded: DirectInput's `GetDeviceState` for the keyboard's and
  the mouse's devices (`GetDeviceData` too, though the game does not use
  it), and `XInputGetState`. Only System32's own methods are hooked; with
  the Steam overlay on, the game calls the overlay's wrappers instead,
  and how to reach those reads is open. The method addresses
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
- **Cost:** fixed-size snapshot publication, virtual-key mapping and a
  capture check per game read. No per-frame allocation or global input
  polling. A local stand-in measurement of six native hook reads plus the
  empty END pass was about 0.2 microseconds per frame, including stand-in
  originals and MinHook; it does not measure Windows device-driver or MHW
  timing. The window layer forwards to the previous procedure when no
  callback consumes a message.

`src/core/input/service.*` owns hotkeys, capture, messages and atomic
snapshots. `src/core/input/native.*` resolves System32 device methods from
throwaway keyboard/mouse objects after the game's DLLs load. It hooks each
distinct target once, including aliases/forwarded XInput exports; nested
calls do not overwrite the original snapshot with captured neutral data.
Only reads on AL's frame thread participate; other-thread reads, return
codes, failed-read buffers and last error are preserved. Standard keyboard
and mouse state formats are supported. Buffered DirectInput capture returns
no events; hotkey snapshots use the measured GetDeviceState path.

The native layer is pinned until exit. It never globally enables/unhooks
other tools' hooks. Partial or later installation failures leave its
existing hooks forwarding and publish an UNAVAILABLE reason and warning.
Window handles must belong to this process and be top-level windows;
destruction messages always reach the previous procedure, even if a plugin
tries to consume them. AL remains behind a later SPL-style subclass.

## Tests

- `input_service`: combination edges, modifiers, pad hotkeys and coherent
  concurrent queries, conflicts, nested capture, callback order/consumption,
  faults, focus, allocation refusals and the empty frame path.
- `input_native`: a real test window with a later subclass, native MinHook
  entries on fake keyboard/mouse reads and nested/aliased XInput stand-ins;
  capture on/off, real pre-capture snapshots, failed/disconnected reads,
  thread isolation, last error, destruction and steady-read allocations.
- `input_native_refusal`: a later installation failure makes an already
  active layer forward again and reports its reason visibly.
- `input_host`: a DLL client of the public C table, version checks, input
  alone requesting graphics/window observation, frame/message callbacks,
  bounded queries and EXITING. C/C++ tests pin the ABI layouts.
- MHW with a hotkey/capture test plugin, DLSS FG and SPL remains the next
  run. Disable the old input_probe first: it has independent MinHook hooks.
