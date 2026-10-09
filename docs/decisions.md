# CMFM decisions

Decisions taken with the user, newest last. Each one answers an open question
from `handoff-20261008.md` or `requirements-dlss-mod.md`.

## 2026-10-08

1. **Name.** One name, CMFM, with two parts: CMFM Core (loader, plugin API,
   hook coordination) and CMFM Menu (settings, status pages, popups). Users
   only ever install "CMFM".
2. **Menu technology.** ImGui for v1, themed to sit close to MHW's look.
   Later, reverse the game's own menu system so CMFM can sit very close to
   native: entries or pages inside the game's own UI. That work also unlocks
   the AA/DLSS option lockout (requirements §4), which needs the game's
   Options menu reversed anyway. Reversing is done on private IDA copies
   under `local/`, with in-memory hooks at signature-verified sites only.
3. **Stracker replacement ships in v1.** The `loader.dll` shim and plugin
   loading are part of the first release.
4. **Licence: MIT.** Matches Stracker's Loader and SharpPluginLoader. Forked
   Stracker code keeps its MIT notice, and its log code keeps its BSD-2
   notice.
5. **Settings storage: per-plugin files.** Each plugin's own config file is
   the source of truth; CMFM reads and writes through the plugin. (The
   recommendation from the handoff; the user did not object.)

6. **Entry: our own launcher (the SKSE route)**, the user's call after run
   entry-slot-1. This answers open question 1.
   - `cmfm_launcher.exe` starts the game suspended, loads `cmfm.dll` by full
     path, then resumes it. CMFM uses no proxy slot at all.
   - Why:
     - the only proxy name the game loads before its graphics device is
       winmm.dll, which SPL and the DLSS mod's standalone mode already use;
     - MO2 already starts this exe suspended and injects usvfs before
       SteamStub, so the game tolerates it;
     - no proxy DLL means no collisions, which fits the "fewer bandaids"
       principle.
   - v1 is launcher-only. Steam users start the game through Steam launch
     options (`"<path>\cmfm_launcher.exe" %command%`, to be verified) or a
     shortcut. A proxy fallback comes only if launch options fail.
   - Changes the DLSS mod's load-point requirement: Steam's plain Play button
     without launch options no longer loads CMFM. The DLSS mod's standalone
     winmm.dll covers that case.
   - Evidence and trade-offs: `research/entry-slot.md`.
   - First step: a launcher plus a `cmfm.dll` that only logs startup timing.
     It proves the injection under MO2, and that usvfs still reaches the
     game.

## 2026-10-09

7. **API roadmap:** `api-roadmap.md`. API 1.1 adds services that need no
   hooks (interfaces by name, game identity, per-plugin timing,
   dependencies, data folders, plugin-to-plugin interfaces, a shared cached
   signature scanner); 1.2 the hook-based ones (remaining phases, per-save
   plugin data, a shared hook service, graphics coordination, input, game
   thread tasks); then the Menu milestone.
8. **Stracker plugins load at PRE_MAIN**, the moment SPL's default
   PreloadDLLs already loads Stracker at, not at the game's first DINPUT8
   load, which would need a hook or a proxy DLL (Claude's proposal; the user
   ran it in stracker-mo2-1 to -3 and did not object). This replaces the
   requirements draft's "defaults to Stracker's moment".
9. **The launch-setup test matrix waits** (the user's call, after
   overlay-off-1). Each part of AGENTS.md's "Verification" list returns
   when it can show something new about CMFM:
   - **Overlay on/off and the stability pass** (alt-tab, resize, cutscenes)
     return with the graphics hooks (API 1.2). Until then CMFM does nothing
     after PRE_MAIN. Under MO2 the overlay setting does not change what
     loads (overlay-off-1).
   - **Steam launch options** wait until a Steam-started game's load order
     matters to a CMFM feature.
   - **The manual install** waits for the release check.

   Done so far, MO2 through the CMFM launcher entry:
   - no force-load, overlay on and off;
   - SPL force-loaded (spl-mo2-1);
   - all of the user's Stracker plugins, compared with Stracker's Loader
     (stracker-all-1 and -2, stracker-baseline-1 and -2).
