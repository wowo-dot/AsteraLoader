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

Still open:

- **Entry slot** (open question 1). Decide from measurement: which delay
  import the game loads early enough, under MO2 with and without the Steam
  overlay. See `docs/research/entry-slot.md` once the runs exist.
