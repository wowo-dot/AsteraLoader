# Clean game image: AL's check, 2026-10-11

The CMFM session found that the decoded image every project used,
`ai\glavenus\MonsterHunterWorld_decrypted.exe` (SHA-256 `d7861173…884c`),
was dumped from a running game with mods loaded. Its code differs from the
original at 29 places. It made a clean image from an unmodded game:
`CMFM\local\runs\20261011-clean-dump\dump\image.exe`, SHA-256
`a78ea69b0566414a42f16d0f8ff7d729e095041e2ae76a34cc02dbe0939f1d80`. The
details, with the 29 places, are in CMFM's
`docs/handoff-clean-image-20261011.md`.

## What AL checked

1. **References.** Only AGENTS.md named the old image; it now points at
   the clean one. Dated design docs that say "the decoded image" keep that
   wording: none of their results touch the 29 places.
2. **`sigscan.py audit <image> src probes`** on both images: identical
   output. 7 of 8 literals match once, at the same RVAs:
   - `kVersionPattern` 0x18d4d1c;
   - `kFramePattern` and `kUpdatePattern` 0x1af2b79;
   - `kTitlePattern` 0x25cf70;
   - `kKeyboardPattern` 0x22e74f0;
   - `kMousePattern` 0x22e9270;
   - `kPadPattern` 0x22a1280.

   `kExitPattern` matches 3 times (0xa410f0, 0x10e1ca0, 0x22e5c60), as
   before. It is checked at its pinned address only.
3. **The 29 places.** None of the 64 RVA-like numbers in AL's tracked files
   lies within 0x200 bytes of a place. Within 0x10000, two do: 0x23a4e2e
   and 0x23a7f50, both 25 KB or more past the place at 0x239ea20. They are
   DLSS-mod sites that AL's docs mention, not AL's. The first 64 bytes at
   each AL site are the same in both images:
   - the pattern matches above;
   - Main:Update 0x1af2b60 and Game:Exiting 0x22e5c60;
   - the startup and phase sites 0x2742a48, 0x2743690, 0x27428d4,
     0x27429d5 and 0x13a58d0.
4. **IDA.** `local\ida\input-20261010\decrypted.i64` was built from the
   old image. AL made its patterns with `sigscan.py` on the image file, not
   from the database. Searched in the database: within 0x200 bytes of each
   place, and in the functions that contain them, there is nothing but
   IDA's own output:
   - switch-table labels (`def_…`, `jpt_…`);
   - `jumptable … case` comments;
   - one argument name (`lpCriticalSection`).

   No names or comments from AL's work. So it was not rebuilt. A copy of
   the clean image is ready for the next database at
   `local\ida\clean-20261011\image.exe`.

Result: nothing in AL was affected. The 0.1.0 release needs no change.
