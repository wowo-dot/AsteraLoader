# ABI freeze review (API 1.5)

2026-10-10. Roadmap "path to v1" step 4: before the first public release,
check the plugin ABI for anything that would be expensive to change once
plugins in the wild depend on it. Reviewed: `include/al/al_plugin.h` at
793d129 (API 1.5), and the host code that reads what plugins pass in
(`manifest.cpp`, `host.cpp`, `game_hooks/registry.cpp`, `graphics/hub.cpp`,
`frame/clock.cpp`). Not reviewed: `al.input`, still in progress; see the last
section.

Status 2026-10-10: items 1 to 5 are done (bc25279 and the header
comments after it), and `al.input` was checked against the last section.

## What the freeze means

From the first public release, every 1.x change is additive: new functions
and fields at the end of tables and structs (`size` grows), new phases,
events and services. Anything else needs API 2.0, which refuses every 1.x
plugin. `tests/abi_layout.h` already pins every offset and size, compiled as
C and as C++.

## Layout: clean

Every struct in the header was checked against the pinned offsets: none has
implicit padding, inside or at the end. Where alignment needs a gap, the
header names it (`reserved`, `reserved2`). So a field added later always
lands past the old `size`, and a `size` check can tell whether it is there.
Keep that rule for new structs and add each one to `abi_layout.h`.

## Fix before release

These are cheap now and impossible later.

1. **Size checks compare with today's `sizeof`.** `game_hooks/registry.cpp:101`
   refuses an `al_hook_t` whose `size` is below `sizeof(al_hook_t)`, and
   `host.cpp` (in `hooks_hook`) reads `name` only when `size >=
   sizeof(al_hook_t)`. Today both sizes are 56. Once al.hooks 1.1 adds a
   field, `sizeof` becomes larger and every plugin built for al.hooks 1.0 is
   refused. The fix: compare with the struct's size in the version that
   introduced it (56, a pinned constant like `manifest.cpp`'s
   `kInfoSize10`), and read each later field only when `size` covers it.
   The in-progress `al.input` host code has the same pattern
   (`key->size < sizeof(*key)` for `al_hotkey_t`); start it on a pinned
   1.0 size. The header should state the rule: AL accepts any `size` at
   least that of the version that introduced the struct.

2. **Reserve the plugin name `al`.** `publish_interface` only checks that a
   name starts with the plugin's own name and a dot, so a plugin named `al`
   may publish `al.*` names. AL's built-in services are matched first, so
   it cannot shadow those. But it can publish a name a later AL adds (say
   `al.input` on AL 1.5), and a plugin asking for that name on the older AL
   would get the impostor's table. The fix: add `al` to the reserved DLL
   names in `host.cpp`'s `load()`, next to `AsteraLoader` and `loader`.

3. **Say that any result other than `AL_OK` is a failure.** AL already
   treats it so (`al_plugin_init` returning anything but `AL_OK` disables
   the plugin). If the header says it, later versions can add error codes
   without breaking plugins that test `== AL_ERROR`. The guide already
   uses `!= AL_OK`. Header text only.

4. **Say that phase numbers are not firing order.** EXITING (7) fires
   before SHUTDOWN (6), and GAME_READY (5) will fire after FIRST_PRESENT.
   The header's top comment should say the numbers are identifiers and give
   the firing order once. Header text only.

5. **The header's first line** still says version 1.4, and its overview has
   no line for 1.5 (`al.frame`, EXITING).

Items 1 and 2 touch `host.cpp`, which `al.input` is changing now; do them
in one commit once `al.input` lands, with a test each (a 56-byte
`al_hook_t` accepted when the struct is larger; a plugin named `al`
refused). Items 3 to 5 are header comments.

## Frozen as they are

These are fine. They are listed so nobody "fixes" them later and breaks
1.x.

- **Array element structs have no `size`:** `al_dependency_t` (16 bytes)
  and `al_d3d12_hook_t` (24 bytes) are read as arrays, so their stride is
  fixed for all of 1.x. They never grow; anything new comes as a new
  function or struct.
- **`al_scanner_t.rip_target` takes no `self`.** It is a pure function.
- **`al.d3d12` subscriptions return a result, not a handle, and have no
  unsubscribe;** registration closes when PRE_MAIN ends. An unsubscribe
  could come later as a new function in al.d3d12 1.1.
- **Status reasons differ:** `al_d3d12_status_t.reason` and `owner` are
  NULL when there is none, `al_frame_status_t.reason` is `""`. Changing
  either breaks code written against it. For every new service, use `""`
  (never NULL), as `al.frame` does.
- **`AL_PHASE_COUNT` and `AL_D3D12_EVENT_COUNT`** are the counts as of the
  header a plugin was built with, and grow with new versions.
- **Unknown phase bits refuse the plugin** (`manifest.cpp`). A plugin can
  only set a bit AL does not know by being built for a newer API, which is
  refused anyway; the check catches garbage masks.
- **Newer headers need a newer AL.** A plugin declares `AL_API_VERSION`
  from the header it was built with, and an older AL refuses it with
  "Update Astera Loader". A plugin that wants to run on older ALs declares
  an older version and treats NULL from `get_interface` as "not there".
  `get_interface` does not depend on the declared version; only
  `data_folder` does (NULL for a plugin declaring 1.0).
- **What AL reads from plugins is bounded and tolerant:** a 1.0-sized info
  struct is accepted, name lists stop at 64 entries, strings at 256
  characters, and status structs are filled up to the caller's `size`.

## Additive later (no freeze impact)

Possible 1.x additions that need nothing now: an unsubscribe for
`al.d3d12` events, AL's own build version for plugins (the API reports
only the API version), another plugin's status and version, and
GAME_READY with its payload.

## al.input: check before it ships

`al.input` (1.6) becomes part of the frozen 1.x the day it is released,
so before then:

- size checks on a pinned 1.0 size (item 1 above);
- `reserved` fields must be zero (the in-progress hotkey check already
  refuses non-zero, which keeps them usable later);
- reasons and names never NULL; a handle of 0 means failure;
- callbacks run under AL's fault guard, as al.frame's do;
- every new struct pinned in `abi_layout.h`, with no implicit padding.
