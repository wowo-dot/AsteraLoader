# Handoff: the DLSS mod after the Astera Loader rename (2026-10-09)

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

## Checking it

The loader's core log (`AsteraLoader\logs\core.<pid>.log`) should show:

- `plugin name=mhw_dlss version=<new> status=active`
- `d3d12_state=running presentation_owner=mhw_dlss frames=<n>` at SHUTDOWN

The DLSS log should show frame generation as in graphics-dlss-1.
