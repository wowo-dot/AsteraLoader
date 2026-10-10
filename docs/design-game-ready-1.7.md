# Design: AL_PHASE_GAME_READY (API 1.7)

Status 2026-10-10: implemented (2e93056) and confirmed in MHW: game-ready-1,
tag v0.1.0-dev.17. GAME_READY fired once on the frame thread 21.6 s after
start, when the title screen came up (the user saw the title's add-on
check popup at about 20 s). Roadmap "path to v1" step 3.

## What it means

The game has booted and its title screen has started: game data is loaded
and the game's own systems (the `s*` singletons, the GUI) are set up. It is
the point SKSE calls "data loaded". Plugins that read game state, show game
popups or start work that needs the game up wait for it instead of polling
from their own threads (plugin-needs-survey.md items 2 and 8). The DLSS
mod's requirements name "title screen or hub" (requirements-dlss-mod.md).

It is not "a save is loaded": per-save and area events belong to the MHW
SDK after v1.

## What the game does (static analysis)

IDA, private database `local/ida/input-20261010/`, exe 421810 (SHA-256
c2ebbbd2...). RVAs from the image base.

- MT Framework runs the game as **areas**. The area classes registered
  through MtDti include `aLogo`, `aLoader`, `aResident`, `aTitle`,
  `aCharacterMaking`, `aStage`, `aPlayable` and others. The title screen is
  `aTitle` (DTI at `exe+0x4c981a8`); its GUI is `uGUITitle`
  (`ui\title\title`).
- The area manager `sArea` (singleton at `exe+0x51c42b8`, derived
  `sMhArea`) keeps up to 8 active areas at `+0x8048` (count `+0x803C`) and
  the requested next area's DTI at `+0x8040`. Its update `exe+0x22610b0`
  walks the active areas: an area not yet started (`area+0x38 == 0`) is
  asked through vtable slot 5 whether it has finished loading; once it
  has, slot 6 starts it and the flag is set; every frame after, slot 7
  updates it.
- `aTitle`'s vtable is at `exe+0x2f108b0` (slot 4 returns its DTI). Its
  slot 6, **`exe+0x25cf70`**, is the start: it creates the title GUIs
  (`uGUITitle`, `uGUITitleDataSelect`, ...) and loads the title's ambient
  sound. It runs once per entry to the title: at boot, and after each
  return to the title.

## How AL uses it

- The frame layer hooks `exe+0x25cf70` through al.hooks ("Area:TitleStart",
  order -500), next to Main:Update and the exit site. Pattern, unique on
  the decoded image (sigscan `make`):
  `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 81 EC 90 00 00 00 48 8B F9 E8`.
- The detour calls the original, then only sets a flag. No plugin runs
  inside the game's area code.
- At the end of the frame in which the flag was set, after al.frame END,
  the frame layer dispatches GAME_READY on the game's frame thread with
  `al_phase_game_ready_t {size, source = AL_GAME_READY_TITLE, frame}`.
- Once per process: later title starts (a return to the title) do nothing.
  Never after EXITING (`Host::raise_game_ready`).
- Optional site: if it is refused, the frame layer and EXITING keep
  working; `game_ready_unavailable` is logged, and if a plugin subscribed
  to GAME_READY the user gets a warning with the reason. Unverified
  executables get no title site.
- The crash handler names crashes after GAME_READY "while the game was
  running".

## Tests

- `frame_layer_normal` / `_backup`: the fake game starts a stand-in title
  in frames 1 and 2; the test plugin gets GAME_READY once, after frame 1's
  END callbacks, on the frame thread, with frame 1 in the payload.
- `frame_layer_title_failure`: a refused title hook leaves frames armed,
  no warning without a subscriber, and a reason.
- `frame_layer_ready_rules`: once only, never after EXITING.
- `example_hello`: the hello example logs GAME_READY's payload.

## Open

- Not yet seen in game: no second GAME_READY after a return to the title
  (covered by the fake-game test). The core log's shutdown summary now
  says `game_ready fired=1 title_starts=N`, so a run with a return to the
  title shows it.
- A mod that skipped the title screen would leave GAME_READY silent. None
  is known for MHW (intro skips skip the logos, not the title). A fallback
  could watch for the first started area of any kind.
