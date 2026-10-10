# Handoff: the DLSS mod with SharpPluginLoader (2026-10-11)

For the DLSS / Frame Generation mod (`D:\mh world modding\dlssupdate`, 0.15.0).
Astera Loader 0.1.0 is tagged on 7cf2e6c.

## What changed in Astera Loader

Before 7cf2e6c, `al.hooks` refused a join when another tool had hooked the
function over AL's patch. SharpPluginLoader (SPL) 2.0.0, force-loaded under
MO2, hooks Main:Update after AL's frame layer. So the mod's
`mhw_dlss Main:Update` layer was refused ("the bytes at the address do not
match the pattern ..."), its frame stage failed, and frame generation stayed
off (spl-release-3).

From 7cf2e6c on, the join goes through. AL checks the pattern against the
bytes it read before hooking the function, and the plugin log says so:

```
INFO al.hooks: mhw_dlss Main:Update joined; another tool has rewritten this function's entry since Astera Loader hooked it, so the pattern was checked against the bytes from before
INFO al.hooks: hooked mhw_dlss Main:Update (handle 6, active)
```

## What happens then (spl-release-4)

With SPL force-loaded and `frame_generation = 2x`, frame generation started.
About 12 s later, on the title screen, the game crashed (0xc0000005, reading
0x0) inside SPL's `winmm.dll`. The thread had been started by
`sl.dlss_g.dll`, and the stack shows two `winmm.dll` frames above seven
`sl.dlss_g.dll` frames. AL's crash report blames mhw_dlss, as the first
plugin on the stack. SPL's log shows its renderer re-initialised after a
ResizeBuffers 5 s before the crash, and no errors.

The mod's own research predicted this (`docs/research/mod-loaders.md`,
"Implications"): with frame generation, SPL's Present hook meets
Streamline's proxy. Why SPL's code faults is SPL's business; this handoff
does not go into its internals.

Evidence: `D:\mh world modding\AsteraLoader\local\runs\spl-release-4\`
(crash report, core and plugin logs, SPL log, module list).

## What the mod should do

- **Detect SPL** before frame generation starts (at DEVICE_CREATED, or when
  FG would be enabled). For example: a `winmm.dll` loaded from outside the
  system directory, or SPL's `SharpPluginLoader.Bootstrapper.dll` in the
  module list (it loads before device creation; seen in spl-release-3).
  Decide in the mod which test is robust.
- **When SPL is there, keep frame generation off** and leave the rest of the
  mod running.
- **Say so visibly.** `set_status(self, AL_STATUS_DEGRADED, "frame
  generation off: SharpPluginLoader is loaded ...")`, so that AL shows its
  start-up warning, plus a log line.
- **Optional override** in `mhw_dlss.ini` (for example
  `frame_generation_with_spl = allow`) for testing once SPL changes.

Not yet tested: SPL with the mod and `frame_generation = off`. Astera
Loader's release notes recommend that setup, and run spl-release-5 checks it.
