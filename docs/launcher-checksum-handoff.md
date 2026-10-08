# Launcher checksum change

The launcher no longer hashes the game executable before `CreateProcessW`.
It records `exe_sha256` after `game_resumed`, while the game runs normally,
then continues waiting for the game's exit code. A missing checksum is a log
warning. The diagnostic helper catches exceptions as well, so a checksum or
logging failure cannot end the launcher while the game is still running.

The small CMFM DLL checksum and DLL validation remain before child creation.
The loading mechanism, `StartupRequest`, `cmfm_bootstrap` signature and return
codes are unchanged. No core source is edited by this change.

The core still hashes the executable during bootstrap. Claude's separate
change must move that diagnostic work off the paused startup path; this
launcher change alone does not remove the measured 229 ms bootstrap delay.
The core startup phases and public C plugin API also remain Claude's work in
the split shown by the user.

Verification uses the existing stand-in suite. The successful-launch test
checks checksum ordering and compares the logged SHA-256 to the launched file.
An additional fault DLL holds a read handle denying new reads until the
stand-in exits, forcing a real checksum read failure after resume. The
stand-in stays alive for 500 ms; the launcher must log the warning, wait, and
return its exit code 37. This fixture is never packaged or installed.

Actual timing gains still require a user-launched MO2 run. The previous
tested tag `v0.1.0-dev.1` remains the baseline.

Release builds with warnings as errors, and all eight CTest cases passed.
The new read-denial case produced its warning after `game_resumed` and returned
37 after the stand-in closed. The installed launcher will be updated separately;
the previously tested core DLL is retained for this change.
