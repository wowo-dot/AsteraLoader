# Handoff: the DLSS mod after the Astera Loader rename (2026-10-09)

Status 2026-10-09: done. DLSS mod 0.15.0 (dlssupdate 871bea3) runs on
Astera Loader 81a5b21 with frame generation on (run dlss-astera-1): its
three live frame hooks are on `al.hooks`, and Main:Update is one chain,
`layers=mhw_dlss,site_probe`.

The loader formerly called CMFM is now **Astera Loader** ("AL"); CMFM is its
menu plugin (`decisions.md` item 11). Only names changed. The ABI is the
same API 1.4: same structs, sizes, versions, phases and rules. A plugin
needs a rebuild against the renamed header and nothing else.

The loader no longer recognises the old export names. Until the mod is
updated, Astera Loader reports it as "not an Astera Loader plugin" and the
game runs without it. Update the loader and the mod together.

## What changed

| Before | After |
| --- | --- |
| `include/cmfm/cmfm_plugin.h` | `include/al/al_plugin.h` |
| `cmfm_*` types and functions, `CMFM_*` macros | `al_*`, `AL_*` (for example `al_plugin_info_t`, `AL_API_VERSION`, `AL_CALL`, `AL_PLUGIN_EXPORT`) |
| exports `cmfm_plugin_info`, `cmfm_plugin_init` | `al_plugin_info`, `al_plugin_init` |
| interfaces `"cmfm.d3d12"`, `"cmfm.hooks"`, `"cmfm.game"`, `"cmfm.scanner"` | `"al.d3d12"`, `"al.hooks"`, `"al.game"`, `"al.scanner"` |
| `cmfm_launcher.exe`, `cmfm\cmfm.dll` | `AsteraLoader.exe`, `AsteraLoader\AsteraLoader.dll` |
| plugins in `<game>\cmfm\plugins\<name>\` | `<game>\AsteraLoader\plugins\<name>\` |
| plugin data in `cmfm\data\<name>`, logs in `cmfm\logs\plugins` | `AsteraLoader\data\<name>`, `AsteraLoader\logs\plugins` |

`AL_VERSION` is deliberately not a name in the header: OpenAL uses it. The
loader's tests build the header after OpenAL's `AL_` names, so a plugin can
include both.

## Steps in `D:\mh world modding\dlssupdate`

1. Replace `third_party/cmfm/cmfm_plugin.h` with a copy of this repository's
   `include/al/al_plugin.h` (for example as `third_party/al/al_plugin.h`), and
   update `third_party/cmfm/README.md` to match.
2. Rename the API uses: `cmfm_` → `al_`, `CMFM_` → `AL_`, `"cmfm.d3d12"` →
   `"al.d3d12"`. The plugin entry is in `src/reflex/presentation_probe.cpp`
   (`plugin_info`, the two exports, the `cmfm.d3d12` lookup); the shared
   chain code in `src/cmfm/cmfm_hooks.{hpp,cpp}`. Renaming the mod's own
   internal names (the `src/cmfm` folder, `mhw_cmfm_hooks`) is optional.
3. Packaging: `tools/package_release.py` and `tests/test_package_release.py`
   install to `AsteraLoader/plugins/mhw_dlss/`. The README text says "an Astera
   Loader plugin" and "Uninstall: delete the AsteraLoader/plugins/mhw_dlss folder".
4. MO2: the "MHW DLSS (CMFM plugin)" mod's files move from
   `cmfm\plugins\mhw_dlss\` to `AsteraLoader\plugins\mhw_dlss\`. Take
   `mhw_dlss.ini` with them; it is the user's configuration.

## Why frame generation was off in the last CMFM runs

In hooks-mo2-1 and hooks-mo2-2 (game PIDs 54724 and 61180) DLSS frame
generation never started. The mod's `reflex-frames-<pid>.jsonl` shows
`frame_anchor_check` passing, then
`{"event":"worker_failed","reason":"live-frame-full-gate",...,"fg_off_requested":true}`
and no `frame_hooks_installed` / `fg_armed`. The runs before them
(graphics-dlss-1, crash-mo2-1/2) armed frame generation normally.

The cause is a second hook on the same game function, not the loader:

- `src/reflex/live_frames.cpp` hooks three game functions with its own
  MinHook: `0x1af2b60` (Main:Update, kind 0), `0x229a080` (publish, kind 2)
  and `0x23a7f50` (submit, kind 11). Before hooking it checks the SHA-256
  of each function's first `bytes` bytes (`live-frame-full-gate`).
- From hooks-mo2-1 on, the diagnostic plugin `site_probe` hooked
  Main:Update through `al.hooks` at PRE_MAIN. Its 5-byte entry patch
  changed the hash, so the gate refused and the worker turned frame
  generation off.

This will happen for everyone once API 1.5 ships: the loader's own frame
tick hooks Main:Update through `al.hooks`. So the live frame hooks must
join the shared chains instead of patching on their own.

## Step 5: the live frame hooks on `al.hooks`

In `src/reflex/live_frames.cpp`:

1. Get the interface once at init:
   `api->get_interface(self, AL_HOOKS_INTERFACE, AL_HOOKS_VERSION)`. If it is
   missing, fail with a reason, as now.
2. In `install`, replace `MH_CreateHook` / `MH_QueueEnableHook` /
   `MH_ApplyQueued` with one `hook()` per gate:

   ```c
   al_hook_t hook = {sizeof(al_hook_t), order, "mhw_dlss Main:Update",
                     pattern, 0, 0, (void*)(image + g.rva),
                     (void*)g.entry, &s.originals[g.kind]};
   uint32_t handle = hooks->hook(self, &hook);
   // handle != 0 and hooks->state(self, handle) == AL_HOOK_ACTIVE, or fail
   ```

   - `pattern`: at least 16 original bytes at the function's start, as
     hex. The registry checks them against the original bytes even after
     another plugin's patch. For Main:Update that is
     `40 53 48 81 EC 80 00 00 00 48 8B D9 E8 5F C6 76`.
   - `next` is `&s.originals[g.kind]`: AL writes the continuation there
     before the hook goes live, so `mhw_reflex_boundary_forward` keeps
     working unchanged.
   - `order`: give the Main:Update hook a low order, for example -1000, so
     its Reflex simulation markers wrap the other per-frame detours (AL's
     frame tick included). The publish and submit hooks can stay at 0.
   - Installing from the worker after device creation is fine: by then
     `al.hooks` is armed and a new hook is active at once.
3. The SHA gate: hash from byte 16 on (`[rva+16, rva+bytes)`). The first
   bytes belong to whoever owns the entry's chain; the registry has checked
   them.
4. In `stop`, call `hooks->set_enabled(self, handle, 0)` for each hook and
   keep the call drain. The entry stays patched by AL's chain, so the
   `frame_hook_restoration` check against the original head no longer
   applies; report the hook state instead.
5. `reserve` (the trampoline pool near the targets) is not needed for these
   three; AL's installer allocates its own entries.

## Checking it

Keep `site_probe` installed for this run: it hooks Main:Update at order 0,
so both hooks share one chain. The loader's core log
(`AsteraLoader\logs\core.<pid>.log`) should show:

- `plugin name=mhw_dlss version=<new> status=active`
- `game_hook target=exe+0x1af2b60 name=... state=active layers=mhw_dlss,site_probe`
  after arming (the DLSS hook joins later, so check the SHUTDOWN summary)
- `d3d12_state=running presentation_owner=mhw_dlss frames=<n>` at SHUTDOWN

The DLSS log should show `frame_hooks_installed` and `fg_armed`, and frame
generation on screen, as in graphics-dlss-1.
