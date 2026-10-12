# Design: "al.patch", checked byte changes in the game (API 1.8)

Status 2026-10-12: agreed with the user ("the general version"),
implemented (87b010f) and tested outside the game: the registry with a
stand-in writer (`test_patch.cpp`), the native writer (`test_patch_writer.cpp`),
the crash report (`test_crash.cpp`) and end to end through the launcher
and core (`patch_native`). Confirmed in MHW (v0.2.0-dev.1): patch-1 with
no al.patch users (every hook chain still active), patch-2 with CMFM
cda17c3 changing its two Start menu pointers through al.patch (written at
WIN_MAIN, switched off by CMFM at SHUTDOWN, listed in the core log and in
an exit-time crash report with their owner).

## Why

`al.hooks` (1.4) shares one kind of change to the game: a hook at a
function's entry. Mods make other kinds too, and AL does not know about
them:

- **CMFM** (its `docs/al-requests.md` item 1): its "Mods" entry in the
  Start menu swaps two 8-byte pointers in the game's read-only data
  (RVAs `0x34818B8` and `0x3481868`, exe 421810). It does that today with
  its own `VirtualProtect` helper, in every run since 2026-10-11.
- **Unbloat** writes game code directly (`research/plugin-needs-survey.md`).
- **The dirty game image** (`research/clean-image-20261011.md`) is a record
  of what the user's mods changed in a running game, at 29 places:
  - 22 jumps at function entries: hooks, which `al.hooks` covers;
  - NOP runs, a `jz` turned into `jmp`, a `ja` turned into `jmp`, and two
    calls pointed elsewhere: changes inside functions, which nothing
    covers.

Each of these mods writes on its own. Two of them on the same bytes undo
each other silently, and a crash near the changed bytes names nobody.

The user chose the general version (2026-10-12): one service for every
checked byte change, code and data, rather than CMFM's case alone.

## The API

`"al.patch"` 1.0, API 1.8, in `include/al/al_plugin.h`:

```c
#define AL_PATCH_INTERFACE "al.patch"
#define AL_PATCH_VERSION AL_MAKE_VERSION(1, 0)

/* Kinds */
#define AL_PATCH_CODE 0u /* bytes in the game's code: NOPs, a flipped jump, any same-length change */
#define AL_PATCH_DATA 1u /* bytes in the game's data, read-only or writable: a pointer, a constant */
#define AL_PATCH_CALL 2u /* a call or jmp rel32 in the game's code, sent to `function` */

#define AL_PATCH_MAX_LENGTH 64u

typedef struct al_patch_t {
    uint32_t size;           /* sizeof(al_patch_t) */
    uint32_t kind;           /* AL_PATCH_CODE, AL_PATCH_DATA or AL_PATCH_CALL */
    const char* name;        /* for logs and reports, e.g. "Start menu: System tab items"; may be NULL */
    const char* pattern;     /* optional, al.scanner syntax; see "Finding the bytes" */
    int32_t offset;          /* target = where the pattern matches + offset */
    uint32_t length;         /* bytes changed: 1 to AL_PATCH_MAX_LENGTH; 5 for AL_PATCH_CALL */
    void* address;           /* the target, or NULL to search for pattern */
    const void* expected;    /* the `length` bytes that must be there now */
    const void* replacement; /* the `length` bytes to write; NULL for AL_PATCH_CALL */
    void* function;          /* AL_PATCH_CALL: where the call should go */
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
    /* Checks and makes one change; its handle, or 0 with the reason in the
     * plugin's log. Before PRE_MAIN ends the change is checked and claimed
     * now and written when PRE_MAIN ends; later, before apply returns. */
    uint32_t(AL_CALL* apply)(al_plugin_t* self, const al_patch_t* patch);
    /* Puts the expected bytes back (0), or the replacement again (1). Only
     * the plugin that made the change. */
    al_result_t(AL_CALL* set_enabled)(al_plugin_t* self, uint32_t handle, uint32_t enabled);
    /* The change's state; AL_PATCH_FAILED for an unknown handle. */
    al_patch_state_t(AL_CALL* state)(al_plugin_t* self, uint32_t handle);
} al_patches_t;
```

State numbers match `al.hooks` (0 pending, 1 active, 2 disabled, 3 failed),
and so does the timing: changes made before PRE_MAIN ends are checked and
claimed at once and written when it ends, before the game's own code
starts; later ones are written before `apply` returns. AL's start-up
wrapper is still on the game's entry while plugins start, and it is gone
by then. AL's other start-up observers change no code bytes (the WinMain
observer uses a debug register; DEVICE_CREATED uses the import table,
which al.patch refuses). So no range has to be reserved for them.

## Rules

### Finding the bytes

- **By address:** `address` is the target. With a `pattern`, it must also
  match at `address - offset`, as in `al.hooks`.
- **By pattern:** `address` is NULL and `pattern` must match exactly once:
  in the game's code for CODE and CALL, in its data sections for DATA.
  Target = match + `offset`.
- **Always:** the `length` bytes at the target must equal `expected`.
  That is the final check, even when a pattern found the place. A plugin
  that gives only an address and expected bytes should name its supported
  exe in `exe_sha256` (its info struct), since a few bytes alone can
  match by chance.
- **Pattern checks see the game's original bytes.** Where a pattern
  covers bytes another plugin has changed (a hook's entry or a patch),
  AL checks it against the bytes from before that change. `al.hooks`
  gets the same rule for patches: today it only does it for its own
  hooks.

### Where AL makes changes

- **CODE and CALL:** inside the exe's executable sections.
- **DATA:** inside the exe's other sections, read-only (`.rdata`) and
  writable (`.data`).
- **Refused everywhere:**
  - the PE headers;
  - the data directories: imports and the import address table, exports,
    exception data (`.pdata`), relocations, resources, TLS, load config,
    debug, delay imports;
  - the DRM stub's section (`.bind`): AL leaves Steam's stub alone;
  - any other module: system DLLs, NVIDIA's, other mods'. Only the game
    exe.
- **The game checks parts of its own code.** AL does nothing about those
  checks. A change a plugin makes is visible to them, as with any mod.

### One owner per byte

- AL keeps one map of claimed byte ranges in the game's image, shared by
  `al.hooks`, `al.patch` and AL's own changes. Claims never overlap:
  - a patch that overlaps a claim is refused, and the reason names the
    owner ("held by the plugin cmfm: Start menu: System tab items");
  - so is a hook whose bytes overlap a patch.
- **A hook's claim** covers the bytes MinHook writes (5 at the entry, or
  5 above it and 2 at it in its hotpatch form) and the entry instructions
  it copies into its trampoline: decoded with the same disassembler
  (hde64), at least 5 bytes. A change to those copied bytes would never
  run, and putting it back later would overwrite the hook.
- **AL's own hooks** (frame, exit, title, input) are ordinary `al.hooks`
  chains, so their claims name "Astera Loader".
- A patch inside a hooked function but outside the hook's claim is fine.

### Writing

- **Atomic when it can be:** DATA of 1, 2, 4 or 8 bytes at a naturally
  aligned address is a single interlocked compare-and-swap. CMFM's
  pointers are this case.
- **Everything else** (code, longer or unaligned data) is written with the
  game's other threads paused, as MinHook does for hooks. For code, AL
  checks that no paused thread is stopped inside the bytes being changed
  (a thread exactly at their first byte is fine) and retries a few times
  before failing with the reason. Then AL flushes the instruction cache.
- **What the pause does not cover:** AL checks where each paused thread
  is, not the return addresses on its stack. A thread inside a function
  called from the changed range comes back into it after the write. So
  plugins change whole instructions and keep an instruction boundary
  wherever a call in the range returns (single-byte NOPs always do). The
  header and the plugin guide say so.
- **Read-only pages** get write access for the write and their old
  protection back after it, whatever it was.
- One lock serializes every write AL makes to the game's image: hooks
  and patches.

### Switching off and on

- `set_enabled(handle, 0)` puts the expected bytes back if the
  replacement is still there; `set_enabled(handle, 1)` writes it again if
  the expected bytes are still there.
- **If someone else changed the bytes since** (a tool outside AL), AL
  leaves them as they are. The state becomes FAILED, and the plugin's log
  says what AL found.
- Only the plugin that made a change can switch it.

### Calls (AL_PATCH_CALL)

- The target must hold `E8` (call) or `E9` (jmp) with a 32-bit
  displacement, and `expected` is those 5 bytes.
- `function` is usually far from the game exe, out of a rel32's reach.
  AL places a 14-byte stub within reach (`jmp [rip+0]` and the address)
  and points the displacement at it.
- **Stubs:** one page each, written while it is read-write, then made
  read-and-execute and never written again. No memory is writable and
  executable at once. Switching the patch off and on reuses the stub.
- `*original` gets the old target before the new call can run, so the
  plugin can call through, as with a hook's `next`.

### When a plugin fails, and at the end

- **A plugin that faults** and is disabled keeps its changes. Putting a
  value back while the game may be reading it is the riskier choice, and
  AL never unloads a plugin's DLL, so a pointer into it stays valid.
- **At SHUTDOWN**, after the plugins' own SHUTDOWN, AL puts back the DATA
  changes that are still active and still the plugin's. Code and call
  changes stay: the game's threads may still be running that code, and
  the process is ending (hooks stay too).
- The header gets one more sentence where it already says "AL never
  unloads the plugin": changes made through al.patch may point into the
  plugin's DLL.

### Logs and crash reports

- **The plugin's log:** each change made, switched or refused, with the
  reason.
- **The core log:** at SHUTDOWN, one line per change: owner, name, kind,
  RVA, length, state, as `al.hooks` does for its chains.
- **Crash reports** get a section listing the active changes and their
  owners. A fault inside changed code bytes names the patch's owner as
  well as the game. The crash handler reads a fixed-size table that AL
  fills as changes are made (no allocation while handling a crash).

## Out of scope for 1.0

- **Mid-function hooks** (run plugin code at an instruction, with the
  registers). They are the high-level form of "jump to a code cave". A
  candidate for 1.1 when a plugin needs one.
- **Memory near the exe for plugins' own code.** `AL_PATCH_CALL` covers
  redirected calls; arbitrary code caves come with mid-function hooks.
- **Other modules** and the **import table**. Functions in other modules
  are `al.d3d12`'s and `al.hooks`' business.

## CMFM, the first client

- **Its two Start menu pointers:** DATA, length 8, by address, expected =
  the System tab's table, replacement = its copy. Atomic.
- It applies them at WIN_MAIN as today. It no longer needs to put them
  back at SHUTDOWN (AL does), but may.
- **On an AL older than 1.8:** `get_interface("al.patch")` returns NULL.
  CMFM hides the Start menu entry and says why, as its request planned.
- Its `.data` swaps (the Options pages' row tables) may move to al.patch
  too, for ownership and crash reports; that is CMFM's choice.

## Tests

Outside the game, through the existing fake game and a plugin fixture.
The fixture exe gets a small function and a constant table in its
`.rdata` for the purpose:

- **CODE:** NOP a range and run the function; switch off and on.
- **DATA:** swap an aligned pointer atomically, then an unaligned 6-byte
  value; read-only page protection is restored afterwards.
- **CALL:**
  - send a call to a plugin function that calls `original`;
  - check that the stub page is not writable;
  - switch off and on.
- **Refusals**, each with its reason:
  - expected bytes differ;
  - pattern matches twice;
  - address outside the exe, in the headers, `.pdata`, the IAT or `.bind`;
  - code kind in data and data kind in code;
  - length 0 or over 64;
  - a CALL target that is not `E8`/`E9`.
- **Claims:**
  - a patch on a hook's entry or copied bytes is refused, naming the plugin;
  - a hook over a patch is refused;
  - two plugins on one range: the second is refused with the first's name;
  - a patch next to a hook's claim works.
- **Pattern overlay:** a hook's pattern spanning a patched range still
  matches, checked against the original bytes.
- **Someone else's change:** bytes changed behind AL's back, then
  `set_enabled(0)` leaves them alone and reports FAILED.
- **Threads:** a busy thread running the function while it is patched.
  The write waits until it is clear of the bytes; no crash, no torn
  instruction.
- **Shutdown:** DATA changes are put back, CODE changes stay; the core log
  summary.
- **Crash report:** a fault inside patched code names the patch's owner.
- **ABI:** struct sizes and offsets in `test_abi`, as for every interface.

In game: CMFM switched to al.patch for its Start menu pointers (its
session makes that change), in the user's MO2 setup.

## Release

AL 0.2.0, API 1.8: the release checklist as for 0.1.0, plus an ABI review
entry for 1.8. CMFM's first release follows it.
