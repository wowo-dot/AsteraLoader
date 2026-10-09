# Design: input, hotkeys and input capture (API 1.6)

Status 2026-10-10: API and input service implemented. The input layer now
captures at the game's own input functions through al.hooks ("Capture at
the game's own input sites" below), after the first layer, on DirectInput
and XInput, met the Steam overlay in input-check-1. Confirmed in MHW with
the overlay on: input-check-3, tag v0.1.0-dev.16. `handoff-input-20261010.md`
has the service rules. Roadmap "path to v1" step 2.

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
  `XInputGetState`. Turning the overlay off would not settle it under MO2:
  overlay-off-1 showed `gameoverlayrenderer64.dll` loading whatever the
  Steam setting. The game-side layer below makes the question moot.

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

More from the same database, read through the IDA MCP (2026-10-10):

- **The mouse buttons are in the keyboard's maps too.** `Keyboard:WriteInput`
  ORs `GetAsyncKeyState(vk) & 0x8001` for the five keys in the table at
  `exe+0x3511fc8` (VK 1, 2, 4, 5, 6) into the held map. Bindings can read
  clicks from there as well as from the mouse state.
- **The mouse update `exe+0x22e89a0`** passes `mouse+0x38` as `out`: it
  moves the buttons to "previous" (`+0x5C`), zeroes the deltas and buttons,
  calls the read only while `hData+0x29ad` and `mouse+0xB2` are set, then
  computes pressed, released and changed (`+0x60` to `+0x68`) and repeat
  timers. On frames it skips the read, the game has no buttons.
- **The keyboard update** edge-detects from its own previous map, so a key
  that capture hid and that is still held when capture ends would arrive
  as a fresh press (Esc closing a menu would then open the game's). It
  clears every map itself when `exe+0x22e58e0` says so.
- **The pad slot function `exe+0x22a59d0`** calls `0x22a1280` only after a
  successful `XInputGetState` and while `manager+0xBB4` is set; a missing
  pad never reaches it. `0x22a1280` keeps the previous buttons at
  `slot+0x160` and derives digital bits from the triggers above the
  slot's threshold byte (`slot+0x19`) and from the sticks.

### Capture at the game's own input sites (implemented)

AL hooks the game's functions through al.hooks instead of DirectInput's and
XInput's: they are reached the same way with the Steam overlay on or off,
or anything else wrapping DirectInput (ReShade, SPL), and they are checked
by their bytes like the frame layer's sites. `src/core/input/sites.cpp`
gives them for the verified 421810 exe only; any other exe gets
UNAVAILABLE with the reason.

Patterns: the shortest unique ones from `sigscan.py make` on the decoded
image (d78611...), each from the function's first byte; `sigscan.py audit`
finds each exactly once:

- keyboard update `0x22e74f0`: `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 33 FF 48 8D B1 38 01 00 00`
- mouse read `0x22e9270`: `40 55 53 56 57 41 56 48 8B EC 48 81 EC 80 00 00 00`
- pad `0x22a1280`: `48 89 5C 24 08 57 44 8B 9A 60 01 00 00`

| Device | Site | Snapshot (after the original) | Capture |
|---|---|---|---|
| keyboard | update `0x22e74f0` | the held map: 256 bits by virtual key | after the original, clear the captured keys in all six maps |
| mouse | read `0x22e9270` | buttons (`out+32`, bits 0 to 4) | after the original, zero the deltas and buttons in `out`; the cursor position stays |
| pad | `0x22a1280` | the `XINPUT_STATE` for the slot's user index (`slot+0x14`) | pass a copy with `Gamepad` zeroed, packet number kept |

- **Devices own their keys.** Keyboard capture clears every key but the
  five mouse buttons; mouse capture clears those five in the keyboard's
  maps as well as in the mouse state.
- **Held through the release, hidden until let go.** Keys, mouse buttons,
  pad buttons and triggers (past the slot's own threshold) that capture
  hid stay hidden from the game until they are released; then they work
  as usual. Sticks come back at once. The game sees no late release edge
  either, since it never saw the press.
- **Unread devices are idle.** At the end of each frame, a mouse the game
  did not read reports no buttons, and a pad slot that was not processed
  reports disconnected.
- Order -500, as the frame layer: outermost, so other detours on these
  functions see the real input and capture covers whatever they leave.
- Only calls on the frame thread while input is RUNNING take part; other
  calls, and every call after EXITING or a failure, pass straight on.
- The hooks are registered at the end of PRE_MAIN if a plugin asked for
  al.input by then, otherwise at the first frame after one does. Nothing
  is hooked when no plugin uses input.
- A refused site (another module patched it first, so its bytes differ)
  leaves the other devices working: `status.devices` lacks that device,
  `status.reason` and a warning say why. All three refused: UNAVAILABLE.
- No site is SPL's: SPL hooks `Keyboard:WriteInput` (below our keyboard
  site) and the mouse update above our mouse site.
- Open: DirectInput pads (`0x22a0880`) are not captured; whether the
  cursor position should also freeze while the mouse is captured; Steam
  Input (does the game read pads another way with it on?); whether Better
  Input Detection, which patches the game's input code, touches these
  sites (the run will show a refusal if it does).
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
- **The input layer:** al.hooks detours on the game's keyboard update,
  mouse read and XInput pad processing ("Capture at the game's own input
  sites" above). The first layer hooked DirectInput's `GetDeviceState` and
  `XInputGetState` in System32 and was refused under the Steam overlay
  (input-check-1).
- **Snapshots, not polling:** after each of those functions runs, the
  layer keeps a copy of what it produced (the 256-bit held map, the mouse
  buttons, each pad's `XINPUT_STATE`) before capture changes it. The game
  runs them once per frame on its frame thread, inside Main:Update, so the
  snapshots are fresh at al.frame's END. Hotkeys, `key_down` and pad
  queries use them: no extra system calls, the same data the game sees,
  and the game's own focus rules (nothing held while it is in the
  background).
- **Hotkeys** are edge-detected per frame from the snapshots and called at
  al.frame END, on the game thread.
- **Capture** changes what the game keeps: its key maps, mouse deltas and
  buttons, and the pad state it is handed. Window messages are not
  involved: the game does not read input from them (frame-input-1).
  Plugins still see the real state through the snapshots and their
  message callbacks.
- **Cost:** a capture check and fixed-size snapshot publication per game
  call; no per-frame allocation or global input polling. Six stand-in
  calls plus both end-of-frame passes took about 0.15 microseconds per
  frame locally, without patching; this is not an MHW measurement. The
  window layer forwards to the previous procedure when no callback
  consumes a message.

`src/core/input/service.*` owns hotkeys, capture, messages and atomic
snapshots. `src/core/input/sites.*` gives the verified game sites and
layouts; `src/core/input/native.*` registers the three hooks with al.hooks,
runs the detours and subclasses the window. Return values pass through.

The native layer is pinned until exit. A refused site leaves the other
devices running and says why; all three refused makes input UNAVAILABLE.
Window handles must belong to this process and be top-level windows;
destruction messages always reach the previous procedure, even if a plugin
tries to consume them. AL remains behind a later SPL-style subclass.

## Tests

- `input_service`: combination edges, modifiers, pad hotkeys and coherent
  concurrent queries, conflicts, nested capture, callback order/consumption,
  faults, focus, allocation refusals and the empty frame path.
- `input_native`: stand-ins with the game's argument layouts and edge
  logic, reached through the real al.hooks registry with a stand-in
  installer; a real test window with a later subclass. Per-device capture
  (mouse buttons in the key maps included), real pre-capture snapshots,
  held input hidden until released, unread devices idle, thread and focus
  isolation, pass-through after EXITING, steady-frame allocations.
- `input_native_refusal`, `_unavailable`, `_late`: one refused site leaves
  the others working with a reason; unverified sites hook nothing and say
  why; a plugin asking after PRE_MAIN gets the hooks at the next frame.
- `input_host`: a DLL client of the public C table, version checks, input
  alone requesting graphics/window observation, frame/message callbacks,
  bounded queries and EXITING. C/C++ tests pin the ABI layouts.
- MHW with a hotkey/capture test plugin, DLSS FG and SPL remains the next
  run. Disable the old input_probe first: it has independent MinHook hooks.
