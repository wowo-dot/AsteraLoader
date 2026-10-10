# CMFM decisions

Decisions taken with the user, newest last. Each one answers an open question
from `handoff-20261008.md` or `requirements-dlss-mod.md`.

## 2026-10-08

1. **Name.** One name, CMFM, with two parts: CMFM Core (loader, plugin API,
   hook coordination) and CMFM Menu (settings, status pages, popups). Users
   only ever install "CMFM". Superseded by 11.
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
10. **Graphics coordination is shared detour chains, not object
    substitution** (Claude's design after reading the DLSS mod's shipped
    code; the user asked for graphics coordination).
    - The mod keeps the game on DXGI's own objects and redirects their
      methods to Streamline. A CMFM hook placed beside its own would turn
      it off.
    - So CMFM owns one hook per method, and plugins register detours with a
      `next` pointer, in a declared order.
    - CMFM's own event layer runs first; a single presentation role guards
      factory and swap-chain methods; a shared internal-call region replaces
      the mod's bypass counter.
    - Details: `design-d3d12-1.3.md`.
11. **The loader becomes Astera Loader ("AL"); CMFM is its menu plugin**
    (the user's call, after the plugin-needs survey). Replaces item 1.
    - The loader is a product of its own and the stable ground everything
      targets: launcher, phases, Stracker compatibility, cmfm.d3d12,
      cmfm.hooks, crash handler. It should rarely change once 1.0 ships.
      New game knowledge goes into separate plugins (the deferred MHW SDK).
    - CMFM ("Configuration Menu For Mods") keeps its name for the in-game
      menu, a bundled first-party plugin of the loader.
    - The name is baked into the C ABI (exports, types, interface names,
      file and folder names), so the rename happens before 1.0, as one
      mechanical series with tests green after each step, plus one
      coordinated update of the DLSS mod.
    - The user's choices: the API prefix `al_`/`AL_` and interface names
      "al.x"; the files `AsteraLoader.exe` and
      `<game>\AsteraLoader\AsteraLoader.dll`, plugins in
      `<game>\AsteraLoader\plugins\<name>\`. A folder named `astera`
      (the first pass) looked like game content, Astera being the in-game
      hub, so the folder and files carry the full name.

## 2026-10-10

12. **v1 ships unsigned; code signing through SignPath Foundation later**
    (the user's call, after comparing costs).
    - Since 2024 no certificate, EV included, skips SmartScreen's prompt for
      new files; reputation builds per file over downloads. For a niche
      loader, paying $120 to $220 a year for a certificate or for Azure
      Artifact Signing would buy little.
    - MO2 users are likely to never see the prompt (MO2 downloads and
      unpacks archives itself); manual installs unpacked with Explorer
      would. The README says so.
    - Later: apply to SignPath Foundation (free for open-source projects)
      once the repository is public and builds come from CI. SignPath
      Foundation is then the publisher named in the signature.
13. **The loader stays at 0.x until the test matrix is done** (the user's
    call). The first public release is a 0.x version; 1.0.0 comes after
    the launch-setup test matrix (item 9) passes. The loader's version is
    separate from the plugin API's (1.6).
14. **Windows Defender: the README asks for an exclusion** (the user's call,
    after Defender quarantined the launcher in input-check-3 as
    `Behavior:Win32/DefenseEvasion.A!ml`). The launcher does what every
    launcher-style loader does: it starts the game paused and loads its DLL
    into it, which behaviour detection flags. AL is open source, so users
    can check what it does. The README explains this and how to add the
    exclusion. No false-positive report to Microsoft for now, and the launch
    method stays. A VirusTotal check of the release binaries shows what
    other antivirus engines make of it.
15. **Claude may upload AL's release binaries to VirusTotal** (the user's
    call, 2026-10-11). Until then only the user uploaded, and Claude
    looked results up by SHA-256: the rule dated from when NVIDIA's DLSS
    DLLs sat in the same folders. Now Claude can upload the four files of
    a staged release (`AsteraLoader.exe`, `AsteraLoader.dll`,
    `stracker_compat.dll`, `loader.dll`) itself. Everything else stays
    off external services: NVIDIA DLLs, other mods' binaries, game files,
    logs, crash dumps and dev builds.
