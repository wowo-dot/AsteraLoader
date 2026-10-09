# Design: "cmfm.hooks", shared hooks on game functions (API 1.4)

Status 2026-10-09: the plugin API and the registry are implemented and
tested without patching (`game_hooks` test). The hook layer is the user's
part (spec below). Until it exists, hooks stay PENDING and never run.
Roadmap item 10; first in the "complete the core" order.

## Why

The survey (`research/plugin-needs-survey.md`) found:

- every MHW plugin bundling its own hooking: MinHook, minhook-sys,
  SafetyHook, hand-made detours;
- nothing ordering two plugins on one function: the quest checks
  (QuestLoader and probably SpecialQuestUnlocker), monster creation, damage.

Two clients are ready:

- **the DLSS mod:** it still keeps 3 Reflex hooks and the game's NGX
  functions on its own MinHook (its migration commit says so);
- **the user's G8F plugin:** 7+ hand-written detours at fixed RVAs.

## The API

`"cmfm.hooks"` 1.0 in `include/cmfm/cmfm_plugin.h`:

- **`hook(cmfm_hook_t)`** returns a handle. Fields:
  - `order` and a `name`, for logs;
  - the target, given one of two ways:
    - **pattern search:** `pattern` (cmfm.scanner syntax, at least 5 bytes)
      must match exactly once in the game's code; target = match + `offset`;
    - **address:** `address`, with `pattern` matching at `address - offset`.

    Either way the bytes are checked before anything is patched (AGENTS.md:
    hooks only at signature-verified sites), and the target must lie in the
    exe's executable sections;
  - `detour`, with the target's signature, and `next`, where CMFM stores
    the function to continue with.
- **`set_enabled(handle, on)`** takes a detour out of its chain, or puts it
  back in its place.
- **`state(handle)`:** PENDING, ACTIVE, DISABLED or FAILED.

### Rules

- **Registration** is open from `cmfm_plugin_init` on, with no deadline.
  Hooks made before PRE_MAIN ends take effect when it ends, before the
  game's CRT starts. Later ones take effect at once.
- **Order:** a lower `order` runs first, further from the original; ties run
  in load order.
- **One patch per function:** joining, leaving and coming back relink the
  chain.
- **Relinking is safe while the function runs on other threads.** The
  registry writes each detour's `next` from the end of the chain toward the
  front, then the entry. So a detour that becomes reachable already has its
  continuation, and every write is one atomic pointer store.
  - A detour taken out keeps its old `next`, so calls already inside it
    finish normally.
  - Plugins are never unloaded, so their code stays valid.
- **Faults:** detours run directly, so CMFM cannot catch their faults, as
  with any hook. CMFM's crash handler still names the plugin.
- **No internal-call regions here.** Unlike cmfm.d3d12, a plugin's calls
  into game functions go through the chains like the game's own calls.

### Core side (implemented)

- `src/core/game_hooks/registry.{hpp,cpp}`:
  - target checks against the exe's code sections;
  - pattern resolution through the cached scanner, which needs exactly one
    match;
  - one chain per target, ordered;
  - safe relinking, states and the summary for the core log.
- `src/core/plugins/host.cpp`: the C table, and `Host::game_hooks()` for the
  hook layer.
- `game_hooks` test, using functions in the test exe as stand-in game code:
  - every refusal (short pattern, differing bytes, data not code, several
    matches, missing next, a struct too small);
  - pending before the hook layer and the order once it arrives;
  - a late join without a second patch, ties in load order;
  - off/on with the entry skipping the detour, and owner-only switching;
  - pattern search;
  - a refusal by the hook layer, reported to the plugin and kept for later
    hooks;
  - the summary.

## The hook layer (the user's part)

Implement `cmfm::game_hooks::Installer` (`registry.hpp`) and hand it over in
`phases::arm`, after PRE_MAIN:

```cpp
host.game_hooks().set_installer(&g_game_installer);  // installs every pending chain now
```

- **`create(target, head, original)`:** an inactive hook on `target`, one
  per function.
  - The entry jumps through the pointer at `head`, for example
    `mov r11, imm64(head); jmp [r11]`. The registry keeps `*head` at the
    first enabled detour, or at the original. R10/R11 are scratch at a
    function entry on x64 Windows; argument registers and the stack must be
    untouched, as in the d3d12 entry.
  - Store the trampoline (the original code) in `*original`.
  - Unlike the d3d12 entry, there is no `internal()` check.
- **`enable(target)`:** activate it.
- **Before patching:**
  - the registry has already checked the bytes and the code range;
  - a target someone else already patched (a Stracker plugin's MinHook) is
    allowed and logged with the owning module, as in the d3d12 installer;
  - refuse what cannot be relocated, and say why. The registry marks the
    function FAILED and tells each plugin.
- **Afterwards:**
  - keep the hooks until exit;
  - `create`/`enable` may be called from any thread later (a plugin hooking
    at WIN_MAIN or in play), always under the registry's lock;
  - log `host.game_hooks().summary()` after arming and at SHUTDOWN.

**Test plan** (fake game):

- two plugin fixtures detour one function, one by pattern and one by
  address; the game calls it; the order and the arguments are checked;
- a third fixture joins at WIN_MAIN;
- one fixture toggles its detour while another thread calls the function in
  a loop;
- an existing MinHook patch on a second function stays under CMFM's;
- `cmfm.dll` still imports only KERNEL32.
