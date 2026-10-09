# Design: AL's crash handler

Status 2026-10-09: implemented, tested outside the game (`crash_handler`
test: real crashes in child processes), and seen in the game in
`crash-mo2-1`: the report named the Stracker plugin auto_food_skill.dll,
on its own thread, correctly. That run also showed exit-time crashes can
come before AL's SHUTDOWN, which led to the withdrawal rule below, confirmed
in `crash-mo2-2` (tag v0.1.0-dev.11).
Requirements: `requirements-dlss-mod.md` §5; roadmap "CMFM Menu milestone",
moved forward because a loader hosting about 20 third-party DLLs needs it
early. Code: `src/core/crash/`.

## What it does

When the game crashes, AL writes `<logs>\crashes\crash-<date>-<time>-<pid>.txt`.
The report holds:

- the AL version and exe hash;
- the time, and the thread (marked when it is the game's main thread);
- the last phase AL dispatched, with what it means: starting, graphics
  setup, running or exiting;
- the exception code and name, and for an access violation whether it was
  a read, write or execute, at which address;
- `at`: the faulting module and offset, with its owner;
- `owner`: the plugin responsible. That is the faulting module's plugin, or
  when the fault is inside Windows or a driver, the first plugin on the
  stack;
- `thread_started_in`: the deepest frame outside Windows, which tells a
  plugin's own thread from a game thread;
- the stack, up to 48 frames, each with its owner, and the registers.

Owners are read from the module's path:

- `...\astera\plugins\<name>\...` is AL plugin `<name>`. That includes the
  DLLs a plugin ships, such as the DLSS mod's `sl.*` and `nvngx_*`.
- `...\nativePC\plugins\<file>` is a Stracker plugin.
- `astera.dll` and the game exe are recognised by their base address.
- System32, SysWOW64 and WinSxS are Windows; anything else is listed by
  name.

The paths come from `K32GetMappedFileNameW`. That needs no loader lock, and
under MO2 it gives the real mod folder, which keeps the same layout.

Unless the crash happened after SHUTDOWN, it also writes
`<logs>\crashes\last-crash.txt`. It is one sentence for the user: when the
crash happened, which plugin was involved, the exception and the report's
path. Exit-time crashes, such as `more_auto_shoutouts` reading freed memory
while the game shuts down, are reported but not announced. That covers
two cases:

- the crash comes after SHUTDOWN;
- the crash comes on another thread and the game still returns from WinMain
  afterwards.

The game tears down inside WinMain, so a plugin thread can crash there just
before SHUTDOWN (crash-mo2-1: 2 ms before). Windows takes a moment to end the
process, the main thread finishes first, and the exit code is 0. At
SHUTDOWN, AL moves the notice to `last-crash.exit.txt` and adds an
`afterwards=` line to the report. It waits up to 2 s for the crashing thread
to finish writing.

Minidumps are opt-in, in `<game>\astera\astera.ini`:

```ini
crash_minidump = small   ; off (default), small, or full (very large)
```

`dbghelp.dll` is loaded from System32 at install only when minidumps are
on, so `astera.dll` still imports only KERNEL32. The dump is written last,
next to the report, and the report's last line names it or says `failed`.
AL keeps the newest 20 reports and 20 dumps.

## How it stays out of the way

- **When it installs:** at WIN_MAIN, from `Host::dispatch`. The game's C
  runtime replaces any top-level filter set earlier and does not chain.
  AL's filter goes in front of the runtime's and remembers it.
- **It never handles a crash.** After writing, it calls the next filter and
  returns that filter's answer. The next filter is a later replacement, if
  one exists, otherwise the one AL replaced. Without either it returns
  "continue search", so Windows error reporting and WER LocalDumps behave as
  before.
- **When something replaces AL's filter:**
  - a vectored handler, registered last, looks at first-chance fatal
    exceptions only. It ignores debug events, C++ exceptions and AL's own
    phase observers, and never handles anything;
  - if another filter has replaced AL's, it puts AL's back in front and
    remembers the other one to call after it;
  - if that filter later calls back into AL's (a filter that kept AL's
    as its predecessor), AL goes on to the filter it replaced. Every
    filter in the chain still runs, once.
- **Safe while the process is failing:**
  - the crash path uses only static buffers: no heap, no locks, no
    loader-locked calls;
  - writing the report runs under SEH, so a fault there loses the report,
    not the chain;
  - after a stack overflow it skips the stack walk and the dump;
  - only the first crash in a process is reported.
- **Not covered:** `__fastfail` and other fail-fast exits never reach any
  handler; neither do a process killed from outside or a hang.

## The next-launch notice

`Host::load_all` reads and retires `last-crash.txt` (renamed to
`last-crash.seen.txt`). `Host::notices()` returns it once. The startup
runtime shows it with its existing visible warning, after PRE_MAIN:

```cpp
for (const auto& notice : g_runtime->host.notices()) visible_warning(notice);
```

That line is the only change outside `src/core/crash/` and the host. It is
the user's to add, since `src/core/startup/` is theirs.

## Tests

`crash_handler` (`tests/test_crash.cpp`, fixture `tests/crash_fixture.cpp`):

- **Pure:** owners for AL plugin, plugin-shipped, Stracker (any case),
  Windows, core, game and other paths; fatal codes; the astera.ini setting.
- **Child processes** crash for real with the handler installed:
  - a thread started by a DLL under `astera\plugins\crashy\` writes through
    null. The report names plugin `crashy` as both faulting module and
    thread origin, at FIRST_PRESENT. The previous filter still ran, and the
    notice names the plugin and is shown once;
  - Windows code (`KernelBase`) faults on a bad pointer passed by a DLL
    under `nativePC\plugins\`. A filter set after AL's does not chain.
    The report finds the Stracker plugin on the stack and marks the main
    thread, and the replacing filter still runs after AL's;
  - an exit-time crash with `crash_minidump = small` writes a dump and no
    notice;
  - a plugin thread crashes, the last filter stalls the way error reporting
    can, and the main thread reaches SHUTDOWN and exits 0. The report gains
    `afterwards=` and the notice is withdrawn.

  Each child ends in a last filter that terminates it, so Windows error
  reporting never runs during the tests.
