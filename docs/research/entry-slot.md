# CMFM entry slot (2026-10-08)

How CMFM gets into the game process early enough. Measured facts come from
run `entry-slot-1` (read-only watcher, `tools/modwatch.py`); everything else
is labelled.

**Decision (2026-10-08): the launcher**, launcher-only in v1. See
`../decisions.md` item 6.

## Run entry-slot-1 (confirmed)

MO2 profile `barebonesdlss` (DLSS mod as winmm.dll, Stracker's Loader), MO2
executable without force-load, Steam overlay on, exe c2ebbbd2…. Times are ms
after process creation, from each loader entry's LoadTime.

| ms | Module | How it was loaded |
|---:|---|---|
| 13 | usvfs_x64.dll | MO2 injects it into the suspended process |
| 293 | steamclient64.dll | dynamic; SteamStub's ownership check, before the game's own code |
| 294 | WINMM, IPHLPAPI (System32) | static imports of steamclient64 |
| 586 | winmm.dll (MO2 copy) | the game's delay-load, redirected by usvfs; a second module beside System32 WINMM |
| 586 | d3d12.dll | static import of the DLSS mod's winmm.dll, not the game |
| 602 | steam_api64.dll | dynamic |
| 670 | dxgi.dll | dynamic |
| 675 | gameoverlayrenderer64.dll | dynamic, after steam_api64 (MO2 launch: not injected at process start) |
| 949–1043 | dxcore, D3D12Core, nvldumdx, nvwgf2umx | **device creation** |
| 1994 | sl.interposer.dll | DLSS mod's Streamline |
| 4732 | dinput8.dll (Stracker) | then loader.dll at 4735 and its plugins |
| 5468 | XINPUT9_1_0 | after device |
| 6589 | WININET | after device |
| 6991 | d3d11 | after device |
| — | MFPlat, MFReadWrite | not loaded by the title screen |

Consequences:
- Of the non-KnownDLL delay imports, only WINMM is called before device
  creation. XINPUT9_1_0, WININET, d3d11, DINPUT8, MFPlat and MFReadWrite are
  too late. The game's own first IPHLPAPI and d3d12 calls are not visible in
  the module list (unmeasured).
- Under MO2 only Win32 LoadLibrary calls are redirected. Static imports
  (steamclient64's WINMM) resolve to System32.
- MO2 already starts this exe suspended and injects a DLL (usvfs) before
  SteamStub runs. SteamStub and the game tolerate that.

## Proxy slot vs launcher (the SKSE route)

Proxy slot: CMFM ships as a game-folder DLL the game imports.
- winmm.dll is the only measured slot before the device, and SPL and the DLSS
  mod's standalone mode already use it.
- Every other proxy name is unmeasured (IPHLPAPI, d3d12) or another tool's
  slot (dxgi.dll is ReShade's).

Launcher: CMFM ships `cmfm_launcher.exe`, which starts the game suspended,
loads `cmfm.dll` into it, then resumes it. This is what SKSE does for Skyrim
and what MO2 itself does for usvfs and force-load.
- Earliest possible load: before SteamStub, before CRT init, before any game
  import. Later moments (post-unpack `pre_main`, WinMain, device, swap chain)
  become phases CMFM dispatches. The post-unpack moment needs SPL-style
  detection (its source shows how, MIT).
- No proxy DLL in the game folder, so no slot collision with SPL, the DLSS
  mod's standalone winmm.dll or ReShade.
- MO2: the launcher is added as an MO2 executable, like skse64_loader.exe.
  usvfs follows child processes, so the game still sees the virtual folder.
  No force-load. (Hypothesis until tested: usvfs's CreateProcess hook honours
  a caller's CREATE_SUSPENDED.)
- Manual install: the user starts the launcher instead of the exe.
- Steam's Play button does not run the launcher. Options: Steam launch
  options `"<path>\cmfm_launcher.exe" %command%` (unverified on Windows), or a
  desktop shortcut.
- CMFM could also preload other DLLs early, like MO2 force-load and SPL's
  PreloadDLLs. That might fix SPL under MO2 without force-load (hypothesis).
- Costs:
  - users must start the game through the launcher;
  - injectors draw more antivirus false positives than proxy DLLs;
  - the launcher writes into the game process. That is its job. The
    measurement tools stay read-only (AGENTS.md).
