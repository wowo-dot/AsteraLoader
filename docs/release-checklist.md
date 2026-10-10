# Release checklist (v1)

2026-10-10. Roadmap "path to v1" step 4: what the first public release needs
beyond working code. Facts below are from the CMake install rules,
`tools/dev.py`, the Release build at 793d129 and the user's MO2 setup.

## The package today

`dev.py stage` installs this layout (CMake `install` rules):

```
AsteraLoader.exe
AsteraLoader\AsteraLoader.dll
AsteraLoader\plugins\stracker_compat\stracker_compat.dll
AsteraLoader\plugins\stracker_compat\loader.dll
licences\MinHook-BSD-2.txt
licences\SharpPluginLoader-MIT.txt
licences\StrackerLoader-MIT.txt
licences\StrackerLoader-log-BSD-2.txt
LICENSE
```

- No runtime to install: the CRT is linked statically.
  `AsteraLoader.dll` imports only KERNEL32 (`stage` enforces it), and
  `AsteraLoader.exe` only KERNEL32, USER32 and SHELL32.
- Probes and the hello example are in the separate `examples` component,
  not in the package.
- AL writes only inside its own folder: `AsteraLoader\logs` (core, plugin
  and crash logs), `AsteraLoader\data\<plugin>` and `AsteraLoader\cache`.
  Under MO2 that is AL's mod folder, not the game folder or MO2's
  overwrite. The one exception is Stracker's: when `loader-config.json`
  turns on `logfile` (off by default), the shim appends to `loader.log` in
  the game folder, as Stracker's Loader did.

## Gaps before v1

1. ~~**A README for players.**~~ Done: `README.md`, shipped in the package. It covers:
   - what AL is, in two lines, and that it replaces Stracker's Loader;
   - install with MO2 (below), manually, and through Steam launch options;
   - "remove Stracker's Loader" (its MO2 mod, or `dinput8.dll` and
     `loader.dll` from the game folder). With a leftover `loader.dll`,
     stracker_compat turns itself off and says why;
   - where logs and crash reports are, and what to send with a bug report;
   - uninstalling: disable the mod or delete the files; nothing else changes.
2. ~~**A release version.**~~ Done: `dev.py stage --release` (CMake option
   `AL_RELEASE`) builds 0.1.0 without `-dev`. Before: `project()` said 0.1.0, and every build was
   stamped `-dev` (`AL_BUILD_VERSION` in CMakeLists.txt, `project_version()`
   in dev.py). Needed: a release switch that drops `-dev`, and a version
   number. The loader's version is separate from the plugin API's (1.6).
   Decided: 0.x until the test matrix is done (decisions.md item 13).
3. ~~**Version info in the binaries.**~~ Done: `al_version_resource` gives the
   exe and the three DLLs one each (test `version_info`). Before: neither file had a Windows version
   resource, so Explorer's Properties tab and Windows' own crash reports
   show no product, version or origin. A small `.rc` for each:
   FileVersion, ProductName "Astera Loader", OriginalFilename,
   LegalCopyright.
4. ~~**A plugin author package:**~~ Done: the `sdk` install component, the
   plugin kit archive, which builds its example on its own (test
   `sdk_package`). It holds `include/al/al_plugin.h`,
   `examples/hello`, `docs/plugin-guide.md` and `LICENSE`, as a second
   archive or a folder in the repository's release.
5. ~~**Retire the probes** from the default build~~: done. `site_probe`
   and `input_probe` are no longer built (al.frame and al.input replaced
   them); `input_check` stays a test-only example.
6. **The ABI-freeze fixes** in `abi-freeze-review.md`.
7. **Unsigned binaries.** A launcher that starts a game suspended and loads
   a DLL into it is the kind of thing antivirus heuristics look at, and
   unsigned downloads get SmartScreen's "unknown publisher" prompt. Test
   with Windows Defender on (the default) and note the prompt in the README.
   Decided: v1 ships unsigned, signing through SignPath Foundation comes
   later (decisions.md item 12). Uploading builds to an antivirus vendor
   for review is the user's call.
   **Seen 2026-10-10: a blocker.** Windows Defender flagged the launcher as
   `Behavior:Win32/DefenseEvasion.A!ml` (severe) while it ran, killed it a
   second after it resumed the game, and quarantined it (input-check-3);
   fresh builds in the build folder too. The user's machine now has folder
   exclusions for the repository and the MO2 dev mod. Players with
   Defender's defaults would lose the launcher. Decided (decisions.md item
   14): the README explains it and asks for a Defender exclusion; a
   VirusTotal check of the release binaries.
   VirusTotal, build 1b54c64 (2026-10-10, uploaded by the user):
   - `AsteraLoader.exe` (cc238433...): 4/71. Arctic Wolf "Unsafe", Bkav Pro
     `W32.Malware.1491A66`, CrowdStrike Falcon "malicious, confidence 60%",
     SecureAge "Malicious". Microsoft, Kaspersky, Malwarebytes, McAfee,
     Sophos, Avast and Google clean.
   - `AsteraLoader.dll` (176132fa...): 1/71, Cynet (machine learning).
   - `loader.dll`, the Stracker shim (25c7eaea...): 1/71, Kaspersky
     `VHO:Trojan.Win32.Agent.gen`, a generic heuristic.
   - The package zip (ce069c6f...): 2/67, Kaspersky (from `loader.dll`)
     and Bkav Pro. `stracker_compat.dll` was not uploaded on its own.
   All are heuristic or machine-learning verdicts, typical for a new
   unsigned loader. Repeat for the release build (new hashes).
   VirusTotal, release 0.1.0 candidate d50de4c (2026-10-10, with version
   information and the icon):
   - `AsteraLoader.exe` (1386cdec...): 1/57, Trapmine "moderate ML score"
     (57 engines had reported). Arctic Wolf, Bkav, CrowdStrike now clean.
   - `AsteraLoader.dll` (cde93ff5...): 0/71.
   - `stracker_compat.dll` (8e444796...): 1/71, Cynet (machine learning).
   - `loader.dll` (71ca9d73...): 0/71; Kaspersky clean.
   Every commit changes every binary's hash (the commit is built in), so
   the final check is on the build that gets tagged.

## Install instructions to verify

**MO2** (as the user's own setup does):

1. Install the archive as a mod ("Install a new mod from an archive").
   MO2 may say the archive has no data folder; the mod root is the game
   folder for MHW, so install it as it is.
2. Add an executable: binary `<mods>\Astera Loader\AsteraLoader.exe`,
   argument `"<game folder>\MonsterHunterWorld.exe"`, start in the game
   folder.
3. Start the game with that entry from now on.

**Manual:** copy `AsteraLoader.exe` and the `AsteraLoader` folder into the
game folder, and start `AsteraLoader.exe`.

**Steam:** launch options `"<path>\AsteraLoader.exe" %command%`.

## Clean install tests

From the release archive, not the dev install. The user runs every launch.

- [ ] **A fresh MO2 instance** (new portable folder, MHW, no other mods):
      install from the archive, add the entry, start. Expect: core log,
      stracker_compat active with no plugins, no warning popup, Exit Game
      returns cleanly.
- [ ] **The same with plugins:** the DLSS mod and the user's Stracker plugins.
- [ ] **A leftover Stracker install** (its MO2 mod enabled, or the files in
      the game folder): stracker_compat turns itself off, the user is told,
      the game runs.
- [ ] **A manual install** into the game folder, then removed (the user's
      test; the game folder is otherwise read-only for this project).
- [ ] **Steam launch options** with `%command%`.
- [ ] **SPL force-loaded** alongside.
- [ ] **Paths:** a game folder with spaces; a Windows user name with
      non-ASCII characters (logs redact the profile path).
- [ ] **Steam overlay** on and off.
- [ ] **Upgrade** over the previous build: the caches keyed by exe hash
      still work, the logs are kept or pruned as designed.
- [ ] **Uninstall:** disable the mod, start the plain entry; the game runs,
      and nothing of AL is left in the game folder.
- [ ] **Stability** (decisions.md item 9): loading screens, cutscenes,
      menus, alt-tab, resize, full screen, with frame generation on.

## Cutting the release

1. `al.input` and GAME_READY are in, tested in game, and the docs cover
   them (plugin guide, header overview).
2. The ABI-freeze fixes are in; every test passes.
3. Stage from a clean tree: `dev.py stage --release` refuses a dirty tree,
   builds in its own folder, runs every test and writes
   `local/stage/release-<version>/AsteraLoader-<version>.zip` and
   `AsteraLoader-plugin-kit-<version>.zip`. Check them on VirusTotal.
4. Run the clean install tests above from that archive.
5. Tag the tested commit (annotated `vX.Y.Z`).
6. Write release notes: what works; known issues. One already: some
   Stracker plugins (auto_food_skill, more_auto_shoutouts) crash while the
   game exits. AL names them in its crash report and shows no notice for
   it, since the game was closing anyway.
7. Pushing and publishing are the user's to do. The logo's images are in
   `assets/logo` (made by `tools/make_logo.py`): `social-preview-1280x640.png`
   for the GitHub repository's social preview (Settings, General) and
   `nexus-cover-1920x1080.png` plus `astera-loader-512.png` for the Nexus page.
