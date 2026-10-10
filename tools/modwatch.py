"""Read-only watcher for a process's module load order.

Waits for the game process to start, opens it with
PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ only, and walks the PEB
loader list (InLoadOrderModuleList) until the process exits. Nothing is
written to the process, no thread is touched, nothing is allocated in it.

Each loader entry carries LoadTime, LoadReason and ParentDllBase (undocumented
LDR_DATA_TABLE_ENTRY fields, stable on x64 Windows 10/11). They give exact load
times and the module that caused each load, so polling only has to catch
modules that are unloaded again.

Output, per watched process, in --out:
  modules-<pid>.jsonl   attach, load, unload and exit events
  modules-<pid>.json    final module list (last complete snapshot) with file
                        versions and SHA-256 of modules outside %SystemRoot%
  report-<pid>.txt      the entry-slot table: key modules relative to device
                        creation

With --map, it also records the address-space layout around the game image
(VirtualQueryEx with the same access rights; region metadata only, no memory
contents): "map" events in the .jsonl when a region appears, changes or goes,
and map-<pid>.json with the last layout. That shows what occupies the +/-2GB
around the game's code, where hook libraries place their trampolines.

Run with: python -B tools/modwatch.py --out local/runs/<name> [--map]
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import hashlib
import json
import os
import struct
import sys
import time
from pathlib import Path

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_VM_READ = 0x0010
ACCESS = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ
STILL_ACTIVE = 259
TH32CS_SNAPPROCESS = 0x2
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

# x64 offsets.
PEB_LDR = 0x18
LDR_IN_LOAD_ORDER = 0x10
ENTRY_SIZE = 0x138
E_FLINK = 0x00
E_DLL_BASE = 0x30
E_ENTRY_POINT = 0x38
E_SIZE_OF_IMAGE = 0x40
E_FULL_NAME = 0x48
E_BASE_NAME = 0x58
E_FLAGS = 0x68
E_TIME_DATE_STAMP = 0x80
E_PARENT_DLL_BASE = 0xB8
E_LOAD_TIME = 0x100
E_LOAD_REASON = 0x10C

LOAD_REASONS = {
    0: "static",
    1: "static_forwarder",
    2: "dynamic_forwarder",
    3: "delayload",
    4: "dynamic",
    5: "as_image",
    6: "as_data",
    7: "enclave_primary",
    8: "enclave_dependency",
    9: "patch_image",
    -1: "unknown",
}

# Modules the entry-slot report tracks, lower-case base names. The GPU driver
# user-mode DLL loads inside D3D12CreateDevice, so its load time marks device
# creation.
REPORT_MODULES = [
    "AsteraLoader.dll",
    "usvfs_x64.dll",
    "steamclient64.dll",
    "gameoverlayrenderer64.dll",
    "winmm.dll",
    "cfgmgr32.dll",
    "xinput9_1_0.dll",
    "iphlpapi.dll",
    "mfplat.dll",
    "mfreadwrite.dll",
    "wininet.dll",
    "dinput8.dll",
    "steam_api64.dll",
    "amd_ags_x64.dll",
    "d3d11.dll",
    "d3d12.dll",
    "dxgi.dll",
    "d3d12core.dll",
    "nvldumdx.dll",
    "nvwgf2umx.dll",
    "amdxc64.dll",
    "igd12umd64.dll",
    "sl.interposer.dll",
    "loader.dll",
]
DEVICE_MARKERS = ["nvwgf2umx.dll", "amdxc64.dll", "igd12umd64.dll", "d3d12core.dll"]

MEM_FREE = 0x10000
MAP_STATES = {0x1000: "commit", 0x2000: "reserve"}
MAP_TYPES = {0x20000: "private", 0x40000: "mapped", 0x1000000: "image"}
GRANULARITY = 0x10000

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")
version = ctypes.WinDLL("version", use_last_error=True)


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD),
        ("cntUsage", wt.DWORD),
        ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wt.DWORD),
        ("cntThreads", wt.DWORD),
        ("th32ParentProcessID", wt.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wt.DWORD),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


class PROCESS_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("ExitStatus", ctypes.c_long),
        ("PebBaseAddress", ctypes.c_void_p),
        ("AffinityMask", ctypes.c_size_t),
        ("BasePriority", ctypes.c_long),
        ("UniqueProcessId", ctypes.c_size_t),
        ("InheritedFromUniqueProcessId", ctypes.c_size_t),
    ]


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_void_p),
        ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wt.DWORD),
        ("PartitionId", wt.WORD),
        ("RegionSize", ctypes.c_size_t),
        ("State", wt.DWORD),
        ("Protect", wt.DWORD),
        ("Type", wt.DWORD),
    ]


kernel32.VirtualQueryEx.argtypes = [
    wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MEMORY_BASIC_INFORMATION), ctypes.c_size_t]
kernel32.VirtualQueryEx.restype = ctypes.c_size_t
kernel32.K32GetMappedFileNameW.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.LPWSTR, wt.DWORD]
kernel32.K32GetMappedFileNameW.restype = wt.DWORD
kernel32.CreateToolhelp32Snapshot.restype = wt.HANDLE
kernel32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
kernel32.Process32FirstW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.Process32NextW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
kernel32.CloseHandle.argtypes = [wt.HANDLE]
kernel32.ReadProcessMemory.argtypes = [
    wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t)]
kernel32.GetExitCodeProcess.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
kernel32.GetProcessTimes.argtypes = [wt.HANDLE] + [ctypes.POINTER(wt.FILETIME)] * 4
kernel32.QueryFullProcessImageNameW.argtypes = [
    wt.HANDLE, wt.DWORD, ctypes.c_wchar_p, ctypes.POINTER(wt.DWORD)]
kernel32.GetSystemTimePreciseAsFileTime.argtypes = [ctypes.POINTER(wt.FILETIME)]
kernel32.GetTickCount64.restype = ctypes.c_uint64
ntdll.NtQueryInformationProcess.restype = ctypes.c_long
ntdll.NtQueryInformationProcess.argtypes = [
    wt.HANDLE, ctypes.c_int, ctypes.c_void_p, wt.ULONG, ctypes.POINTER(wt.ULONG)]
version.GetFileVersionInfoSizeW.argtypes = [wt.LPCWSTR, ctypes.POINTER(wt.DWORD)]
version.GetFileVersionInfoW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p]
version.VerQueryValueW.argtypes = [
    ctypes.c_void_p, wt.LPCWSTR, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wt.UINT)]


def filetime_value(ft: wt.FILETIME) -> int:
    return (ft.dwHighDateTime << 32) | ft.dwLowDateTime


def now_filetime() -> int:
    ft = wt.FILETIME()
    kernel32.GetSystemTimePreciseAsFileTime(ctypes.byref(ft))
    return filetime_value(ft)


def find_processes(exe_name: str) -> list[int]:
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == INVALID_HANDLE_VALUE or not snap:
        return []
    pids = []
    try:
        pe = PROCESSENTRY32W()
        pe.dwSize = ctypes.sizeof(pe)
        ok = kernel32.Process32FirstW(snap, ctypes.byref(pe))
        while ok:
            if pe.szExeFile.lower() == exe_name.lower():
                pids.append(pe.th32ProcessID)
            ok = kernel32.Process32NextW(snap, ctypes.byref(pe))
    finally:
        kernel32.CloseHandle(snap)
    return pids


class Target:
    """A process opened for query and read only."""

    def __init__(self, pid: int):
        self.pid = pid
        self.handle = kernel32.OpenProcess(ACCESS, False, pid)
        if not self.handle:
            raise OSError(ctypes.get_last_error(), f"OpenProcess({pid}) failed")

    def close(self) -> None:
        if self.handle:
            kernel32.CloseHandle(self.handle)
            self.handle = None

    def read(self, address: int, size: int) -> bytes | None:
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t(0)
        if not kernel32.ReadProcessMemory(self.handle, ctypes.c_void_p(address), buf,
                                          size, ctypes.byref(got)) or got.value != size:
            return None
        return buf.raw

    def read_ptr(self, address: int) -> int | None:
        raw = self.read(address, 8)
        return None if raw is None else struct.unpack("<Q", raw)[0]

    def read_unicode(self, raw_unicode_string: bytes) -> str | None:
        length, _maximum, buffer = struct.unpack("<HH4xQ", raw_unicode_string)
        if length == 0:
            return ""
        if not buffer or length > 0x2000 or length % 2:
            return None
        raw = self.read(buffer, length)
        return None if raw is None else raw.decode("utf-16-le", "replace")

    def alive(self) -> tuple[bool, int]:
        code = wt.DWORD(0)
        if not kernel32.GetExitCodeProcess(self.handle, ctypes.byref(code)):
            return False, -1
        return code.value == STILL_ACTIVE, code.value

    def creation_time(self) -> int:
        times = [wt.FILETIME() for _ in range(4)]
        kernel32.GetProcessTimes(self.handle, *[ctypes.byref(t) for t in times])
        return filetime_value(times[0])

    def image_path(self) -> str:
        size = wt.DWORD(32768)
        buf = ctypes.create_unicode_buffer(size.value)
        if not kernel32.QueryFullProcessImageNameW(self.handle, 0, buf, ctypes.byref(size)):
            return ""
        return buf.value

    def regions(self, low: int, high: int) -> list[tuple] | None:
        """Allocated regions overlapping [low, high), or None if a query failed.

        Each is (base, size, allocation_base, state, type, protect).
        """
        mbi = MEMORY_BASIC_INFORMATION()
        address = low
        found = []
        while address < high:
            if not kernel32.VirtualQueryEx(self.handle, ctypes.c_void_p(address),
                                           ctypes.byref(mbi), ctypes.sizeof(mbi)):
                return None
            base = mbi.BaseAddress or 0
            if mbi.RegionSize == 0:
                return None
            if mbi.State != MEM_FREE:
                found.append((base, mbi.RegionSize, mbi.AllocationBase or 0, mbi.State,
                              mbi.Type, mbi.Protect))
            address = base + mbi.RegionSize
        return found

    def mapped_name(self, address: int) -> str | None:
        buf = ctypes.create_unicode_buffer(1024)
        if not kernel32.K32GetMappedFileNameW(self.handle, ctypes.c_void_p(address), buf, 1024):
            return None
        return buf.value

    def peb(self) -> int | None:
        pbi = PROCESS_BASIC_INFORMATION()
        got = wt.ULONG(0)
        status = ntdll.NtQueryInformationProcess(self.handle, 0, ctypes.byref(pbi),
                                                 ctypes.sizeof(pbi), ctypes.byref(got))
        if status != 0:
            return None
        return pbi.PebBaseAddress


def walk_modules(target: Target, peb: int, known: dict) -> list[dict] | None:
    """One complete pass over InLoadOrderModuleList, or None if it was torn.

    Entries already in `known` (keyed by base and load time) reuse their
    decoded strings, so a steady-state pass is one read per entry.
    """
    ldr = target.read_ptr(peb + PEB_LDR)
    if not ldr:
        return None
    head = ldr + LDR_IN_LOAD_ORDER
    link = target.read_ptr(head)
    if link is None:
        return None
    modules = []
    while link != head:
        if len(modules) >= 4096:
            return None
        raw = target.read(link, ENTRY_SIZE)
        if raw is None:
            return None
        base = struct.unpack_from("<Q", raw, E_DLL_BASE)[0]
        load_time = struct.unpack_from("<q", raw, E_LOAD_TIME)[0]
        key = (base, load_time)
        entry = known.get(key)
        if entry is None:
            full = target.read_unicode(raw[E_FULL_NAME:E_FULL_NAME + 16])
            name = target.read_unicode(raw[E_BASE_NAME:E_BASE_NAME + 16])
            if full is None or name is None:
                return None
            entry = {
                "base": base,
                "size": struct.unpack_from("<I", raw, E_SIZE_OF_IMAGE)[0],
                "entry_point": struct.unpack_from("<Q", raw, E_ENTRY_POINT)[0],
                "name": name,
                "path": full,
                "flags": struct.unpack_from("<I", raw, E_FLAGS)[0],
                "pe_timestamp": struct.unpack_from("<I", raw, E_TIME_DATE_STAMP)[0],
                "parent_base": struct.unpack_from("<Q", raw, E_PARENT_DLL_BASE)[0],
                "load_time": load_time,
                "load_reason": struct.unpack_from("<i", raw, E_LOAD_REASON)[0],
            }
        modules.append(entry)
        link = struct.unpack_from("<Q", raw, E_FLINK)[0]
    return modules


def file_version(path: str) -> str | None:
    handle = wt.DWORD(0)
    size = version.GetFileVersionInfoSizeW(path, ctypes.byref(handle))
    if not size:
        return None
    block = ctypes.create_string_buffer(size)
    if not version.GetFileVersionInfoW(path, 0, size, block):
        return None
    ptr = ctypes.c_void_p()
    length = wt.UINT(0)
    if not version.VerQueryValueW(block, "\\", ctypes.byref(ptr), ctypes.byref(length)):
        return None
    if length.value < 16:
        return None
    ms, ls = struct.unpack("<II", ctypes.string_at(ptr.value + 8, 8))
    return f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}"


def sha256_file(path: str) -> str | None:
    try:
        digest = hashlib.sha256()
        with open(path, "rb") as handle:
            for chunk in iter(lambda: handle.read(1 << 20), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError:
        return None


def ms_since(start: int, filetime: int) -> float:
    return round((filetime - start) / 10_000, 3)


class Watch:
    """Watches one process from attach to exit."""

    def __init__(self, pid: int, out: Path, fast_seconds: float, fast_interval: float,
                 slow_interval: float, map_margin: int = 0, map_interval: float = 0.05):
        self.target = Target(pid)
        self.pid = pid
        self.out = out
        self.fast_seconds = fast_seconds
        self.fast_interval = fast_interval
        self.slow_interval = slow_interval
        self.map_margin = map_margin
        self.map_interval = map_interval
        self.map_window: tuple[int, int, int, int] | None = None
        self.map_last: dict[int, tuple] = {}
        self.map_names: dict[int, str | None] = {}
        self.map_polls = 0
        self.map_torn = 0
        self.map_due = 0.0
        self.events = open(out / f"modules-{pid}.jsonl", "w", encoding="utf-8")
        self.known: dict = {}
        self.order: list = []
        self.last_complete: list[dict] = []
        self.polls = 0
        self.torn = 0
        self.seq = 0
        self.attach_perf = time.perf_counter()
        self.creation = self.target.creation_time()
        self.exe = self.target.image_path()
        self.layout_suspect = 0
        self.emit({
            "event": "attach",
            "pid": pid,
            "exe": self.exe,
            "creation_filetime": self.creation,
            "attach_ms": ms_since(self.creation, now_filetime()),
            "attach_tick64": kernel32.GetTickCount64(),
            "access": hex(ACCESS),
            "observer": {
                "python": sys.version.split()[0],
                "windows": ".".join(str(x) for x in sys.getwindowsversion()[:3]),
            },
        })

    def emit(self, record: dict) -> None:
        self.events.write(json.dumps(record, ensure_ascii=False) + "\n")
        self.events.flush()

    def describe(self, entry: dict, position: int, by_base: dict) -> dict:
        load_time = entry["load_time"]
        plausible = self.creation - 50_000_000 <= load_time <= now_filetime() + 50_000_000
        if not plausible:
            self.layout_suspect += 1
        parent = by_base.get(entry["parent_base"])
        return {
            "seq": self.seq,
            "pos": position,
            "name": entry["name"],
            "path": entry["path"],
            "base": hex(entry["base"]),
            "size": entry["size"],
            "load_ms": ms_since(self.creation, load_time) if plausible else None,
            "load_time": load_time,
            "reason": LOAD_REASONS.get(entry["load_reason"], str(entry["load_reason"])),
            "parent": parent["name"] if parent else (
                hex(entry["parent_base"]) if entry["parent_base"] else None),
            "flags": hex(entry["flags"]),
            "pe_timestamp": hex(entry["pe_timestamp"]),
            "seen_ms": ms_since(self.creation, now_filetime()),
        }

    def poll(self, peb: int) -> None:
        self.polls += 1
        modules = walk_modules(self.target, peb, self.known)
        if modules is None:
            self.torn += 1
            return
        by_base = {m["base"]: m for m in modules}
        current = set()
        for position, entry in enumerate(modules):
            key = (entry["base"], entry["load_time"])
            current.add(key)
            if key not in self.known:
                self.known[key] = entry
                self.order.append(key)
                record = self.describe(entry, position, by_base)
                record["event"] = "load"
                entry["first"] = record
                self.emit(record)
                self.seq += 1
        for entry in self.last_complete:
            key = (entry["base"], entry["load_time"])
            if key not in current and not entry.get("unloaded"):
                entry["unloaded"] = True
                self.emit({"event": "unload", "name": entry["name"], "path": entry["path"],
                           "base": hex(entry["base"]),
                           "seen_ms": ms_since(self.creation, now_filetime())})
        self.last_complete = modules
        if self.map_margin and self.map_window is None and modules:
            # The first loader entry is the executable itself.
            exe = modules[0]
            low = max(GRANULARITY, exe["base"] - self.map_margin)
            high = min(exe["base"] + exe["size"] + self.map_margin, 0x7FFFFFFF0000)
            self.map_window = (low, high, exe["base"], exe["size"])
            self.emit({"event": "map_window", "low": hex(low), "high": hex(high),
                       "image": hex(exe["base"]), "image_size": exe["size"],
                       "seen_ms": ms_since(self.creation, now_filetime())})

    def region_record(self, region: tuple) -> dict:
        base, size, allocation, state, kind, protect = region
        if kind in (0x40000, 0x1000000) and allocation not in self.map_names:
            self.map_names[allocation] = self.target.mapped_name(allocation)
        return {
            "base": hex(base),
            "size": size,
            "allocation": hex(allocation),
            "state": MAP_STATES.get(state, hex(state)),
            "type": MAP_TYPES.get(kind, hex(kind)),
            "protect": hex(protect),
            "name": self.map_names.get(allocation),
        }

    def poll_map(self) -> None:
        low, high, _image, _size = self.map_window
        regions = self.target.regions(low, high)
        if regions is None:
            self.map_torn += 1
            return
        first = self.map_polls == 0
        self.map_polls += 1
        seen_ms = ms_since(self.creation, now_filetime())
        current = {r[0]: r for r in regions}
        for base, region in current.items():
            if self.map_last.get(base) != region:
                change = "initial" if first else (
                    "changed" if base in self.map_last else "new")
                self.emit({"event": "map", "change": change, **self.region_record(region),
                           "seen_ms": seen_ms})
        for base, region in self.map_last.items():
            if base not in current:
                self.emit({"event": "map", "change": "gone", "base": hex(base),
                           "size": region[1], "seen_ms": seen_ms})
        self.map_last = current

    def run(self) -> int:
        peb = None
        while True:
            alive, code = self.target.alive()
            if not alive:
                break
            if peb is None:
                peb = self.target.peb()
            if peb:
                self.poll(peb)
            now = time.perf_counter()
            if self.map_window and now >= self.map_due:
                self.poll_map()
                self.map_due = now + self.map_interval
            elapsed = now - self.attach_perf
            time.sleep(self.fast_interval if elapsed < self.fast_seconds else self.slow_interval)
        self.emit({"event": "exit", "exit_code": code, "polls": self.polls, "torn": self.torn,
                   "modules_seen": len(self.order), "layout_suspect": self.layout_suspect,
                   "map_polls": self.map_polls, "map_torn": self.map_torn,
                   "watched_s": round(time.perf_counter() - self.attach_perf, 1)})
        self.events.close()
        self.target.close()
        self.finish()
        return code

    def finish(self) -> None:
        system_root = os.environ.get("SystemRoot", r"C:\Windows").lower()
        exe_sha = sha256_file(self.exe) if self.exe else None
        final = []
        for position, entry in enumerate(self.last_complete):
            record = dict(entry["first"])
            record["final_pos"] = position
            if not entry["path"].lower().startswith(system_root):
                record["file_version"] = file_version(entry["path"])
                record["sha256"] = sha256_file(entry["path"])
            final.append(record)
        unloaded = [self.known[k]["first"] for k in self.order
                    if self.known[k].get("unloaded")]
        summary = {
            "pid": self.pid,
            "exe": self.exe,
            "exe_sha256": exe_sha,
            "exe_version": file_version(self.exe) if self.exe else None,
            "creation_filetime": self.creation,
            "polls": self.polls,
            "torn": self.torn,
            "layout_suspect": self.layout_suspect,
            "modules": final,
            "unloaded": unloaded,
        }
        (self.out / f"modules-{self.pid}.json").write_text(
            json.dumps(summary, indent=1, ensure_ascii=False), encoding="utf-8")
        text = report(summary)
        if self.map_window:
            layout = self.map_layout()
            (self.out / f"map-{self.pid}.json").write_text(
                json.dumps(layout, indent=1, ensure_ascii=False), encoding="utf-8")
            text += map_report(layout)
        (self.out / f"report-{self.pid}.txt").write_text(text, encoding="utf-8")

    def map_layout(self) -> dict:
        """The last recorded layout: allocated regions and the free gaps between."""
        low, high, image, image_size = self.map_window
        regions = [self.region_record(r) for _, r in sorted(self.map_last.items())]
        gaps = []
        cursor = low
        for base, size, *_ in sorted(self.map_last.values()):
            if base > cursor:
                gaps.append((cursor, base))
            cursor = max(cursor, base + size)
        if cursor < high:
            gaps.append((cursor, high))
        return {
            "window": [hex(low), hex(high)],
            "image": hex(image),
            "image_size": image_size,
            "map_polls": self.map_polls,
            "regions": regions,
            "free": [{"low": hex(a), "high": hex(b), "size": b - a} for a, b in gaps],
        }


def map_report(layout: dict) -> str:
    image = int(layout["image"], 16)
    lines = ["", f"Address space around the game image {layout['image']} "
             f"(window {layout['window'][0]}-{layout['window'][1]}, last of "
             f"{layout['map_polls']} polls):"]
    for region in layout["regions"]:
        base = int(region["base"], 16)
        if image <= base < image + layout["image_size"]:
            continue
        lines.append(f"  {region['base']:>14} {region['size']:>12,} {region['type']:<8} "
                     f"{region['state']:<8} {region['protect']:<6} "
                     f"{(base - image) / 2**30:+7.3f} GiB from image  "
                     f"{region['name'] or ''}")
    lines.append("Free gaps of 64 KiB or more:")
    for gap in layout["free"]:
        if gap["size"] >= GRANULARITY:
            lines.append(f"  {gap['low']:>14}-{gap['high']:<14} {gap['size']:>14,}")
    return "\n".join(lines) + "\n"


def report(summary: dict) -> str:
    rows = [m for m in summary["modules"] + summary["unloaded"]]
    rows.sort(key=lambda m: m["seq"])
    device = None
    for marker in DEVICE_MARKERS:
        hits = [m for m in rows if m["name"].lower() == marker]
        if hits:
            device = hits[0]
            break
    lines = [
        f"pid {summary['pid']}  exe {summary['exe']}",
        f"exe sha256 {summary['exe_sha256']}  version {summary['exe_version']}",
        f"polls {summary['polls']}  torn {summary['torn']}  "
        f"layout_suspect {summary['layout_suspect']}  modules {len(rows)}",
        "device creation marker: " + (
            f"{device['name']} seq {device['seq']} at {device['load_ms']} ms" if device
            else "none seen"),
        "",
        f"{'seq':>4} {'ms':>10} {'vs device':>10}  {'reason':<10} {'parent':<26} name / path",
    ]
    for m in rows:
        if m["name"].lower() not in REPORT_MODULES:
            continue
        if device and m["load_ms"] is not None and device["load_ms"] is not None:
            delta = f"{m['load_ms'] - device['load_ms']:+.1f}"
        else:
            delta = "?"
        lines.append(f"{m['seq']:>4} {str(m['load_ms']):>10} {delta:>10}  "
                     f"{m['reason']:<10} {str(m['parent'])[:26]:<26} "
                     f"{m['name']}  {m['path']}")
    lines.append("")
    lines.append("Modules outside SystemRoot (final list):")
    for m in summary["modules"]:
        if "sha256" in m:
            lines.append(f"{m['seq']:>4} {m['name']:<32} {str(m.get('file_version')):<16} "
                         f"{str(m.get('sha256'))[:16]}  {m['path']}")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--exe", default="MonsterHunterWorld.exe")
    parser.add_argument("--pid", type=int, help="watch this process instead (testing)")
    parser.add_argument("--wait", type=float, default=3600,
                        help="seconds to wait for the first process")
    parser.add_argument("--linger", type=float, default=20,
                        help="seconds to wait for a relaunch after the last process exits")
    parser.add_argument("--fast-seconds", type=float, default=120)
    parser.add_argument("--fast-interval", type=float, default=0.01)
    parser.add_argument("--slow-interval", type=float, default=0.5)
    parser.add_argument("--find-interval", type=float, default=0.02)
    parser.add_argument("--map", action="store_true",
                        help="also record the address-space layout around the game image")
    parser.add_argument("--map-margin", type=lambda v: int(v, 0), default=0x90000000,
                        help="bytes recorded below and above the image (default 2.25 GiB)")
    parser.add_argument("--map-interval", type=float, default=0.05)
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    print(f"waiting for {args.exe} (read-only: access {hex(ACCESS)})", flush=True)
    seen: set[int] = set()
    deadline = time.monotonic() + args.wait
    while time.monotonic() < deadline:
        if args.pid:
            new = [args.pid] if args.pid not in seen else []
            if not new:
                break
        else:
            new = [pid for pid in find_processes(args.exe) if pid not in seen]
        if not new:
            time.sleep(args.find_interval)
            continue
        pid = new[0]
        seen.add(pid)
        try:
            watch = Watch(pid, args.out, args.fast_seconds, args.fast_interval,
                          args.slow_interval, args.map_margin if args.map else 0,
                          args.map_interval)
        except OSError as error:
            print(f"pid {pid}: cannot open: {error}", flush=True)
            continue
        print(f"pid {pid}: attached at {watch.events.name}", flush=True)
        code = watch.run()
        print(f"pid {pid}: exited with {code}; {len(watch.order)} modules", flush=True)
        deadline = time.monotonic() + args.linger
    print(f"done; watched {sorted(seen) or 'nothing'}", flush=True)
    return 0 if seen else 1


if __name__ == "__main__":
    sys.exit(main())
