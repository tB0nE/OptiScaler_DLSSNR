#!/usr/bin/env python3
"""ostool - put the OptiScaler-DLSSNR + frame-generation setup into Steam/Proton games, reversibly.

Nothing is changed without --apply. Every change is recorded in a manifest (with sha256 hashes) so
`uninstall` puts a game back exactly as it was. Standard library only.

  ostool detect                      list Steam games and how suitable they look
  ostool kit init --from-game DIR    build the local kit from a game where the setup already works
  ostool install GAME [--apply]      plan (or perform) an install; GAME = appid, name fragment or path
  ostool verify GAME [--kernel]      check hashes and read the game's logs
  ostool uninstall GAME [--apply]    undo an install from its manifest
  ostool list                        games that have an install
  ostool selftest                    round-trip test on a throwaway fake game
"""
from __future__ import annotations

import argparse
import hashlib
import json
import mmap
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

TOOL_VERSION = "0.1.0"
MANIFEST_DIR = "_mod_backups/ostool"

# What a kit holds. Left: path inside the kit. Right: where it goes next to the game's exe.
KIT_FILES = {
    "OptiScaler.dll": "dxgi.dll",
    "nvngx.dll_dlssnr.dll": "nvngx.dll_dlssnr.dll",
    "nvngx_dlssnr.dll": "nvngx_dlssnr.dll",  # the NR model: supplied by the user, never redistributed
}
KIT_DIRS = ["OptiScaler", "Licenses", "peripheral_warp"]
MOD_FILES = {"mod/version.dll": "version.dll", "mod/dlssg_sm86.ini": "dlssg_sm86.ini"}
INI_TEMPLATE = "OptiScaler.ini.template"
WARP_SHADERS = ["fullscreen_vs.dxbc", "pack_ps.dxbc", "unpack_ps.dxbc"]

# Per-game ini overrides. Work must be >= (100 + Center) / 2 per axis (the warp SDK's rule).
PROFILES = {
    "default": {"DlssNr": {"PeripheralWarpEnabled": "false", "TemporalEnabled": "false",
                           "TemporalBackground": "false"}},
    "warp": {"DlssNr": {"PeripheralWarpEnabled": "true", "PeripheralWarpCenterX": "59",
                        "PeripheralWarpCenterY": "59", "PeripheralWarpWorkX": "80",
                        "PeripheralWarpWorkY": "80", "TemporalEnabled": "false",
                        "TemporalBackground": "false"}},
    "warp-rt": {"DlssNr": {"PeripheralWarpEnabled": "true", "PeripheralWarpCenterX": "28",
                           "PeripheralWarpCenterY": "28", "PeripheralWarpWorkX": "64.01",
                           "PeripheralWarpWorkY": "64.01", "TemporalEnabled": "false",
                           "TemporalBackground": "false"}},
}

# Extra profile for Baldur's Gate 3's DX11 executable (bg3_dx11.exe); bg3.exe uses Dx12Upscaler=dlss (already default).
PROFILES["bg3-dx11"] = {"Upscalers": {"Dx11Upscaler": "dlss_12"}}

STREAMLINE_FILES = ["sl.interposer.dll", "sl.dlss_g.dll", "sl.dlss.dll", "sl.common.dll"]
DLSS_FILES = ["nvngx_dlss.dll", "nvngx_dlssd.dll", "nvngx_dlssg.dll"]
PROXY_NAMES = ["dxgi.dll", "version.dll", "winmm.dll", "dbghelp.dll", "d3d12.dll", "dinput8.dll"]
ANTICHEAT = ["easyanticheat", "battleye", "beclient", "xigncode", "eac_launcher"]
# Online-only games where modding risks a ban, however the file scan looks.
KNOWN_ONLINE = {"2344520": "Diablo IV is online-only (Blizzard); modding can get an account banned"}


def kit_dir() -> Path:
    return Path(os.environ.get("OSTOOL_KIT", str(Path.home() / ".local/share/ostool/kit")))


# ----------------------------------------------------------------------------- small helpers
def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def info(msg: str = "") -> None:
    print(msg)


def die(msg: str, code: int = 2) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(code)


def human(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return f"{n}"


# ----------------------------------------------------------------------------- Steam discovery
def parse_vdf(text: str) -> dict:
    tokens = re.findall(r'"((?:[^"\\]|\\.)*)"|([{}])', text)
    stack: list[dict] = [{}]
    key = None
    for s, brace in tokens:
        if brace == "{":
            new: dict = {}
            stack[-1][key] = new
            stack.append(new)
            key = None
        elif brace == "}":
            if len(stack) > 1:
                stack.pop()
            key = None
        else:
            s = s.replace("\\\\", "\\")
            if key is None:
                key = s
            else:
                stack[-1][key] = s
                key = None
    return stack[0]


def steam_roots() -> list[Path]:
    env = os.environ.get("OSTOOL_STEAM_ROOTS")
    if env:
        return [Path(p) for p in env.split(":") if p]
    cands = [Path.home() / ".local/share/Steam", Path.home() / ".steam/steam",
             Path.home() / ".var/app/com.valvesoftware.Steam/.local/share/Steam"]
    out, seen = [], set()
    for c in cands:
        try:
            r = c.resolve()
        except OSError:
            continue
        if r.exists() and str(r) not in seen:
            seen.add(str(r))
            out.append(r)
    return out


def libraries() -> list[Path]:
    libs: list[Path] = []
    seen: set[tuple[int, int]] = set()

    def add(p: Path) -> None:
        try:
            if not (p / "steamapps").is_dir():
                return
            st = (p / "steamapps").stat()
        except OSError:
            return
        key = (st.st_dev, st.st_ino)
        if key not in seen:
            seen.add(key)
            libs.append(p)

    for root in steam_roots():
        add(root)
        vdf = root / "steamapps" / "libraryfolders.vdf"
        if vdf.exists():
            data = parse_vdf(vdf.read_text(errors="replace")).get("libraryfolders", {})
            for v in data.values():
                if isinstance(v, dict) and v.get("path"):
                    add(Path(v["path"]))
    extra = os.environ.get("OSTOOL_LIBRARIES")
    if extra:
        for p in extra.split(":"):
            add(Path(p))
    elif not os.environ.get("OSTOOL_STEAM_ROOTS"):
        # Removable drives are often mounted under a different name than Steam recorded.
        for pattern in ("/run/media/*/*/SteamLibrary", "/mnt/*/SteamLibrary", "/var/mnt/*/SteamLibrary"):
            for p in sorted(Path("/").glob(pattern.lstrip("/"))):
                add(p)
    return libs


@dataclass
class Game:
    appid: str
    name: str
    path: Path
    prefix: Path | None
    library: Path


def games() -> list[Game]:
    out: list[Game] = []
    for lib in libraries():
        sa = lib / "steamapps"
        for acf in sorted(sa.glob("appmanifest_*.acf")):
            try:
                d = parse_vdf(acf.read_text(errors="replace")).get("AppState", {})
            except OSError:
                continue
            appid, name, inst = d.get("appid"), d.get("name", ""), d.get("installdir")
            if not appid or not inst:
                continue
            if re.search(r"proton|steam linux runtime|steamworks|redistributable", name, re.I):
                continue
            path = sa / "common" / inst
            if not path.is_dir() or not any(path.iterdir()):
                continue  # not installed (leftover empty folder)
            prefix = sa / "compatdata" / appid / "pfx"
            out.append(Game(appid, name, path, prefix if prefix.is_dir() else None, lib))
    return out


def resolve_game(sel: str) -> Game:
    p = Path(sel)
    if p.is_dir():
        return Game("?", p.name, p, None, p.parent)
    allg = games()
    hits = [g for g in allg if g.appid == sel] or \
           [g for g in allg if sel.lower() in g.name.lower()]
    if not hits:
        die(f"no Steam game matches '{sel}' (try `ostool detect`)")
    if len(hits) > 1:
        names = ", ".join(f"{g.name} ({g.appid})" for g in hits[:6])
        die(f"'{sel}' matches several games: {names}")
    return hits[0]


# ----------------------------------------------------------------------------- exe analysis
SKIP_DIRS = {"redist", "redistributables", "_commonredist", "__installer", "directx", "vcredist",
             "crashreportclient", "easyanticheat", "battleye", "eos", "dotnet", "__pycache__", "_mod_backups"}
SKIP_EXE = re.compile(r"(unins|crash|report|launcher|redist|dxsetup|setup|helper|updater|installer|"
                      r"easyanticheat|battleye|dotnet|webview|cef|benchmark)", re.I)


def pe_machine(path: Path) -> int:
    try:
        with open(path, "rb") as f:
            head = f.read(4096)
            if head[:2] != b"MZ":
                return 0
            off = int.from_bytes(head[0x3C:0x40], "little")
            if off + 6 > len(head):
                f.seek(off)
                head = f.read(8)
                off = 0
            if head[off:off + 4] != b"PE\0\0":
                return 0
            return int.from_bytes(head[off + 4:off + 6], "little")
    except OSError:
        return 0


def find_exes(root: Path, depth: int = 5) -> list[Path]:
    found: list[tuple[int, Path]] = []
    for dirpath, dirnames, filenames in os.walk(root):
        rel = os.path.relpath(dirpath, root)
        d = 0 if rel == "." else rel.count(os.sep) + 1
        dirnames[:] = [x for x in dirnames if x.lower() not in SKIP_DIRS] if d < depth else []
        for f in filenames:
            if f.lower().endswith(".exe") and not SKIP_EXE.search(f):
                p = Path(dirpath) / f
                try:
                    found.append((p.stat().st_size, p))
                except OSError:
                    pass
    found.sort(key=lambda t: -t[0])
    return [p for _, p in found]


def main_exe(game: Game) -> Path | None:
    cands = [p for p in find_exes(game.path)[:12] if pe_machine(p) == 0x8664]
    if not cands:
        return None

    def dx12(p: Path) -> bool:
        return "dx12" in str(p.relative_to(game.path)).lower() or (p.parent / "D3D12").is_dir()

    return ([p for p in cands if dx12(p)] or cands)[0]


def scan_exe(path: Path) -> dict:
    needles = {"d3d12": "d3d12.dll", "d3d11": "d3d11.dll", "vulkan": "vulkan-1.dll",
               "nvngx": "nvngx", "nvapi": "nvapi64"}
    res = {k: False for k in needles}

    def both(text: str) -> bytes:  # the name as narrow text or as UTF-16 text
        narrow = re.escape(text.encode())
        wide = b"\x00".join(re.escape(bytes([c])) for c in text.encode()) + b"\x00"
        return b"(?:" + narrow + b"|" + wide + b")"

    try:
        with open(path, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
            for k, text in needles.items():
                res[k] = re.search(both(text), mm, re.I) is not None
    except (OSError, ValueError):
        pass
    return res


def assess(game: Game, exe_override: Path | None = None) -> dict:
    exe = exe_override or main_exe(game)
    a: dict = {"exe": exe, "notes": [], "ok": False}
    if exe is None:
        a["notes"].append("no 64-bit exe found")
        return a
    exe_dir = exe.parent
    a["exe_dir"] = exe_dir
    a["scan"] = scan_exe(exe)
    names = {p.name.lower() for p in exe_dir.iterdir()} if exe_dir.is_dir() else set()
    a["streamline"] = [f for f in STREAMLINE_FILES if f in names]
    a["dlss_files"] = [f for f in DLSS_FILES if f in names]
    a["existing_proxies"] = [f for f in PROXY_NAMES if f in names]
    a["has_dlss_g"] = "sl.dlss_g.dll" in names or "nvngx_dlssg.dll" in names
    top = {p.name.lower() for p in game.path.iterdir()} if game.path.is_dir() else set()
    a["anticheat"] = sorted({ac for ac in ANTICHEAT for n in (names | top) if ac in n})
    s = a["scan"]
    dx12 = s["d3d12"] or a["has_dlss_g"] or (exe_dir / "D3D12").is_dir() or (exe_dir / "d3d12").is_dir() \
        or (exe_dir / "D3D12Core.dll").exists()
    a["dx12"] = dx12
    if not dx12:
        a["notes"].append("does not look like a DirectX 12 game")
    if not (a["dlss_files"] or s["nvngx"] or a["streamline"]):
        a["notes"].append("no sign of DLSS (needs DLSS SR/RR to hook into)")
    if a["anticheat"]:
        a["notes"].append("anti-cheat present: " + ", ".join(a["anticheat"]))
    if game.appid in KNOWN_ONLINE:
        a["notes"].append(KNOWN_ONLINE[game.appid])
    a["ok"] = dx12 and bool(a["dlss_files"] or s["nvngx"] or a["streamline"]) and not a["anticheat"] \
        and game.appid not in KNOWN_ONLINE
    return a


def cmd_detect(args) -> int:
    gl = games()
    if not gl:
        info("no Steam games found")
        return 1
    info(f"{'APPID':>8}  {'FIT':<4} {'DLSS-G':<6} GAME  (exe dir)")
    for g in gl:
        a = assess(g)
        if a["exe"] is None and not args.all:
            continue
        fit = "yes" if a["ok"] else "no"
        if not args.all and not a["ok"]:
            continue
        fg = "yes" if a.get("has_dlss_g") else "-"
        where = a["exe_dir"].relative_to(g.path) if a.get("exe_dir") else "?"
        info(f"{g.appid:>8}  {fit:<4} {fg:<6} {g.name}  ({where})")
        if args.all and a["notes"]:
            info("          " + "; ".join(a["notes"]))
    if not args.all:
        info("\n(only likely candidates shown; --all lists every game with the reasons)")
    return 0


# ----------------------------------------------------------------------------- ini editing
def set_ini(text: str, section: str, key: str, value: str) -> str:
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(nl)
    sec = re.compile(r"^\[(.+?)\]\s*$")
    start = next((i for i, l in enumerate(lines)
                  if (m := sec.match(l)) and m.group(1).lower() == section.lower()), None)
    if start is None:
        while lines and lines[-1] == "":
            lines.pop()
        lines += ["", f"[{section}]", f"{key} = {value}", ""]
        return nl.join(lines)
    end = next((j for j in range(start + 1, len(lines)) if sec.match(lines[j])), len(lines))
    keyre = re.compile(rf"^\s*{re.escape(key)}\s*=", re.I)
    for j in range(start + 1, end):
        if keyre.match(lines[j]):
            lines[j] = f"{key} = {value}"
            return nl.join(lines)
    lines.insert(start + 1, f"{key} = {value}")
    return nl.join(lines)


def merge_profiles(names: str) -> dict:
    merged: dict = {}
    for name in [n.strip() for n in names.split(",") if n.strip()]:
        prof = PROFILES.get(name)
        if prof is None:
            die(f"unknown profile '{name}' (have: {', '.join(PROFILES)})")
        for sec, kv in prof.items():
            merged.setdefault(sec, {}).update(kv)
    return merged


def render_ini(template: str, profile: dict, sets: list[str]) -> str:
    # An ini that names one game's exe puts OptiScaler in pass-through mode in another: always auto.
    text = set_ini(template, "ProcessFilter", "TargetProcessName", "auto")
    for section, kv in profile.items():
        for k, v in kv.items():
            text = set_ini(text, section, k, v)
    for s in sets:
        m = re.fullmatch(r"([^.=]+)\.([^=]+)=(.*)", s)
        if not m:
            die(f"--set expects Section.Key=Value, got '{s}'")
        text = set_ini(text, m.group(1), m.group(2), m.group(3))
    return text


# ----------------------------------------------------------------------------- Wine registry
def reg_header(exe: str) -> str:
    return "[Software\\\\Wine\\\\AppDefaults\\\\" + exe + "\\\\DllOverrides]"


def reg_apply(text: str, exe: str, names: list[str]) -> tuple[str, dict]:
    """Ensure "name"="native,builtin" for each name in the game's AppDefaults DllOverrides section."""
    lines = text.split("\n")
    header = reg_header(exe).lower()
    rec = {"section_added": False, "added": [], "changed": {}}
    start = next((i for i, l in enumerate(lines) if l.lower().startswith(header)), None)
    want = '"{}"="native,builtin"'
    if start is None:
        ts = int(time.time())
        ft = format((ts + 11644473600) * 10_000_000, "x")
        while lines and lines[-1] == "":
            lines.pop()
        lines += ["", f"{reg_header(exe)} {ts}", f"#time={ft}"] + [want.format(n) for n in names] + [""]
        rec["section_added"] = True
        rec["added"] = list(names)
        return "\n".join(lines), rec
    end = next((j for j in range(start + 1, len(lines)) if lines[j].startswith("[")), len(lines))
    for n in names:
        pat = re.compile(rf'^"{re.escape(n)}"=', re.I)
        idx = next((j for j in range(start + 1, end) if pat.match(lines[j])), None)
        if idx is None:
            lines.insert(end, want.format(n))
            end += 1
            rec["added"].append(n)
        elif lines[idx] != want.format(n):
            rec["changed"][n] = lines[idx]
            lines[idx] = want.format(n)
    return "\n".join(lines), rec


def reg_undo(text: str, exe: str, rec: dict) -> str:
    lines = text.split("\n")
    header = reg_header(exe).lower()
    start = next((i for i, l in enumerate(lines) if l.lower().startswith(header)), None)
    if start is None:
        return text
    end = next((j for j in range(start + 1, len(lines)) if lines[j].startswith("[")), len(lines))
    body = lines[start + 1:end]
    for n in rec.get("added", []):
        body = [l for l in body if not re.match(rf'^"{re.escape(n)}"=', l, re.I)]
    for n, old in rec.get("changed", {}).items():
        body = [old if re.match(rf'^"{re.escape(n)}"=', l, re.I) else l for l in body]
    remaining = [l for l in body if l.startswith('"')]
    if rec.get("section_added") and not remaining:
        del lines[start:end]
        while len(lines) > start and lines[start] == "" and start > 0 and lines[start - 1] == "":
            del lines[start]
        return "\n".join(lines).rstrip("\n") + "\n" if text.endswith("\n") else "\n".join(lines)
    lines[start + 1:end] = body
    return "\n".join(lines)


def proc_scan(pred) -> list[int]:
    hits = []
    for d in Path("/proc").iterdir():
        if d.name.isdigit():
            try:
                if pred(d):
                    hits.append(int(d.name))
            except OSError:
                hits.append(int(d.name)) if False else None
    return hits


def wineserver_using(prefix: Path) -> bool:
    if os.environ.get("OSTOOL_NO_PROCCHECK"):
        return False
    target = str(prefix.parent)

    def pred(d: Path) -> bool:
        if (d / "comm").read_text().strip() != "wineserver":
            return False
        try:
            env = (d / "environ").read_bytes().decode(errors="replace")
        except OSError:
            return True  # cannot tell: be conservative
        return any(target in kv or str(prefix) in kv for kv in env.split("\0")
                   if kv.startswith(("WINEPREFIX=", "STEAM_COMPAT_DATA_PATH=")))

    return bool(proc_scan(pred))


def game_running(exe_name: str) -> bool:
    if os.environ.get("OSTOOL_NO_PROCCHECK"):
        return False
    # Wine names a process after its exe (the kernel keeps 15 characters): match that exactly, not any
    # command line that merely mentions the name (a shell, an editor, this very tool).
    want = exe_name.lower()[:15]
    return bool(proc_scan(lambda d: (d / "comm").read_text().strip().lower() == want))


# ----------------------------------------------------------------------------- kit
def kit_check(kit: Path) -> list[str]:
    need = ["OptiScaler.dll", INI_TEMPLATE, "nvngx.dll_dlssnr.dll"] + \
           [f"peripheral_warp/{s}" for s in WARP_SHADERS]
    return [n for n in need if not (kit / n).exists()]


def contains(path: Path, needle: bytes) -> bool:
    with open(path, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
        return mm.find(needle) >= 0


def cmd_kit(args) -> int:
    kit = kit_dir()
    if args.kit_cmd == "show":
        if not kit.exists():
            info(f"no kit at {kit}")
            return 1
        meta = kit / "kit.json"
        info(f"kit: {kit}")
        if meta.exists():
            m = json.loads(meta.read_text())
            info(f"  from {m.get('source')} on {m.get('created')}")
            for k, v in m.get("files", {}).items():
                info(f"  {v[:12]}  {k}")
        miss = kit_check(kit)
        info("  missing: " + ", ".join(miss) if miss else "  complete")
        return 0
    src = Path(args.from_game)
    if not (src / "dxgi.dll").exists():
        die(f"{src}/dxgi.dll not found: point --from-game at the folder that holds the working setup")
    if not contains(src / "dxgi.dll", b"OptiScaler"):
        die("dxgi.dll there is not OptiScaler")
    if kit.exists() and any(kit.iterdir()) and not args.force:
        die(f"{kit} already has a kit (use --force to rebuild it)")
    ini = src / "OptiScaler.ini"
    plan = [("dxgi.dll", "OptiScaler.dll"), ("OptiScaler.ini", INI_TEMPLATE),
            ("nvngx.dll_dlssnr.dll", "nvngx.dll_dlssnr.dll"), ("nvngx_dlssnr.dll", "nvngx_dlssnr.dll")]
    total = sum((src / a).stat().st_size for a, _ in plan if (src / a).exists())
    dirs = [d for d in KIT_DIRS if (src / d).is_dir()]
    for d in dirs:
        total += sum(f.stat().st_size for f in (src / d).rglob("*") if f.is_file())
    mods = [(a, b) for a, b in (("version.dll", "mod/version.dll"), ("dlssg_sm86.ini", "mod/dlssg_sm86.ini"))
            if (src / a).exists()]
    if (src / "version.dll").exists() and not contains(src / "version.dll", b"dlssg"):
        mods = []
        info("note: version.dll there is not the frame-gen mod; the kit will have no frame generation")
    free = shutil.disk_usage(kit.parent if kit.parent.exists() else Path.home()).free
    info(f"kit will use about {human(total)} at {kit} ({human(free)} free)")
    if total > free * 0.9:
        die("not enough free space")
    if args.dry_run:
        info("dry run: nothing copied")
        return 0
    kit.mkdir(parents=True, exist_ok=True)
    files = {}
    for a, b in plan + mods:
        s = src / a
        if s.exists():
            (kit / b).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(s, kit / b)
            files[b] = sha256(kit / b)
    for d in dirs:
        shutil.copytree(src / d, kit / d, dirs_exist_ok=True)
    (kit / "kit.json").write_text(json.dumps({"tool": TOOL_VERSION, "source": str(src),
                                              "created": time.strftime("%Y-%m-%d %H:%M:%S"),
                                              "files": files}, indent=2))
    miss = kit_check(kit)
    info("kit built" + (f" but incomplete: missing {', '.join(miss)}" if miss else ""))
    if not (kit / "nvngx_dlssnr.dll").exists():
        info("note: nvngx_dlssnr.dll (the NR model) is not in the kit; installs will skip it")
    return 1 if miss else 0


# ----------------------------------------------------------------------------- install plan
def manifest_path(game: Game, exe_dir: Path | None = None) -> Path:
    return game.path / MANIFEST_DIR / "manifest.json"


def load_manifest(game: Game) -> dict | None:
    p = manifest_path(game)
    return json.loads(p.read_text()) if p.exists() else None


def build_plan(game: Game, a: dict, args) -> dict:
    kit = kit_dir()
    miss = kit_check(kit)
    if miss:
        die(f"kit at {kit} is incomplete (missing {', '.join(miss)}); run `ostool kit init --from-game DIR`")
    exe_dir: Path = a["exe_dir"]
    profile = merge_profiles(args.profile)

    fg = args.fg
    if fg == "auto":
        fg = "separate" if a["has_dlss_g"] and (kit / "mod/version.dll").exists() else "off"
    if fg == "separate" and not (kit / "mod/version.dll").exists():
        die("--fg separate needs the frame-gen mod in the kit (mod/version.dll)")

    items: list[tuple[Path, str]] = []  # (source, destination relative to exe_dir)
    for src, dst in KIT_FILES.items():
        if (kit / src).exists():
            items.append((kit / src, dst))
    for d in KIT_DIRS:
        if (kit / d).is_dir():
            for f in sorted((kit / d).rglob("*")):
                if f.is_file():
                    items.append((f, str(f.relative_to(kit))))
    if fg == "separate":
        for src, dst in MOD_FILES.items():
            if (kit / src).exists():
                items.append((kit / src, dst))

    ini_text = render_ini((kit / INI_TEMPLATE).read_bytes().decode("utf-8", errors="replace"),
                          profile, args.set or [])
    files, conflicts = [], []
    for src, rel in items:
        dst = exe_dir / rel
        entry = {"rel": rel, "src": str(src), "sha256": sha256(src), "size": src.stat().st_size}
        if dst.exists():
            if sha256(dst) == entry["sha256"]:
                entry["action"] = "identical"
            else:
                entry["action"] = "replaced"
                conflicts.append(rel)
        else:
            entry["action"] = "added"
        files.append(entry)
    ini_dst = exe_dir / "OptiScaler.ini"
    ini_entry = {"rel": "OptiScaler.ini", "content": ini_text,
                 "sha256": hashlib.sha256(ini_text.encode()).hexdigest(), "size": len(ini_text)}
    if ini_dst.exists():
        ini_entry["action"] = "replaced"
        conflicts.append("OptiScaler.ini")
    else:
        ini_entry["action"] = "added"
    files.append(ini_entry)

    overrides = ["dxgi"] + (["version"] if fg == "separate" else [])
    return {"exe_dir": exe_dir, "files": files, "conflicts": conflicts, "fg": fg,
            "overrides": overrides, "profile": args.profile}


def print_plan(game: Game, a: dict, plan: dict, args) -> None:
    exe = a["exe"]
    info(f"game:    {game.name} ({game.appid})")
    info(f"exe:     {exe.relative_to(game.path)}   (64-bit; dx12={a['dx12']}, "
         f"DLSS files={a['dlss_files'] or 'none'}, DLSS-G={'yes' if a['has_dlss_g'] else 'no'})")
    info(f"prefix:  {game.prefix or 'not found (game never launched with Proton?)'}")
    info(f"profile: {plan['profile']}   frame gen: {plan['fg']}")
    n = {"added": 0, "replaced": 0, "identical": 0}
    for f in plan["files"]:
        n[f["action"]] += 1
    total = sum(f["size"] for f in plan["files"] if f["action"] != "identical")
    info(f"files:   {n['added']} to add, {n['replaced']} to replace, {n['identical']} already identical "
         f"({human(total)})")
    key = [f for f in plan["files"] if "/" not in f["rel"] and "\\" not in f["rel"]]
    for f in key:
        info(f"  {f['action']:<9} {f['rel']}")
    if a["notes"]:
        info("notes:   " + "; ".join(a["notes"]))
    if plan["conflicts"]:
        info("CONFLICTS (already present and different): " + ", ".join(plan["conflicts"][:8]))
    ovr = ";".join(f"{n}=n,b" for n in plan["overrides"])
    info(f'launch options:  WINEDLLOVERRIDES="{ovr}" PROTON_NVIDIA_NVCUDA=1 %command%')
    info(f"   or: --edit-registry to write the override into the prefix's user.reg")


def cmd_install(args) -> int:
    game = resolve_game(args.game)
    if args.prefix:
        game.prefix = Path(args.prefix).expanduser()
        if not (game.prefix / "user.reg").exists():
            die(f"{game.prefix} is not a Wine prefix (no user.reg)")
    override = None
    if args.exe:
        override = Path(args.exe)
        override = override if override.is_absolute() else game.path / override
        if not override.is_file() or pe_machine(override) != 0x8664:
            die(f"{override} is not a 64-bit exe")
    a = assess(game, override)
    if a["exe"] is None:
        die("no 64-bit exe found in that game (use --exe)")
    if load_manifest(game):
        die("already installed here; run `ostool uninstall` first (upgrades are not supported yet)")
    plan = build_plan(game, a, args)
    print_plan(game, a, plan, args)
    hard = []
    if not a.get("dx12") and not args.force:
        hard.append("not a DirectX 12 game (use --force to override)")
    if a["anticheat"] and not args.force:
        hard.append("anti-cheat detected (use --force at your own risk)")
    existing = [p for p in ("dxgi.dll", "version.dll" if plan["fg"] == "separate" else "")
                if p and any(f["rel"] == p and f["action"] == "replaced" for f in plan["files"])]
    if existing:
        hard.append(f"{', '.join(existing)} already exists and is not ours (another mod?); "
                    "installing would replace it (use --force to back it up and replace)")
    if plan["conflicts"] and not args.force and not existing:
        hard.append("some files already exist and differ (use --force to back them up and replace)")
    if args.edit_registry:
        if game.prefix is None:
            hard.append("no Proton prefix found for this game")
        elif wineserver_using(game.prefix):
            hard.append("a wineserver is running for this prefix; close the game/Steam and retry")
        elif game_running(a["exe"].name):
            hard.append("the game is running")
    if hard:
        info("\nCANNOT INSTALL:\n  - " + "\n  - ".join(hard))
        return 3
    if not args.apply:
        info("\ndry run: nothing was changed. Re-run with --apply to install.")
        return 0
    return do_install(game, a, plan, args)


def do_install(game: Game, a: dict, plan: dict, args) -> int:
    exe_dir: Path = plan["exe_dir"]
    mdir = game.path / MANIFEST_DIR
    bdir = mdir / "files"
    done: list[dict] = []
    created_dirs: list[str] = []
    reg_rec = None
    reg_backup = None
    try:
        mdir.mkdir(parents=True, exist_ok=True)
        for f in plan["files"]:
            dst = exe_dir / f["rel"]
            if f["action"] == "identical":
                continue
            cur = exe_dir
            for part in Path(f["rel"]).parent.parts:
                cur = cur / part
                if not cur.exists():
                    created_dirs.append(str(cur))
                    cur.mkdir()
            if f["action"] == "replaced":
                (bdir / f["rel"]).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(dst, bdir / f["rel"])
                f["backup"] = str(Path("files") / f["rel"])
            if "content" in f:
                dst.write_bytes(f["content"].encode("utf-8"))
            else:
                shutil.copy2(f["src"], dst)
            done.append(f)
        if args.edit_registry and game.prefix:
            ureg = game.prefix / "user.reg"
            reg_backup = mdir / "user.reg.bak"
            shutil.copy2(ureg, reg_backup)
            text = ureg.read_bytes().decode("utf-8", errors="surrogateescape")
            new, reg_rec = reg_apply(text, a["exe"].name, plan["overrides"])
            ureg.write_bytes(new.encode("utf-8", errors="surrogateescape"))
            reg_rec["file"] = str(ureg)
            reg_rec["exe"] = a["exe"].name
            reg_rec["sha256_after"] = sha256(ureg)
            reg_rec["backup"] = "user.reg.bak"
    except Exception as e:  # roll back whatever was done
        info(f"\nFAILED ({e}); rolling back")
        rollback(exe_dir, mdir, done, created_dirs, reg_rec)
        return 4
    manifest = {
        "tool_version": TOOL_VERSION, "installed_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "game": game.name, "appid": game.appid, "exe": str(a["exe"].relative_to(game.path)),
        "exe_dir": str(exe_dir.relative_to(game.path)), "profile": plan["profile"], "fg": plan["fg"],
        "kit": str(kit_dir()), "created_dirs": created_dirs, "registry": reg_rec,
        "files": [{k: v for k, v in f.items() if k not in ("content", "src")} for f in plan["files"]],
    }
    manifest_path(game).write_text(json.dumps(manifest, indent=2))
    info(f"\ninstalled {len([f for f in plan['files'] if f['action'] != 'identical'])} files. "
         f"Manifest: {manifest_path(game)}")
    ovr = ";".join(f"{n}=n,b" for n in plan["overrides"])
    if reg_rec:
        info("registry override written; no launch options needed")
    else:
        info(f'set the Steam launch options to:  WINEDLLOVERRIDES="{ovr}" PROTON_NVIDIA_NVCUDA=1 %command%')
    info("then run the game once and `ostool verify` it.")
    return 0


def rollback(exe_dir: Path, mdir: Path, done: list[dict], created_dirs: list[str], reg_rec) -> None:
    for f in reversed(done):
        dst = exe_dir / f["rel"]
        if f.get("backup") and (mdir / f["backup"]).exists():
            shutil.copy2(mdir / f["backup"], dst)
        elif dst.exists():
            dst.unlink()
    for d in sorted(created_dirs, key=len, reverse=True):
        try:
            Path(d).rmdir()
        except OSError:
            pass
    if reg_rec and (mdir / "user.reg.bak").exists():
        shutil.copy2(mdir / "user.reg.bak", reg_rec["file"])
    shutil.rmtree(mdir, ignore_errors=True)


# ----------------------------------------------------------------------------- uninstall / verify
def cmd_uninstall(args) -> int:
    game = resolve_game(args.game)
    m = load_manifest(game)
    if not m:
        die("no ostool install found for that game")
    exe_dir = game.path / m["exe_dir"]
    mdir = game.path / MANIFEST_DIR
    problems, actions = [], []
    for f in m["files"]:
        dst = exe_dir / f["rel"]
        if f["action"] == "identical":
            continue
        if not dst.exists():
            problems.append(f"missing: {f['rel']}")
        elif sha256(dst) != f["sha256"] and not args.force:
            problems.append(f"changed since install: {f['rel']} (use --force to remove/restore anyway)")
        actions.append(f)
    reg = m.get("registry")
    if reg:
        ureg = Path(reg["file"])
        if wineserver_using(ureg.parent) or game_running(Path(m["exe"]).name):
            problems.append("game or wineserver is running; close it to undo the registry change")
    info(f"{m['game']}: installed {m['installed_at']} (profile {m['profile']}, frame gen {m['fg']})")
    info(f"will remove {sum(1 for f in actions if f['action'] == 'added')} added files and restore "
         f"{sum(1 for f in actions if f['action'] == 'replaced')} replaced files"
         + ("; undo the registry override" if reg else ""))
    if problems:
        info("CANNOT UNINSTALL:\n  - " + "\n  - ".join(problems[:12]))
        return 3
    if not args.apply:
        info("dry run: nothing was changed. Re-run with --apply to uninstall.")
        return 0
    for f in reversed(actions):
        dst = exe_dir / f["rel"]
        if f["action"] == "replaced" and f.get("backup"):
            shutil.copy2(mdir / f["backup"], dst)
        elif dst.exists():
            dst.unlink()
    for d in sorted(m.get("created_dirs", []), key=len, reverse=True):
        try:
            Path(d).rmdir()
        except OSError:
            pass
    if reg:
        ureg = Path(reg["file"])
        if sha256(ureg) == reg["sha256_after"] and (mdir / reg["backup"]).exists():
            shutil.copy2(mdir / reg["backup"], ureg)  # untouched since: restore byte-for-byte
        else:
            text = ureg.read_bytes().decode("utf-8", errors="surrogateescape")
            ureg.write_bytes(reg_undo(text, reg["exe"], reg).encode("utf-8", errors="surrogateescape"))
    shutil.rmtree(mdir, ignore_errors=True)
    try:
        (game.path / "_mod_backups").rmdir()
    except OSError:
        pass
    info("uninstalled.")
    return 0


def cmd_verify(args) -> int:
    game = resolve_game(args.game)
    m = load_manifest(game)
    if not m:
        die("no ostool install found for that game")
    exe_dir = game.path / m["exe_dir"]
    bad = 0
    info(f"{m['game']}  (profile {m['profile']}, frame gen {m['fg']}, installed {m['installed_at']})")
    for f in m["files"]:
        dst = exe_dir / f["rel"]
        if not dst.exists():
            info(f"  FAIL  missing {f['rel']}")
            bad += 1
        elif f["action"] != "identical" and sha256(dst) != f["sha256"]:
            info(f"  WARN  changed {f['rel']}")
    info(f"  files: {'ok' if not bad else str(bad) + ' problem(s)'}")

    log = exe_dir / "OptiScaler.log"
    if not log.exists():
        info("  log:   no OptiScaler.log yet: run the game once, then verify again")
    else:
        text = log.read_bytes().decode("utf-8", errors="replace")
        signals = [("proxy loaded", r"OptiScaler working as (\w+)\.dll"),
                   ("NR model created", r"DLSS-NR model feature created"),
                   ("NR running", r"DLSS-NR running after")]
        if "PeripheralWarpEnabled = true" in (exe_dir / "OptiScaler.ini").read_text(errors="replace"):
            signals.append(("warp active", r"PeripheralWarp: .* native -> .* work"))
        for label, pat in signals:
            hit = re.search(pat, text)
            info(f"  {'ok  ' if hit else 'MISS'}  {label}" + (f" ({hit.group(0)[:60]})" if hit else ""))
        errs = [l for l in text.split("\n") if "[E]" in l and "streamlineLogCallback" not in l]
        info(f"  errors (excluding Streamline noise): {len(errs)}")
        for l in errs[:5]:
            info("     " + l[:160])
        for l in text.split("\n"):
            if re.search(r"not used --|falling back|skipping|unsupported", l, re.I):
                info("  note: " + l[l.find("]") + 2:][:160])
    if m["fg"] == "separate":
        logs = sorted((exe_dir / "dlssg_sm86" / "logs").glob("loader_*.jsonl"), key=lambda p: p.stat().st_mtime) \
            if (exe_dir / "dlssg_sm86" / "logs").is_dir() else []
        if logs:
            last = None
            for line in logs[-1].read_text(errors="replace").split("\n"):
                if '"event":"configuration"' in line.replace(" ", ""):
                    last = line
            act = re.search(r'"active":"([^"]+)"', last or "")
            info(f"  frame gen mod: log {logs[-1].name}" + (f", active proxy {act.group(1)}" if act else ""))
        else:
            info("  frame gen mod: no log yet (needs the game run with frame generation enabled)")
    if args.kernel:
        try:
            out = subprocess.run(["journalctl", "-k", "-b", "0", "--no-pager"], capture_output=True,
                                 text=True, timeout=30).stdout
            x = [l for l in out.split("\n") if "NVRM: Xid" in l]
            info(f"  kernel: {len(x)} NVIDIA Xid fault(s) this boot" + ("" if not x else ": " + x[-1][-120:]))
        except (OSError, subprocess.SubprocessError):
            info("  kernel: could not read the kernel log")
    return 1 if bad else 0


def cmd_list(args) -> int:
    n = 0
    for g in games():
        m = load_manifest(g)
        if m:
            n += 1
            info(f"{g.appid:>8}  {g.name}  profile={m['profile']} fg={m['fg']} since {m['installed_at']}")
    if not n:
        info("no installs")
    return 0


# ----------------------------------------------------------------------------- self test
def tree_hash(root: Path) -> str:
    h = hashlib.sha256()
    for p in sorted(root.rglob("*")):
        h.update(str(p.relative_to(root)).encode())
        if p.is_file():
            h.update(sha256(p).encode())
    return h.hexdigest()


def cmd_selftest(args) -> int:
    ok = True

    def check(cond: bool, what: str) -> None:
        nonlocal ok
        print(f"  {'ok  ' if cond else 'FAIL'}  {what}")
        ok = ok and cond

    tmp = Path(tempfile.mkdtemp(prefix="ostool-selftest-"))
    try:
        steam, lib = tmp / "steam", tmp / "lib"
        (steam / "steamapps").mkdir(parents=True)
        (steam / "steamapps/libraryfolders.vdf").write_text(
            f'"libraryfolders"\n{{\n\t"0"\n\t{{\n\t\t"path"\t\t"{lib}"\n\t}}\n}}\n')
        sa = lib / "steamapps"
        game = sa / "common/FakeGame"
        exe_dir = game / "bin/x64_DX12"
        exe_dir.mkdir(parents=True)
        (game / "bin/x64").mkdir(parents=True)
        (sa / "appmanifest_999.acf").write_text(
            '"AppState"\n{\n\t"appid"\t\t"999"\n\t"name"\t\t"Fake Game"\n\t"installdir"\t\t"FakeGame"\n}\n')
        pe = bytearray(b"MZ" + b"\0" * 0x3E)
        pe[0x3C:0x40] = (0x80).to_bytes(4, "little")
        pe += b"\0" * (0x80 - len(pe)) + b"PE\0\0" + (0x8664).to_bytes(2, "little") + b"\0" * 200
        (exe_dir / "Fake-Win64-Shipping.exe").write_bytes(bytes(pe) + b"d3d12.dll nvngx " * 4 + b"\0" * 5000)
        (game / "bin/x64/Fake-Big.exe").write_bytes(bytes(pe) + b"\0" * 90000)  # bigger, but DX11
        for n in ("nvngx_dlss.dll", "sl.dlss_g.dll", "sl.interposer.dll"):
            (exe_dir / n).write_bytes(b"x")
        (exe_dir / "game.cfg").write_text("keep me")
        prefix = sa / "compatdata/999/pfx"
        prefix.mkdir(parents=True)
        (prefix / "user.reg").write_text('WINE REGISTRY Version 2\n\n[Software\\\\Wine\\\\Other] 1\n"a"="b"\n')
        kit = tmp / "kit"
        (kit / "OptiScaler").mkdir(parents=True)
        (kit / "peripheral_warp").mkdir()
        (kit / "mod").mkdir()
        (kit / "OptiScaler.dll").write_bytes(b"OptiScaler-dll")
        (kit / "nvngx.dll_dlssnr.dll").write_bytes(b"fwd")
        (kit / "nvngx_dlssnr.dll").write_bytes(b"model")
        (kit / "OptiScaler/libx.dll").write_bytes(b"lib")
        (kit / "mod/version.dll").write_bytes(b"dlssg mod")
        (kit / "mod/dlssg_sm86.ini").write_text("[General]\nEnabled=1\n")
        for s in WARP_SHADERS:
            (kit / f"peripheral_warp/{s}").write_bytes(b"dxbc")
        (kit / INI_TEMPLATE).write_text("[FrameGen]\r\nExternal = auto\r\n\r\n[ProcessFilter]\r\n"
                                        "TargetProcessName = Cyberpunk2077.exe\r\n\r\n[Upscalers]\r\n"
                                        "Dx11Upscaler = auto\r\n\r\n[DlssNr]\r\nPeripheralWarpEnabled = auto\r\n")
        os.environ.update({"OSTOOL_STEAM_ROOTS": str(steam), "OSTOOL_KIT": str(kit),
                           "OSTOOL_NO_PROCCHECK": "1"})
        before = tree_hash(game)
        reg_before = (prefix / "user.reg").read_bytes()

        check(any(g.appid == "999" for g in games()), "discovers the fake Steam game")
        a = assess(games()[0])
        check(a["ok"] and a["has_dlss_g"], "recognises it as DX12 + DLSS + DLSS-G")
        check(a["exe"].parent == exe_dir, "prefers the DX12 exe over a bigger one")
        rc = main(["install", "999", "--exe", "bin/x64/Fake-Big.exe"])
        check(rc == 3, "--exe pointing at a non-DX12 exe is refused")
        rc = main(["install", "999", "--exe", "bin/x64/Fake-Big.exe", "--force"])
        check(rc == 0, "--exe with --force is accepted (dry run)")

        rc = main(["install", "999", "--profile", "warp"])
        check(rc == 0 and tree_hash(game) == before, "dry run changes nothing")

        (exe_dir / "dxgi.dll").write_bytes(b"someone else's mod")
        rc = main(["install", "999", "--apply"])
        check(rc == 3, "refuses to overwrite an existing dxgi.dll")
        (exe_dir / "dxgi.dll").unlink()
        before = tree_hash(game)

        rc = main(["install", "999", "--apply", "--edit-registry", "--profile", "warp,bg3-dx11",
                   "--set", "FrameGen.External=true"])
        check(rc == 0, "install --apply succeeds")
        check((exe_dir / "dxgi.dll").read_bytes() == b"OptiScaler-dll", "proxy dll installed as dxgi.dll")
        ini = (exe_dir / "OptiScaler.ini").read_bytes().decode()
        check("PeripheralWarpEnabled = true" in ini and "External = true" in ini and "\r\n" in ini,
              "ini rendered with profile + --set and CRLF kept")
        check("TargetProcessName = auto" in ini and "Cyberpunk2077" not in ini,
              "another game's exe name is stripped from the ini")
        check("Dx11Upscaler = dlss_12" in ini, "profiles can be combined (warp + bg3-dx11)")
        reg = (prefix / "user.reg").read_text()
        check('"dxgi"="native,builtin"' in reg and '"version"="native,builtin"' in reg
              and "AppDefaults\\\\Fake-Win64-Shipping.exe" in reg, "registry override written")
        check(main(["verify", "999"]) == 0, "verify passes")
        (exe_dir / "game.cfg").write_text("player edit")
        (exe_dir / "game.cfg").write_text("keep me")

        rc = main(["uninstall", "999", "--apply"])
        check(rc == 0, "uninstall --apply succeeds")
        check(tree_hash(game) == before, "game folder is byte-identical after uninstall")
        check((prefix / "user.reg").read_bytes() == reg_before, "registry restored byte-for-byte")

        # user.reg changed by someone else after our edit: surgical undo must keep their change
        main(["install", "999", "--apply", "--edit-registry"])
        with open(prefix / "user.reg", "a") as f:
            f.write('\n[Software\\\\Wine\\\\Later] 2\n"z"="1"\n')
        main(["uninstall", "999", "--apply"])
        after = (prefix / "user.reg").read_text()
        check("Later" in after and "AppDefaults" not in after, "surgical registry undo keeps later edits")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("SELFTEST " + ("PASSED" if ok else "FAILED"))
    return 0 if ok else 1


# ----------------------------------------------------------------------------- main
def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="ostool", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("detect")
    d.add_argument("--all", action="store_true")
    k = sub.add_parser("kit")
    ksub = k.add_subparsers(dest="kit_cmd", required=True)
    ki = ksub.add_parser("init")
    ki.add_argument("--from-game", required=True, help="folder holding the working setup (the game's exe dir)")
    ki.add_argument("--force", action="store_true")
    ki.add_argument("--dry-run", action="store_true")
    ksub.add_parser("show")
    i = sub.add_parser("install")
    i.add_argument("game")
    i.add_argument("--profile", default="default",
                   help=f"one or more, comma separated (e.g. warp,bg3-dx11): {', '.join(PROFILES)}")
    i.add_argument("--exe", help="the game exe to install beside (default: the DX12 or largest 64-bit exe)")
    i.add_argument("--prefix", help="Wine prefix, for games that are not Steam library entries")
    i.add_argument("--fg", default="auto", choices=["auto", "separate", "off"])
    i.add_argument("--set", action="append", metavar="Section.Key=Value")
    i.add_argument("--edit-registry", action="store_true")
    i.add_argument("--force", action="store_true")
    i.add_argument("--apply", action="store_true")
    u = sub.add_parser("uninstall")
    u.add_argument("game")
    u.add_argument("--apply", action="store_true")
    u.add_argument("--force", action="store_true")
    v = sub.add_parser("verify")
    v.add_argument("game")
    v.add_argument("--kernel", action="store_true")
    sub.add_parser("list")
    sub.add_parser("selftest")
    args = ap.parse_args(argv)
    return {"detect": cmd_detect, "kit": cmd_kit, "install": cmd_install, "uninstall": cmd_uninstall,
            "verify": cmd_verify, "list": cmd_list, "selftest": cmd_selftest}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
