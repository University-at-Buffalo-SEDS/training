#!/usr/bin/env python3
"""
CrashNBurn CMake build + flash helper.
Adapted from ../analog_tears/build.py on 2026-09-15.
Local changes: explicit ARM toolchain, folder-local root, no telemetry option.

Features
- Detect repo root by searching upwards for CMakeLists.txt
- Read project name from CMakeLists.txt `project(<name> ...)`
- Build Debug/Release with a toolchain file
- Generate .bin from .elf via arm-none-eabi-objcopy
- Flash via:
    * dfu-util (DFU)
    * st-flash (ST-LINK USB dongle, stlink tools)
    * st-util + arm-none-eabi-gdb (ST-LINK server + GDB "load")
    * ST-LINK_gdbserver + arm-none-eabi-gdb (CubeProgrammer gdbserver / ST-LINK server)
    * STM32_Programmer_CLI (STM32CubeProgrammer direct flash)
- Friendly, non-traceback error messages by default

Usage examples
  ./build.py build --debug
  ./build.py build --release
  ./build.py flash --debug --method st-flash
  ./build.py flash --release --method st-util
"""
from __future__ import annotations

import argparse
import os
import re
import shlex
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional


# ---------------------------
# UI / formatting (emoji-safe)
# ---------------------------

def _supports_unicode() -> bool:
    enc = (sys.stdout.encoding or "").lower()
    if "utf" not in enc:
        return False
    # If piped, some envs lie about encoding; keep conservative:
    return sys.stdout.isatty()


@dataclass(frozen=True)
class UI:
    emoji: str  # "auto" | "on" | "off"

    def sym(self, kind: str) -> str:
        use_emoji = (self.emoji == "on") or (self.emoji == "auto" and _supports_unicode())
        if not use_emoji:
            return {
                "ok": "[OK]",
                "warn": "[WARN]",
                "err": "[ERR]",
                "run": "[RUN]",
                "info": "[INFO]",
            }.get(kind, "[*]")
        return {
            "ok": "✅",
            "warn": "⚠️ ",
            "err": "❌",
            "run": "▶️ ",
            "info": "ℹ️ ",
        }.get(kind, "•")

    def say(self, kind: str, msg: str) -> None:
        print(f"{self.sym(kind)} {msg}")


# ---------------------------
# Errors (no raw tracebacks)
# ---------------------------

class FriendlyError(RuntimeError):
    pass


def die(ui: UI, msg: str, code: int = 2) -> None:
    ui.say("err", msg)
    raise SystemExit(code)


def _wrap_unhandled(ui: UI):
    def excepthook(exc_type, exc, tb):
        # For FriendlyError and SystemExit, let normal flow handle it.
        if isinstance(exc, SystemExit):
            raise exc
        if isinstance(exc, FriendlyError):
            die(ui, str(exc), code=2)
        # Otherwise, hide traceback and show hint.
        die(ui, f"Unexpected error: {exc_type.__name__}: {exc}\n"
                f"Tip: re-run with --trace to see a full traceback.", code=2)
    return excepthook


# ---------------------------
# Utilities
# ---------------------------

def which(cmd: str) -> Optional[str]:
    from shutil import which as _which
    return _which(cmd)


def run(ui: UI, cmd: list[str], cwd: Optional[Path] = None, env: Optional[dict[str, str]] = None) -> None:
    ui.say("run", " ".join(shlex.quote(c) for c in cmd))
    try:
        subprocess.run(cmd, check=True, cwd=str(cwd) if cwd else None, env=env)
    except FileNotFoundError:
        raise FriendlyError(f"Command not found: {cmd[0]}\n"
                            f"Make sure it's installed and on PATH.")
    except subprocess.CalledProcessError as e:
        raise FriendlyError(f"Command failed (exit {e.returncode}): {cmd[0]}\n"
                            f"See output above for details.")


def popen(ui: UI, cmd: list[str], cwd: Optional[Path] = None) -> subprocess.Popen:
    ui.say("run", " ".join(shlex.quote(c) for c in cmd))
    try:
        return subprocess.Popen(cmd, cwd=str(cwd) if cwd else None)
    except FileNotFoundError:
        raise FriendlyError(f"Command not found: {cmd[0]}\n"
                            f"Make sure it's installed and on PATH.")


def find_repo_root(start: Path) -> Path:
    p = start.resolve()
    if (p / "CMakeLists.txt").is_file():
        return p
    raise FriendlyError(
        f"No CMakeLists.txt in {p}. Generate the CubeMX CMake/GCC project "
        "here (Generate under root), then rerun ./build.py build."
    )


# --- CMake parsing helpers ---
# We intentionally keep this parser "simple but practical" (no full CMake eval),
# covering the patterns most embedded templates use (CubeMX included).

_COMMENT_RE = re.compile(r"(?m)#.*$")

# Matches: set(VAR value) or set(VAR "value")
_SET_RE = re.compile(
    r"(?is)\bset\s*\(\s*([A-Za-z0-9_]+)\s+(.+?)\s*\)",
)

# Matches: project(name ...) or project(${VAR} ...)
_PROJECT_CALL_RE = re.compile(
    r"(?is)\bproject\s*\(\s*([^\s\)]+)",
)

_VAR_REF_RE = re.compile(r"^\$\{([A-Za-z0-9_]+)\}$")


def _strip_quotes(s: str) -> str:
    s = s.strip()
    if (len(s) >= 2) and ((s[0] == s[-1] == '"') or (s[0] == s[-1] == "'")):
        return s[1:-1].strip()
    return s


def parse_project_name(cmakelists: Path) -> str:
    raw = cmakelists.read_text(encoding="utf-8", errors="replace")
    text = _COMMENT_RE.sub("", raw)

    # 1) Collect simple set(VAR value) assignments.
    vars: dict[str, str] = {}
    for m in _SET_RE.finditer(text):
        var = m.group(1).strip()
        rhs = m.group(2).strip()

        # Take the first token of rhs as the variable's value.
        # This handles: set(CMAKE_PROJECT_NAME Valve_Board26)
        # and ignores CubeMX noise like CACHE/STRING/FORCE if present.
        token = rhs.split()[0] if rhs else ""
        token = _strip_quotes(token)

        if token:
            vars[var] = token

    # 2) If the file sets CMAKE_PROJECT_NAME explicitly, prefer that.
    if "CMAKE_PROJECT_NAME" in vars:
        return vars["CMAKE_PROJECT_NAME"]

    # 3) Otherwise, try to parse project(<first-arg>) and resolve ${VAR}.
    pm = _PROJECT_CALL_RE.search(text)
    if pm:
        first_arg = _strip_quotes(pm.group(1).strip())
        vm = _VAR_REF_RE.match(first_arg)
        if vm:
            var = vm.group(1)
            if var in vars:
                return vars[var]
            raise FriendlyError(
                f"Found project({first_arg} ...) in {cmakelists}, but {var} wasn't set to a simple value.\n"
                f"Tip: add a line like: set({var} MyProjectName)\n"
                f"Or pass --project MyProjectName to the script."
            )
        return first_arg

    # 4) Give a helpful failure message with hints.
    raise FriendlyError(
        f"Couldn't parse project name from {cmakelists}\n"
        f"Expected either:\n"
        f"  - project(MyProject ...)\n"
        f"  - set(CMAKE_PROJECT_NAME MyProject) then project(${{CMAKE_PROJECT_NAME}})\n"
        f"Tip: you can override with --project <name>."
    )
def pick_elf(build_dir: Path, preferred_name: Optional[str]) -> Path:
    if preferred_name:
        p = build_dir / f"{preferred_name}.elf"
        if p.exists():
            return p

    # fallback: any *.elf in build dir (common for embedded)
    elfs = sorted(build_dir.glob("*.elf"))
    if len(elfs) == 1:
        return elfs[0]
    if len(elfs) > 1:
        names = ", ".join(e.name for e in elfs[:10])
        raise FriendlyError(
            f"Multiple .elf files found in {build_dir}: {names}\n"
            f"Pass --artifact <name> to select one (without extension)."
        )
    raise FriendlyError(
        f"No .elf produced in {build_dir}.\n"
        f"Tip: check your CMake target output or pass --artifact <name>."
    )


def wait_port(host: str, port: int, timeout_s: float = 5.0) -> None:
    deadline = time.time() + timeout_s
    last_err = None
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return
        except OSError as e:
            last_err = e
            time.sleep(0.1)
    raise FriendlyError(f"Timed out waiting for {host}:{port} to open ({timeout_s:.1f}s). Last error: {last_err}")


# ---------------------------
# Build
# ---------------------------

@dataclass
class BuildConfig:
    repo_root: Path
    build_type: str  # "Debug" | "Release"
    generator: str
    toolchain_file: Path
    build_subdir: str
    project_name: str
    artifact: Optional[str]  # base name without extension (if known/forced)

    @property
    def build_dir(self) -> Path:
        return self.repo_root / "build" / self.build_subdir


def configure_and_build(ui: UI, cfg: BuildConfig) -> tuple[Path, Path]:
    cfg.build_dir.mkdir(parents=True, exist_ok=True)


    run(ui, [
        "cmake",
        f"-DCMAKE_BUILD_TYPE={cfg.build_type}",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        f"-DCMAKE_TOOLCHAIN_FILE={str(cfg.toolchain_file)}",
        "-S", str(cfg.repo_root),
        "-B", str(cfg.build_dir),
        "-G", cfg.generator,
    ], cwd=cfg.repo_root)

    run(ui, ["cmake", "--build", str(cfg.build_dir), "--parallel"], cwd=cfg.repo_root)

    # Find elf, then objcopy -> bin
    elf = pick_elf(cfg.build_dir, cfg.artifact or cfg.project_name)
    bin_path = elf.with_suffix(".bin")

    # Prefer arm-none-eabi-objcopy but allow override via env
    objcopy = os.environ.get("OBJCOPY", "arm-none-eabi-objcopy")
    run(ui, [objcopy, "-O", "binary", str(elf), str(bin_path)], cwd=cfg.repo_root)

    ui.say("ok", f"Built: {elf.name} -> {bin_path.name}")
    return elf, bin_path


# ---------------------------
# Flashing
# ---------------------------

def flash_dfu(ui: UI, bin_path: Path, addr: str) -> None:
    if which("dfu-util") is None:
        raise FriendlyError("dfu-util not found.\n"
                            "Install it (e.g., apt-get install dfu-util, brew install dfu-util) "
                            "or use --method st-flash / st-util.")
    run(ui, ["dfu-util", "-a", "0", "-s", addr, "-D", str(bin_path)])


def flash_st_flash(ui: UI, bin_path: Path, addr: str, reset: bool) -> None:
    # st-flash comes from stlink tools
    if which("st-flash") is None:
        raise FriendlyError("st-flash not found.\n"
                            "Install STLink tools (stlink). On Ubuntu: apt-get install stlink-tools. "
                            "On macOS: brew install stlink.")
    cmd = ["st-flash"]
    if reset:
        cmd.append("--reset")
    cmd += ["write", str(bin_path), addr]
    run(ui, cmd)


def flash_stm32prog_cli(
    ui: UI,
    bin_path: Path,
    addr: str,
    reset: bool,
    stm32prog_cli: str | None,
    connect: str,
    extra_args: list[str],
) -> None:
    exe = (
        stm32prog_cli
        or os.environ.get("STM32_PROGRAMMER_CLI")
        or which("STM32_Programmer_CLI")
        or which("STM32ProgrammerCLI")
    )
    if exe is None:
        raise FriendlyError("STM32_Programmer_CLI not found.\n"
                            "Install STM32CubeProgrammer and add it to PATH, "
                            "or pass --stm32prog-cli /path/to/STM32_Programmer_CLI.")
    cmd = [exe, "-c", connect, "-w", str(bin_path), addr, "-v"]
    if reset:
        cmd.append("-rst")
    cmd += extra_args
    run(ui, cmd)


def flash_via_gdb(ui: UI, elf_path: Path, host: str, port: int, gdb: str, extra_gdb_cmds: list[str] | None = None) -> None:
    if which(gdb) is None:
        raise FriendlyError(f"{gdb} not found.\n"
                            "Install the ARM GNU toolchain that provides arm-none-eabi-gdb "
                            "or pass --gdb <path>.")
    cmds = [
        "set confirm off",
        "set pagination off",
        f"target extended-remote {host}:{port}",
        "monitor reset halt",
        "load",
        "monitor reset run",
        "quit",
    ]
    if extra_gdb_cmds:
        # Insert after connection
        cmds = cmds[:4] + extra_gdb_cmds + cmds[4:]
    # -batch-silent hides extra noise but still prints errors
    run(ui, [gdb, "-q", "-batch", *sum([["-ex", c] for c in cmds], []), str(elf_path)])


def flash_st_util(ui: UI, elf_path: Path, gdb: str, host: str, port: int, st_util_args: list[str]) -> None:
    # st-util comes from stlink tools; it provides a GDB server (default :4242).
    if which("st-util") is None:
        raise FriendlyError("st-util not found.\n"
                            "Install STLink tools (stlink). On Ubuntu: apt-get install stlink-tools. "
                            "On macOS: brew install stlink.")
    proc = popen(ui, ["st-util", *st_util_args])
    try:
        wait_port(host, port, timeout_s=8.0)
        flash_via_gdb(ui, elf_path, host, port, gdb)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            proc.kill()


def flash_stlink_gdbserver(ui: UI, elf_path: Path, gdb: str, host: str, port: int, gdbserver: str, gdbserver_args: list[str]) -> None:
    # Common names: ST-LINK_gdbserver (CubeProgrammer) or ST-LINK_gdbserver.exe on Windows.
    if which(gdbserver) is None:
        raise FriendlyError(f"{gdbserver} not found.\n"
                            "Install STM32CubeProgrammer (for ST-LINK_gdbserver) or provide --gdbserver <path>.\n"
                            "Alternatively use --method st-flash.")
    proc = popen(ui, [gdbserver, *gdbserver_args])
    try:
        wait_port(host, port, timeout_s=10.0)
        # Some servers don't support monitor reset; keep it, but allow override
        flash_via_gdb(ui, elf_path, host, port, gdb)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            proc.kill()


# ---------------------------
# CLI
# ---------------------------

def make_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="build.py",
        description="Build (CMake/Ninja) and flash firmware with friendly errors.",
    )
    p.add_argument("--trace", action="store_true", help="Show full Python tracebacks on errors.")
    p.add_argument("--emoji", choices=["auto", "on", "off"], default="auto",
                   help="UI symbols. 'auto' uses emoji only when it looks safe.")
    p.add_argument("--toolchain", default=None,
                   help="Toolchain file path (default: <repo>/cmake/gcc-arm-none-eabi.cmake)")
    p.add_argument("--generator", default="Ninja", help="CMake generator (default: Ninja)")
    p.add_argument("--artifact", default=None,
                   help="Base name of output artifact (without extension), if not equal to project name.")
    p.add_argument("--project", default=None,
                   help="Override project name (otherwise read from CMakeLists.txt).")
    p.add_argument("--build-subdir", default=None,
                   help="Build folder name under ./build (default: Debug_Script or Release_Script)")


    # Build-mode flags are accepted on subcommands (and we also support them before/after via argv normalization).

    sub = p.add_subparsers(dest="cmd", required=True)

    def add_mode_and_common(sp: argparse.ArgumentParser) -> None:
        mode = sp.add_mutually_exclusive_group()
        mode.add_argument("--debug", action="store_true", help="Debug build (default).")
        mode.add_argument("--release", action="store_true", help="Release build.")

    b = sub.add_parser("build", help="Configure + build + objcopy to .bin")
    add_mode_and_common(b)

    f = sub.add_parser("flash", help="Build then flash")
    add_mode_and_common(f)

    f.add_argument("--method", choices=["dfu", "st-flash", "st-util", "stlink-gdbserver", "stm32prog-cli"], default="st-flash",
                   help="Flashing method.")
    f.add_argument("--addr", default="0x08000000", help="Flash base address (default: 0x08000000)")
    f.add_argument("--no-reset", action="store_true", help="Do not reset after flash (st-flash/stm32prog-cli).")

    # st-util options
    f.add_argument("--host", default="127.0.0.1", help="GDB server host (default: 127.0.0.1)")
    f.add_argument("--port", type=int, default=None,
                   help="GDB server port (st-util default 4242; gdbserver varies).")
    f.add_argument("--gdb", default="arm-none-eabi-gdb", help="GDB executable (default: arm-none-eabi-gdb)")
    f.add_argument("--st-util-args", default="", help="Extra args for st-util (quoted string).")
    f.add_argument("--gdbserver", default="ST-LINK_gdbserver", help="GDB server executable (default: ST-LINK_gdbserver)")
    f.add_argument("--gdbserver-args", default="", help="Extra args for gdbserver (quoted string).")
    f.add_argument("--stm32prog-cli", default=None,
                   help="STM32CubeProgrammer CLI executable path (default: env STM32_PROGRAMMER_CLI or PATH).")
    f.add_argument("--stm32prog-connect", default="port=SWD",
                   help="STM32_Programmer_CLI -c argument value (default: port=SWD).")
    f.add_argument("--stm32prog-args", default="",
                   help="Extra args for STM32_Programmer_CLI (quoted string).")

    return p


def build_cfg_from_args(ui: UI, args: argparse.Namespace) -> BuildConfig:
    script_dir = Path(__file__).resolve().parent
    repo_root = find_repo_root(script_dir)

    cmakelists = repo_root / "CMakeLists.txt"
    project_name = args.project or parse_project_name(cmakelists)

    build_type = "Release" if args.release else "Debug"
    build_subdir = args.build_subdir
    if build_subdir is None:
        build_subdir = "Release_Script" if build_type == "Release" else "Debug_Script"

    toolchain = Path(args.toolchain) if args.toolchain else (repo_root / "cmake" / "gcc-arm-none-eabi.cmake")
    if not toolchain.is_absolute():
        toolchain = repo_root / toolchain
    toolchain = toolchain.resolve()
    if not toolchain.exists():
        raise FriendlyError(f"Toolchain file not found: {toolchain}\n"
                            f"Pass --toolchain <path> to set it explicitly.")

    return BuildConfig(
        repo_root=repo_root,
        build_type=build_type,
        generator=args.generator,
        toolchain_file=toolchain,
        build_subdir=build_subdir,
        project_name=project_name,
        artifact=args.artifact,
    )


def main() -> None:
    parser = make_parser()
    args = parser.parse_args()

    ui = UI(emoji=args.emoji)

    if not args.trace:
        sys.excepthook = _wrap_unhandled(ui)

    cfg = build_cfg_from_args(ui, args)

    if args.cmd == "build":
        configure_and_build(ui, cfg)
        return

    if args.cmd == "flash":
        elf, bin_path = configure_and_build(ui, cfg)

        method = args.method
        addr = args.addr

        if method == "dfu":
            flash_dfu(ui, bin_path, addr)
        elif method == "st-flash":
            flash_st_flash(ui, bin_path, addr, reset=(not args.no_reset))
        elif method == "st-util":
            port = args.port or 4242
            st_args = shlex.split(args.st_util_args) if args.st_util_args else []
            flash_st_util(ui, elf, args.gdb, args.host, port, st_args)
        elif method == "stlink-gdbserver":
            # Reasonable default port used by some gdbservers (override with --port).
            port = args.port or 61234
            gs_args = shlex.split(args.gdbserver_args) if args.gdbserver_args else []
            # If user didn't specify port in args, try to nudge server via args when possible.
            # We won't guess vendor-specific flags; user can pass them in --gdbserver-args.
            flash_stlink_gdbserver(ui, elf, args.gdb, args.host, port, args.gdbserver, gs_args)
        elif method == "stm32prog-cli":
            sp_args = shlex.split(args.stm32prog_args) if args.stm32prog_args else []
            flash_stm32prog_cli(
                ui,
                bin_path,
                addr,
                reset=(not args.no_reset),
                stm32prog_cli=args.stm32prog_cli,
                connect=args.stm32prog_connect,
                extra_args=sp_args,
            )
        else:
            raise FriendlyError(f"Unknown method: {method}")

        ui.say("ok", f"Flashed using method: {method}")
        return

    raise FriendlyError("No command provided. Use 'build' or 'flash'.")


if __name__ == "__main__":
    main()
