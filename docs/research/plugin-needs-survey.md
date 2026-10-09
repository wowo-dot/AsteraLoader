# What MHW plugins need: survey (2026-10-09)

The question was whether CMFM's API is shaped too much by the DLSS mod, and
what else to reverse and offer. To answer it from evidence, this survey
reads what existing MHW plugins and frameworks actually do. Read-only
research from published source, Nexus pages and the user's own projects.
No binaries were disassembled.

Sources:

- **SharpPluginLoader 2.0.0** (SPL, MIT, tag `e9ebb59`): its C# plugin API
  is a list of what MHW modders asked for.
- **The Stracker plugins in the user's profiles** (19 DLLs):
  - source found for QuestLoader, MonsterLoader (Strackeror), FextyMod
    (Fexty12573/CatMod), Unbloat (Strackeror), custom_name_color and
    more_auto_shoutouts (eigeen, Rust on mhw-toolkit);
  - no source for BetterInputs, DPSTickFix, the five kassent plugins
    (SpecialQuestUnlocker, UnlimitedChatMessage, PermanentSpecialArenaQuest,
    NoHomeChange, BetterDailyCatSkills), auto_food_skill and
    MatchMakingUnlocker. For these only the descriptions were read.
- **References:** LuaEngine, HealthNotes, eigeen's mhw-toolkit.
- **The user's own code:**
  - the G8F actor-routing plugin (`D:\mh world modding\base body\plugin`);
  - cpuopt (`D:\mh world modding\cpuopt`).

## Findings

1. **Quest lifecycle is the most wanted gameplay signal.** SPL raises
   Accept, Enter, Depart, Leave, Abandon, Cancel, Return, Complete and Fail
   from nine named hooks. Others get at it their own way:
   - more_auto_shoutouts polls `QUEST_BASE+0x38` every 50 ms on its own
     thread;
   - FextyMod acts on quest start;
   - mhw-toolkit lists the same nine functions;
   - going by their descriptions, NoHomeChange, PermanentSpecialArenaQuest
     and BetterDailyCatSkills need these points too.

   QuestLoader hooks the quest-availability checks, and SpecialQuestUnlocker
   probably hooks the same ones (no source to confirm).
2. **A per-frame tick on the game thread, instead of private threads:**
   - SPL's `OnUpdate` hooks `Main:Update`;
   - LuaEngine's per-frame hook (`MapClockLocal`) drives all of its
     callbacks;
   - more_auto_shoutouts (50 ms loop), FextyMod (10 ms loop), HealthNotes
     (2 s loop) and cpuopt (100 ms worker plus a 500 µs poller) run their own
     threads instead.

   Those threads read game memory with no synchronisation and never stop.
   more_auto_shoutouts' source confirms its exit crash: its worker keeps
   following player and quest pointers while the game frees them. CMFM's
   crash handler caught exactly that (crash-mo2-2), and auto_food_skill
   shows the same symptom (crash-mo2-1).
3. **"The game is exiting" must come before teardown.** CMFM's SHUTDOWN
   fires after WinMain returns, but the game frees its state inside WinMain.
   The exit crashes came 2 and 27 ms before SHUTDOWN. Plugins with threads
   need a signal while game memory is still valid.
4. **Calls into the game from the wrong thread.**
   - FextyMod calls `sMhGUI::DisplayPopup` from its loop thread;
   - more_auto_shoutouts calls the game's `SystemMessage` from a worker;
   - four plugins reach "show a message" four different ways.

   A main-thread task queue would make those calls safe.
5. **Chat commands conflict today.** more_auto_shoutouts (`!mas `) and
   LuaEngine (`luac:`) both read *and clear* the same chat buffer
   (`MESSAGE_BASE+0xC0`), so either can swallow the other's commands.
6. **Every plugin bundles its own hooking.**
   - MinHook in QuestLoader, MonsterLoader, FextyMod and custom_name_color;
     `minhook-sys` in the Rust plugins; SafetyHook in LuaEngine and SPL;
   - G8F writes hand-made inline detours at fixed RVAs;
   - Unbloat overwrites game code directly.

   Nothing orders two plugins on one function. The overlaps found are the
   quest checks (QuestLoader and probably SpecialQuestUnlocker), monster
   create (MonsterLoader, HealthNotes, LuaEngine, mhw-toolkit) and damage
   and hits (more_auto_shoutouts, LuaEngine, mhw-toolkit).
7. **Hard-coded addresses are widespread.** Player base `0x1450139A0→+0x50`,
   `QUEST_BASE`, the chat pointers and G8F's 7+ RVAs are all fixed numbers.
   SPL instead has **106 named signature records**, re-scanned per game
   revision. CMFM's cached scanner (API 1.1) is the right base for a shared,
   signature-verified registry of named game objects and functions.
8. **MT Framework type information is reusable.** SPL and G8F independently
   use the engine's `MtDti` class registry:
   - name at +0x8; size = (low 23 bits of +0x30) × 4; id = the game's CRC32
     of the name & 0x7FFFFFFF;
   - `MtObject` virtual function 4 returns the object's type (DTI).

   With that, a plugin can check "is this a uPlayer?" and find singletons
   by class name, which is more robust than addresses. SPL names its
   singletons by hooking `cSystem:Ctor`: sQuest, sEnemy, sPlayer, sMhGUI,
   sChat, sMhArea, sMhCamera, sMhKeyboard, sUserData, sMhMain, sMhRender,
   sMhResource and others.
9. **Not asked for by any current plugin:**
   - per-save plugin storage. SPL has `OnSelectSaveSlot`, but its `OnSave`
     is declared and never raised, and it has no per-save store. SKSE's
     co-save shows the value, but there is no MHW client yet;
   - network packets (SPL has them). Out of scope for CMFM Core, which
     must not change network behaviour;
   - G8F's resource-manager internals (acquire/release, cache key). Useful
     SDK facts for asset mods, not a core service.
10. **Loader expectations, already met:**
    - plugins scan and hook inside DllMain, so they need code unpacked at
      load. CMFM loads them at PRE_MAIN;
    - they read config files relative to the current directory. The
      launcher starts the game in its folder;
    - Rust plugins reach `loader::LOG` through a static FFI shim, which
      CMFM's `loader.dll` already exports.

## Ranked services

Each entry: who would use it, what reversing it needs, who builds it.
"Mechanics" means installing the hooks: the user's, per
`launcher-injection-owned-by-user`. API, registry and dispatch are
Claude's.

| # | Service | Evidence (would use it) | Reversing needed | Build |
|---|---|---|---|---|
| 1 | **Game frame tick** on the game thread, plus **"game exiting" before teardown** | SPL OnUpdate, LuaEngine, more_auto_shoutouts, FextyMod, HealthNotes, cpuopt; the exit crashes | the per-frame function (SPL `Main:Update`; LuaEngine `MapClockLocal`); where the main loop decides to exit | hook: user; events, ordering, timing: Claude |
| 2 | **Main-thread task queue** (run this on the game thread next frame) | FextyMod popup, more_auto_shoutouts SystemMessage | none beyond #1 | Claude (on #1's tick) |
| 3 | **Quest and area events**: accept/enter/depart/leave/abandon/cancel/return/complete/fail; area/stage change | SPL (9 hooks), more_auto_shoutouts, FextyMod, NoHomeChange, PermanentSpecialArenaQuest, BetterDailyCatSkills, LuaEngine scene check | verify SPL's `Quest:*` patterns and `sMhArea+0xD328` on exe 421810 | hooks: user; event API: Claude |
| 4 | **Shared hook service for game functions**: named targets, ordered detours with `next`, `begin/end_internal` | QuestLoader / SpecialQuestUnlocker overlap, monster create, damage/hit; every plugin's private MinHook | none (the target comes from the plugin or the registry) | the d3d12 chain installer extended to game code: user; `cmfm.hooks` API and registry: Claude |
| 5 | **Named game objects and functions registry, MtDti** (SDK) | player/quest/chat pointers in 5+ plugins; SPL's 106 records; G8F's RVAs | verify the patterns on 421810; MtDti layout; the singleton table | Claude (analysis, scanner-backed registry, SDK header) |
| 6 | **Chat commands and game messages**: a command router that does not swallow, system message and popup through the task queue | more_auto_shoutouts × LuaEngine conflict; 4 message routes | chat send / message buffer functions (mhw-toolkit names) | on #2–#4 |
| 7 | **Monster lifecycle and actions**: create, initialised, destroy, death, enrage, action | SPL (9 events), MonsterLoader, HealthNotes, LuaEngine | SPL `Monster:*` patterns | as #3 |
| 8 | **Input and hotkeys** (also needed by the Menu) | FextyMod, LuaEngine, SPL keybinds, BetterInputs | `sMhKeyboard`, pad state (SPL offsets) | roadmap item 12 |
| 9 | **Per-save plugin data** | none today (SKSE's lesson; SPL leaves it unwired) | save/load and slot selection (SPL `SaveData:SelectSlot`) | when a client exists |

The first step costs one game run: a pattern probe. A small CMFM plugin
scans SPL's named records and mhw-toolkit's signatures with `cmfm.scanner`
at PRE_MAIN, then logs which match exactly once on exe 421810 and where.
That gives a verified, named function list for #1, #3, #5 and #7 before
anything is hooked. It runs in the unpacked game, where on-disk scanning
cannot work because `.text` is encrypted. SPL's records are MIT; keep its
notice.

## A note on two of the user's plugins

- **Tic Rate Fix (DPSTickFix):** its Nexus page says it requires
  Performance Booster and Plugin Extender (mod 3473), and that the game
  crashes without it. That mod removes the game's CRC checks; the user
  deleted it from MO2 on 2026-10-09 as unused. Every CMFM run since then has
  loaded DPSTickFix without a crash, but those sessions were short.
- **Unbloat** writes game code directly.

If either trips an integrity check in a longer session, CMFM's crash
handler will now name the module. CMFM itself will not disable such
checks (AGENTS.md).
