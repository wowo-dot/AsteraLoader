"""CMFM development helper: build, test, stage, install into MO2, collect runs.

Run from anywhere with Python 3.10+:  python -B tools/dev.py <command> ...

  build                     configure (records the git commit) and build Release
  test                      run CTest
  check                     build, then test
  stage                     build, then install the CMFM package and the examples into
                            local/stage/<version>-<commit>[-dirty] and
                            local/stage/examples-<commit>[-dirty]; print hashes and
                            check that the DLLs import only KERNEL32
  install-mo2 RUN [--stage COMMIT]
                            back up the installed CMFM and example mods into
                            local/runs/RUN/installed-backup, install the staged
                            build (HEAD's, or an earlier COMMIT's), write
                            RUN/manifest.json and RUN/logs-before.txt
  baseline RUN              write RUN/manifest.json for a run without CMFM, started
                            from MO2's plain game entry; installs nothing
  watch RUN [--launch N]    run tools/modwatch.py (read-only) into RUN or RUN/launch-N
  collect RUN [--launch N]  copy the new CMFM logs and the scan cache into RUN or
                            RUN/launch-N and print the lines that matter

Machine-local paths (MO2, the game) live in local/dev.json, which is not
committed; the first run writes a template to fill in. Nothing here edits
MO2's own configuration or anything in the game folder.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCAL = ROOT / "local"
SETTINGS = LOCAL / "dev.json"
TEMPLATE = {
    "build_dir": "local/build",
    "game_exe": "<game folder>/MonsterHunterWorld.exe",
    "mo2_ini": "<MO2 folder>/ModOrganizer.ini",
    "mo2_mods": "<MO2 data folder>/mods",
    "mo2_profiles": "<MO2 data folder>/profiles",
    "cmfm_mod": "CMFM (dev)",
    "examples_mod": "CMFM hello plugin (test)",
    "launcher_title": "CMFM Launcher",
    "game_title": "Monster Hunter: World",
}


def fail(message: str) -> None:
    print(f"dev.py: {message}", file=sys.stderr)
    sys.exit(1)


def settings() -> dict:
    if not SETTINGS.exists():
        LOCAL.mkdir(exist_ok=True)
        SETTINGS.write_text(json.dumps(TEMPLATE, indent=2) + "\n", encoding="utf-8")
        fail(f"wrote a template to {SETTINGS}; fill in the paths and run again")
    values = json.loads(SETTINGS.read_text(encoding="utf-8"))
    # Paths ("<...>" in the template) must be filled in; names have defaults.
    missing = [key for key, value in TEMPLATE.items() if key not in values and value.startswith("<")]
    if missing:
        fail(f"{SETTINGS} lacks {', '.join(missing)}")
    return {**TEMPLATE, **values}


def run(command: list[str], **options) -> subprocess.CompletedProcess:
    print("> " + " ".join(f'"{part}"' if " " in part else part for part in command), flush=True)
    return subprocess.run(command, cwd=ROOT, check=True, **options)


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True, text=True).stdout.strip()


def commit_label() -> str:
    label = git("rev-parse", "--short=7", "HEAD")
    return label + ("-dirty" if git("status", "--porcelain") else "")


def project_version() -> str:
    found = re.search(r"project\(cmfm VERSION ([0-9.]+)", (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))
    return (found.group(1) if found else "0.0.0") + "-dev"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def imports(path: Path) -> list[str]:
    """The DLL names in a PE file's import table (read from the file)."""
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    optional = pe + 24
    sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    table = [struct.unpack_from("<IIII", data, optional + optional_size + 40 * i + 8) for i in range(sections)]

    def offset(rva: int) -> int | None:
        for virtual_size, address, raw_size, raw in table:
            if address <= rva < address + max(virtual_size, raw_size):
                return raw + rva - address
        return None

    names = []
    at = offset(struct.unpack_from("<I", data, optional + 112 + 8)[0])
    while at is not None and struct.unpack_from("<I", data, at + 12)[0]:
        name = offset(struct.unpack_from("<I", data, at + 12)[0])
        names.append(data[name:data.index(b"\0", name)].decode())
        at += 20
    return names


def game_running() -> bool:
    output = subprocess.run(["tasklist", "/FI", "IMAGENAME eq MonsterHunterWorld.exe", "/NH"],
                            capture_output=True, text=True).stdout
    return "MonsterHunterWorld.exe" in output


def stage_dirs(label: str | None = None) -> tuple[Path, Path]:
    label = label or commit_label()
    return LOCAL / "stage" / f"{project_version()}-{label}", LOCAL / "stage" / f"examples-{label}"


def files_under(folder: Path, skip: tuple[str, ...] = ()) -> list[Path]:
    return sorted(p for p in folder.rglob("*") if p.is_file() and not any(part in skip for part in p.relative_to(folder).parts))


# Commands ---------------------------------------------------------------

def build(_: argparse.Namespace) -> None:
    build_dir = settings()["build_dir"]
    run(["cmake", "-S", ".", "-B", build_dir, "-A", "x64"], stdout=subprocess.DEVNULL)
    # MSBuild prints every project; quiet keeps warnings and errors only.
    run(["cmake", "--build", build_dir, "--config", "Release", "--parallel", "--", "/nologo", "/v:quiet"])


def test(_: argparse.Namespace) -> None:
    run(["ctest", "--test-dir", settings()["build_dir"], "-C", "Release", "--output-on-failure"])


def check(args: argparse.Namespace) -> None:
    build(args)
    test(args)


def stage(args: argparse.Namespace) -> None:
    # Build first: the binaries record the commit at configure time, and the
    # stage folder is named after HEAD; both must agree.
    build(args)
    build_dir = settings()["build_dir"]
    package, examples = stage_dirs()
    if commit_label().endswith("-dirty"):
        print("warning: staging uncommitted changes; the label says -dirty")
    for folder in (package, examples):
        shutil.rmtree(folder, ignore_errors=True)
    run(["cmake", "--install", build_dir, "--config", "Release", "--prefix", str(package)], stdout=subprocess.DEVNULL)
    run(["cmake", "--install", build_dir, "--config", "Release", "--component", "examples", "--prefix", str(examples)],
        stdout=subprocess.DEVNULL)
    problems = []
    for folder in (package, examples):
        print(f"\n{folder.relative_to(ROOT)}")
        for path in files_under(folder):
            line = f"  {sha256(path)}  {path.relative_to(folder).as_posix()}"
            if path.suffix.lower() in (".dll", ".exe"):
                names = imports(path)
                line += f"  imports {', '.join(names)}"
                if path.suffix.lower() == ".dll" and [n.upper() for n in names] != ["KERNEL32.DLL"]:
                    problems.append(path.name)
            print(line)
    if problems:
        fail("DLLs importing more than KERNEL32: " + ", ".join(problems))


def selected_profile(mo2_ini: Path) -> str:
    found = re.search(r"^selected_profile=@ByteArray\((.*)\)\s*$", mo2_ini.read_text(encoding="utf-8", errors="replace"),
                      re.MULTILINE)
    return found.group(1) if found else ""


def enabled_mods(profile_folder: Path) -> list[str]:
    lines = (profile_folder / "modlist.txt").read_text(encoding="utf-8", errors="replace").splitlines()
    return [line[1:] for line in lines if line.startswith("+")]


def forced_libraries(profile_folder: Path, title: str) -> list[str] | None:
    """The executable's enabled force-load entries as "process: library", or
    None when force-load is off for it."""
    text = (profile_folder / "settings.ini").read_text(encoding="utf-8", errors="replace")
    prefix = title.replace(" ", "%20").replace(":", "%3A") + "\\"
    values = {}
    for line in text.splitlines():
        key, _, value = line.strip().partition("=")
        if key.startswith(prefix):
            values[key[len(prefix):]] = value
    if values.get("enabled") != "true":
        return None
    entries = []
    for index in range(1, int(values.get("size", "0") or 0) + 1):
        if values.get(f"{index}\\enabled") == "true":
            process = values.get(f"{index}\\process", "")
            library = values.get(f"{index}\\library", "").replace("\\\\", "\\")
            entries.append(f"{process}: {library}")
    return entries


def install_mo2(args: argparse.Namespace) -> None:
    config = settings()
    if game_running():
        fail("the game is running; close it first")
    label = args.stage or commit_label()
    package, examples = stage_dirs(label)
    if not package.exists() or not examples.exists():
        fail(f"nothing staged for {label}; run 'stage' first")
    run_folder = LOCAL / "runs" / args.run
    if (run_folder / "installed-backup").exists():
        fail(f"{run_folder} already has an installed-backup; pick a new run name")
    mods = Path(config["mo2_mods"])
    targets = {mods / config["cmfm_mod"]: package, mods / config["examples_mod"]: examples}

    # Back up what is installed now, except CMFM's own logs, cache and data.
    for mod in targets:
        for path in files_under(mod, skip=("logs", "cache", "data")):
            backup = run_folder / "installed-backup" / mod.name / path.relative_to(mod)
            backup.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, backup)

    installed = {}
    for mod, source in targets.items():
        extras = {p.relative_to(mod) for p in files_under(mod, skip=("logs", "cache", "data"))} - \
                 {p.relative_to(source) for p in files_under(source)} - {Path("meta.ini")}
        for path in files_under(source):
            target = mod / path.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            installed[f"{mod.name}/{path.relative_to(source).as_posix()}"] = sha256(target)
        (mod / "meta.ini").write_text(f"[General]\r\nversion={project_version()}\r\ncomments=CMFM local dev build "
                                      f"{label}, installed by tools/dev.py for run {args.run}\r\n",
                                      encoding="utf-8", newline="")
        for extra in sorted(extras):
            print(f"note: {mod.name}/{extra.as_posix()} is installed but not part of this build")

    profile = selected_profile(Path(config["mo2_ini"]))
    profile_folder = Path(config["mo2_profiles"]) / profile
    forced = forced_libraries(profile_folder, config["launcher_title"])
    logs = mods / config["cmfm_mod"] / "cmfm" / "logs"
    run_folder.mkdir(parents=True, exist_ok=True)
    (run_folder / "logs-before.txt").write_text(
        "\n".join(sorted(p.relative_to(logs).as_posix() for p in logs.rglob("*") if p.is_file())) + "\n"
        if logs.exists() else "", encoding="utf-8")
    manifest = {
        "run": args.run,
        "date": datetime.date.today().isoformat(),
        "source_commit": label,
        "cmfm_version": project_version(),
        "exe_sha256": sha256(Path(config["game_exe"])),
        "installed": installed,
        "mo2_profile": profile,
        "mo2_enabled_mods": enabled_mods(profile_folder),
        "mo2_executable": config["launcher_title"],
        "force_load": forced is not None,
        "force_loaded_libraries": forced or [],
        "steam_overlay": "unconfirmed",
        "sealed": False,
    }
    (run_folder / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                              encoding="utf-8")
    print(f"\ninstalled {label} for run {args.run}; profile {profile}, "
          f"{len(manifest['mo2_enabled_mods'])} mods enabled, force-load {manifest['force_load']}")
    print(f"backup and manifest: {run_folder.relative_to(ROOT)}")


def baseline(args: argparse.Namespace) -> None:
    config = settings()
    if game_running():
        fail("the game is running; close it first")
    run_folder = LOCAL / "runs" / args.run
    if (run_folder / "manifest.json").exists():
        fail(f"{run_folder} already has a manifest; pick a new run name")
    title = config.get("game_title", TEMPLATE["game_title"])
    profile = selected_profile(Path(config["mo2_ini"]))
    profile_folder = Path(config["mo2_profiles"]) / profile
    mods = enabled_mods(profile_folder)
    cmfm_mods = [mod for mod in (config["cmfm_mod"], config["examples_mod"]) if mod in mods]
    if cmfm_mods:
        print(f"note: CMFM mods enabled in {profile}: {', '.join(cmfm_mods)}; the plain entry does not load "
              "them, but their files are in the virtual folder")
    forced = forced_libraries(profile_folder, title)
    run_folder.mkdir(parents=True, exist_ok=True)
    manifest = {
        "run": args.run,
        "kind": "baseline (no CMFM)",
        "date": datetime.date.today().isoformat(),
        "source_commit": commit_label(),
        "exe_sha256": sha256(Path(config["game_exe"])),
        "mo2_profile": profile,
        "mo2_enabled_mods": mods,
        "mo2_executable": title,
        "force_load": forced is not None,
        "force_loaded_libraries": forced or [],
        "steam_overlay": "unconfirmed",
        "sealed": False,
    }
    (run_folder / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                              encoding="utf-8")
    print(f"baseline {args.run}: profile {profile}, {len(mods)} mods enabled, executable '{title}', "
          f"force-load {manifest['force_load']}")
    print(f"manifest: {(run_folder / 'manifest.json').relative_to(ROOT)}")


def launch_folder(args: argparse.Namespace) -> Path:
    folder = LOCAL / "runs" / args.run
    if args.launch:
        folder = folder / f"launch-{args.launch}"
    folder.mkdir(parents=True, exist_ok=True)
    return folder


def watch(args: argparse.Namespace) -> None:
    folder = launch_folder(args)
    with (folder / "watcher.log").open("w", encoding="utf-8") as log:
        subprocess.run([sys.executable, "-B", str(ROOT / "tools" / "modwatch.py"), "--out", str(folder)],
                       cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=False)


def collect(args: argparse.Namespace) -> None:
    config = settings()
    if game_running():
        fail("the game is still running; its logs are not finished. Collect after it closes")
    cmfm = Path(config["mo2_mods"]) / config["cmfm_mod"] / "cmfm"
    folder = launch_folder(args)
    run_folder = LOCAL / "runs" / args.run
    # Logs are named by process id, and ids get reused, so a name seen before
    # the run can still be a new launch: go by modification time instead.
    marker = run_folder / ("logs-before.txt" if (run_folder / "logs-before.txt").exists() else "manifest.json")
    if not marker.exists():
        fail(f"{run_folder} has no logs-before.txt or manifest.json; run install-mo2 or baseline first")
    cutoff = marker.stat().st_mtime_ns
    collected_file = run_folder / "logs-collected.txt"
    collected = set()
    if collected_file.exists():
        for line in collected_file.read_text(encoding="utf-8").splitlines():
            name, _, stamp = line.partition(" ")
            collected.add((name, int(stamp) if stamp else None))
    collected_names = {name for name, stamp in collected if stamp is None}  # older format: name only

    def is_new(path: Path) -> bool:
        stamp = path.stat().st_mtime_ns
        return stamp > cutoff and (path.name, stamp) not in collected and path.name not in collected_names

    target = folder / "cmfm-logs"
    target.mkdir(exist_ok=True)

    # Every launch since install-mo2 or the last collect, oldest first.
    new = {kind: sorted((p for p in (cmfm / "logs").glob(f"{kind}.*.log") if is_new(p)),
                        key=lambda p: p.stat().st_mtime)
           for kind in ("core", "launcher")}
    for log in new["core"] + new["launcher"]:
        shutil.copy2(log, target / log.name)
    # A later collect (the next --launch) skips these.
    with open(collected_file, "a", encoding="utf-8") as record:
        record.writelines(f"{log.name} {log.stat().st_mtime_ns}\n" for log in new["core"] + new["launcher"])
    # Plugin logs keep one previous launch; copy it too when there were two.
    for log in (cmfm / "logs" / "plugins").glob("*.log"):
        if not log.name.endswith(".previous.log") or len(new["core"]) > 1:
            shutil.copy2(log, target / log.name)
    for cache in (cmfm / "cache").glob("scan.*.txt"):
        shutil.copy2(cache, folder / "scan-cache.txt")
    # Crash reports, dumps and notices written since the install.
    crashes = [p for p in (cmfm / "logs" / "crashes").glob("*") if p.is_file() and p.stat().st_mtime_ns > cutoff]
    for crash in crashes:
        (folder / "crashes").mkdir(exist_ok=True)
        shutil.copy2(crash, folder / "crashes" / crash.name)
    print(f"copied to {folder.relative_to(ROOT)}: " + ", ".join(sorted(p.name for p in target.iterdir())))
    for report in sorted(p for p in crashes if p.suffix == ".txt" and p.name.startswith("crash-")):
        print(f"\ncrash report {report.name}")
        for line in report.read_text(encoding="utf-8", errors="replace").splitlines():
            if re.match(r"(phase|exception|at|owner|thread_started_in|minidump|afterwards)=", line):
                print("  " + line)
    if len(new["core"]) > 2:
        print(f"note: {len(new['core'])} launches; plugin logs cover only the last two")

    if not new["core"]:
        print("no new core log")
    for core in new["core"]:
        print(f"\n{core.name}")
        for line in core.read_text(encoding="utf-8", errors="replace").splitlines():
            text = re.sub(r"^\[qpc=\d+ bootstrap_us=\d+\] ", "", line)
            if re.match(r"(plugin name=|phase_pre_main_(enter|complete|skipped|failed)|startup_signature_|"
                        r"startup_crt_cache_written|warning|previous_session_crash|d3d12_state)", text):
                print("  " + text)
    for launcher in new["launcher"]:
        lines = launcher.read_text(encoding="utf-8", errors="replace").splitlines()
        values = dict(line.split("=", 1) for line in lines if "=" in line and line.split("=", 1)[0].isidentifier())
        print(f"\n{launcher.name}")
        if "before_create_qpc" in values and "resumed_qpc" in values:
            print(f"  paused {(int(values['resumed_qpc']) - int(values['before_create_qpc'])) / 1e4:.1f} ms "
                  "(assuming a 10 MHz QPC)")
        print(f"  game_exit_code={values.get('game_exit_code', '?')}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    for name, function in (("build", build), ("test", test), ("check", check), ("stage", stage)):
        commands.add_parser(name).set_defaults(function=function)
    install = commands.add_parser("install-mo2")
    install.add_argument("run")
    install.add_argument("--stage", metavar="COMMIT", help="install local/stage/*-COMMIT instead of HEAD's build")
    install.set_defaults(function=install_mo2)
    plain = commands.add_parser("baseline")
    plain.add_argument("run")
    plain.set_defaults(function=baseline)
    for name, function in (("watch", watch), ("collect", collect)):
        command = commands.add_parser(name)
        command.add_argument("run")
        command.add_argument("--launch", type=int, default=0)
        command.set_defaults(function=function)
    args = parser.parse_args()
    try:
        args.function(args)
    except subprocess.CalledProcessError as error:
        fail(f"command failed with exit code {error.returncode}")


if __name__ == "__main__":
    main()
