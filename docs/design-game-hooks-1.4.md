# Design: "al.hooks", shared hooks on game functions (API 1.4)

Status 2026-10-09: the plugin API, registry and native MinHook installer are
implemented. Tested in the original fake game through the production
launcher/core, with three plugin fixtures, late registration, concurrent
callers and an existing external hook. All 41 tests pass and `AsteraLoader.dll`
imports only KERNEL32. See `handoff-game-hook-installer-20261009.md`.
Roadmap item 10.

Confirmed in MHW in run hooks-mo2-1 (AL 8be5411): the diagnostic plugin
`probes/site_probe.cpp` hooked Main:Update (exe+0x1af2b60) by address, with
SharpPluginLoader's pattern checked 25 bytes in. The chain was active from
arming to SHUTDOWN and ran 9710 times on the game's main thread. No other mod
had patched the function. The game exited with code 0.

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

`"al.hooks"` 1.0 in `include/al/al_plugin.h`:

- **`hook(al_hook_t)`** returns a handle. Fields:
  - `order` and a `name`, for logs;
  - the target, given one of two ways:
    - **pattern search:** `pattern` (al.scanner syntax, at least 5 bytes)
      must match exactly once in the game's code; target = match + `offset`;
    - **address:** `address`, with `pattern` matching at `address - offset`.

    Either way the bytes are checked before anything is patched (AGENTS.md:
    hooks only at signature-verified sites), and the target must lie in the
    exe's executable sections;
  - `detour`, with the target's signature, and `next`, where AL stores
    the function to continue with.
- **`set_enabled(handle, on)`** takes a detour out of its chain, or puts it
  back in its place.
- **`state(handle)`:** PENDING, ACTIVE, DISABLED or FAILED.

### Rules

- **Registration** is open from `al_plugin_init` on, with no deadline.
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
- **Faults:** detours run directly, so AL cannot catch their faults, as
  with any hook. AL's crash handler still names the plugin.
- **No internal-call regions here.** Unlike al.d3d12, a plugin's calls
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

## The hook layer (implemented)

`src/core/game_hooks/installer.{hpp,cpp}` provides `NativeInstaller`.
It generates a 13-byte `mov r11, imm64(head); jmp [r11]` entry on a page
made RX before creating the inactive MinHook hook. It never reads the
graphics internal-call flag and makes no helper call on the detour path.
The registry publishes continuations and the head before enabling. Entries
and trampolines stay until process exit, including failed activations.
Trampolines come from the reservation next to the game made at bootstrap
(`decisions.md` item 19), so a hook registered late, after the game has
taken the address space around it, still finds room within MinHook's 1 GB.
Unrelocatable entries return an explicit MinHook reason; an earlier jump
is allowed and logged, with bounded relay traversal to its module.

`phases::arm` sets the installer after PRE_MAIN, even for an empty registry
(plugins may register later), and logs the summary. SHUTDOWN logs it again.
MinHook and registry locking serialize later creation and activation.

### Signature validation after a patch

The original registry checked new registrations against the live patched
entry, so a real WIN_MAIN join would reject the original signature. The
registry now retains the bytes MinHook replaced (five at entry, or five
above and two at entry for its hotpatch form). Signature validation overlays
only that footprint with the pre-AL bytes, after checking the live patch
still matches AL's recorded patch. Bytes outside it remain live checks.

Another tool may hook the same function after AL, over AL's patch: with
SharpPluginLoader force-loaded, it hooks Main:Update after AL's frame layer,
and AL's chain keeps running behind it (spl-release-3, 2026-10-10). Before
0.1.0 such a join was refused, and the DLSS plugin's Main:Update layer with
it, so frame generation stayed off. The registry now also copies the
function's first 64 bytes (to the end of the code section) just before AL
hooks it. When AL's footprint no longer holds AL's patch, the part of a
pattern inside that copy is checked against it, never against the other
tool's bytes; a pattern whose overlap with the footprint lies outside the
copy fails as before. The join is still only a pointer update; AL writes
nothing over the other tool's patch. The plugin's log says it joined after
another tool rewrote the entry, and the summary line shows
`entry=rewritten_by_another_tool`.

In game (spl-release-4, 2026-10-11) the DLSS plugin's Main:Update layer
joined this way and frame generation started. The game then crashed inside
SPL's winmm.dll, on a thread of NVIDIA's sl.dlss_g.dll. The DLSS mod's
research had predicted SPL's Present hook meeting Streamline's proxy. The
fix was briefly reverted (3aef447) and reapplied (abaeed2): the refusal had
only kept frame generation off by accident of timing, since a layer
registered before SPL's patch would have joined anyway. Keeping frame
generation off while SPL is loaded is the DLSS mod's job
(handoff-dlss-spl-20261011.md).

Pattern searches combine the scanner's live matches with verified matches
at existing chain targets, deduplicate them and still require exactly one.
Address-based joins use the same validation. The registry lock covers
resolution, checking and installation, and offset arithmetic is checked.
Joining and toggling remain pointer updates; they never create a second
patch on a function.

### Installer contract

Implement `al::game_hooks::Installer` (`registry.hpp`) and hand it over in
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
- an existing MinHook patch on a second function stays under AL's;
- `AsteraLoader.dll` still imports only KERNEL32.

This is implemented as `game_hooks_native`: two initial fixtures register
by pattern and address, a third joins by pattern at WIN_MAIN, and a worker
installs a new target after startup. The main thread toggles the first
detour 5,000 times while that worker calls the shared function. Each call
checks the legal order, eight mixed integer/float arguments and the result;
the target's patch bytes must remain unchanged. A second target retains a
pre-existing hook in the alpha fixture's separate MinHook instance. The
test also checks arming/SHUTDOWN summaries and that a graphics internal
region does not bypass the game-function chain.

`game_hook_installer` checks inactive creation, head publication, all four
XMM argument registers and stack arguments, last-error preservation,
duplicate/invalid requests, an unrelocatable LOOP instruction, an
address-based late join, an incorrect late signature, a join after another
tool's longer entry jump over AL's patch (accepted, reported, the wrong
signature still refused, the other tool's bytes untouched) and a synthetic
executable-image function requiring MinHook's hotpatch-above form.
