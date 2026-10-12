# CMFM's Start menu values through al.patch (2026-10-12)

For the CMFM session (`D:\mh world modding\CMFM`; read-only from AL). It
answers CMFM's `docs/al-requests.md` item 1. AL side: API 1.8, "al.patch"
1.0, commit aed4647 (docs) on top of 87b010f (code). Tested outside the
game (`test_patch`, `test_patch_writer`, `crash_handler`, `patch_native`),
not yet in it: CMFM's two values are the first in-game client. Contract:
`include/al/al_plugin.h`; design: `docs/design-patch-1.8.md`; how to use it:
`docs/plugin-guide.md`, "al.patch: byte changes".

## What differs from the request

The user chose the general version (2026-10-12): one service for code,
data and calls, sharing one owner per byte with al.hooks. For CMFM's case:

| Request | al.patch 1.0 |
|---|---|
| `expected`, `replacement` as `uint64_t` | pointers to `length` bytes; `kind = AL_PATCH_DATA`, `length = 8` |
| `address` | the same; or a `pattern` (not needed here) |
| `restore(handle)` | `set_enabled(handle, 0)`; `1` writes it again |
| AL puts back at shutdown | yes: after every plugin's own SHUTDOWN, DATA changes still active go back |
| 8-byte aligned values atomically | yes: a single compare-and-swap, read-only page protection restored |
| refused if another plugin holds it, naming it | yes, and the same for hooks over it |

`apply` copies the bytes it is given, so the old and new values may live
on the stack. The handle is 0 on failure, with the reason in CMFM's log.

## CMFM's side

1. Copy the 1.8 header into `third_party/al/al_plugin.h` and note the AL
   commit in `third_party/al/README.md` (a tag follows the in-game run:
   `v0.2.0-dev.N`, then `v0.2.0`).
2. At WIN_MAIN, as today, after CMFM's own checks (exe hash, the table's
   contents):

```c
const al_patches_t* patches =
    (const al_patches_t*)api->get_interface(self, AL_PATCH_INTERFACE, AL_PATCH_VERSION);
if (!patches) { /* AL older than 1.8: hide the Start menu entry and say why */ }

void* const old_items = *(void* const*)slot;  /* the System tab's table (RVA 0x3FC0E40) */
void* const new_items = cmfm_items;            /* CMFM's copy, in cmfm.dll */
al_patch_t change = {sizeof(al_patch_t)};
change.kind = AL_PATCH_DATA;
change.name = "Start menu: System tab items";  /* the second: "...in a quest" */
change.address = (void*)slot;                  /* exe + 0x34818B8, then exe + 0x3481868 */
change.length = 8;
change.expected = &old_items;
change.replacement = &new_items;
const uint32_t handle = patches->apply(self, &change);
if (!handle) { /* CMFM's log has the reason: hide the entry */ }
```

3. Drop the read-only path of `src/native/data_patch.cpp`
   (`VirtualProtect`). Putting the values back at SHUTDOWN is optional now;
   if CMFM does, `set_enabled(handle, 0)` before AL's own put-back.
4. CMFM's `.data` swaps (the Options pages' row tables) may move to
   al.patch too, for ownership and crash reports. AL writes writable data
   without a protection change. That is CMFM's choice.

## What the run shows

In AL's core log at SHUTDOWN, one line per value:

```
patch target=exe+0x34818b8 name=Start menu: System tab items kind=data length=8 owner=cmfm state=put_back_at_shutdown
```

(`state=off` if CMFM switched it off itself.) CMFM's plugin log has
`al.patch: "Start menu: System tab items" (handle N, written)`. A crash
report would list both values and their owner under `changes (al.patch):`.

## Order

1. AL: a dev build of 0.2.0 staged and installed in MO2 (the AL session).
2. CMFM: the change above, built against the 1.8 header (the CMFM
   session).
3. The user's run with both: the Start menu entry works, the core log
   shows the two lines. Then AL tags `v0.2.0-dev.N`, and 0.2.0 follows the
   release checklist, with CMFM's first release after it.
