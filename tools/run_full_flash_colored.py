#!/usr/bin/env python3
from __future__ import annotations

import argparse
import ctypes
import os
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path

ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")

RESET = "\033[0m"
COLORS = {
    "red": "\033[91m",
    "yellow": "\033[93m",
    "green": "\033[92m",
    "cyan": "\033[96m",
    "magenta": "\033[95m",
    "gray": "\033[90m",
    "darkyellow": "\033[33m",
    "darkcyan": "\033[36m",
    "white": "\033[97m",
}


def enable_windows_vt() -> None:
    if os.name != "nt":
        return
    try:
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.GetStdHandle(-11)
        mode = ctypes.c_uint32()
        if kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            kernel32.SetConsoleMode(handle, mode.value | 0x0004)
    except Exception:
        pass


def strip_ansi(s: str) -> str:
    return ANSI_RE.sub("", s)


def color_for(line: str) -> str:
    s = line.lower()
    if any(x in s for x in ("fatal", "error", " failed", "fail", "timeout", "ksd:err", "command rx timeout")):
        return COLORS["red"]
    if any(x in s for x in ("warning", "warn", "full 1 mb", "erase", "deprecation warning")):
        return COLORS["yellow"]
    if any(x in s for x in ("success", "done", "ok:", "ksd:ok", "ksd:size", "esp flash result: ok", "kesp:")):
        return COLORS["green"]
    if any(x in s for x in ("ksd:", "sd-uart", "command rx", "flash.json", " get ", " put ", "reset")):
        return COLORS["cyan"]
    if any(x in s for x in ("esp-uart", "ets jan", "boot mode", "~ld")):
        return COLORS["magenta"]
    if any(x in line for x in ("�", "☻", "☺")):
        return COLORS["darkyellow"]
    if "[sd]" in s:
        return COLORS["darkcyan"]
    if "[main]" in s:
        return COLORS["gray"]
    return COLORS["white"]


class Runner:
    def __init__(self, port: str, no_full_flash: bool):
        self.port = port
        self.no_full_flash = no_full_flash
        self.esp_repo = Path(__file__).resolve().parents[1]
        workspace = self.esp_repo.parent
        self.k210_repo = workspace / "K210_AI_V7s_Plus"
        if not self.k210_repo.exists():
            self.k210_repo = Path(r"D:\w_space\K210_AI_V7s_Plus")
        if not self.k210_repo.exists():
            raise RuntimeError(r"K210 repo not found. Expected sibling repo or D:\w_space\K210_AI_V7s_Plus")

        log_dir = self.esp_repo / "logs" / "one_click"
        log_dir.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        self.log_path = log_dir / f"full_flash_{stamp}.log"
        self.log_file = self.log_path.open("w", encoding="utf-8", newline="\n")

    def close(self) -> None:
        self.log_file.flush()
        self.log_file.close()

    def log(self, line: str = "") -> None:
        clean = strip_ansi(line.rstrip("\r\n"))
        self.log_file.write(clean + "\n")
        self.log_file.flush()
        sys.stdout.write(color_for(clean) + clean + RESET + "\n")
        sys.stdout.flush()

    def run_step(self, title: str, cwd: Path, command: str) -> None:
        self.log("")
        self.log(f"========== {title} ==========")
        self.log(f"DIR: {cwd}")
        self.log(f"CMD: {command}")

        proc = subprocess.Popen(
            ["cmd.exe", "/d", "/c", command],
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        assert proc.stdout is not None
        for line in proc.stdout:
            self.log(line)
        rc = proc.wait()
        self.log(f"EXIT: {rc}")
        if rc != 0:
            raise RuntimeError(f"Step failed: {title}, exit code {rc}")

    def run(self) -> None:
        self.log("=== K210 + ESP8285 one-click colored runner ===")
        self.log(f"Port: {self.port}")
        self.log(f"ESP repo: {self.esp_repo}")
        self.log(f"K210 repo: {self.k210_repo}")
        self.log(f"Log file: {self.log_path}")

        self.run_step("K210 git pull", self.k210_repo, "git pull")
        self.run_step("K210 build", self.k210_repo, "build_k210.bat")
        self.run_step("K210 flash", self.k210_repo, f"flash_k210.bat {self.port}")

        self.run_step("ESP git pull", self.esp_repo, "git pull")
        self.run_step("ESP payload build", self.esp_repo, "build_esp_payload.bat")

        upload_args = self.port
        if not self.no_full_flash:
            upload_args += " --full-flash"
        self.run_step("ESP payload upload through K210", self.esp_repo, f"upload_esp_payload_uart.bat {upload_args}")

        self.log("")
        self.log("ALL STEPS DONE")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM12")
    parser.add_argument("--no-full-flash", action="store_true")
    args = parser.parse_args()

    enable_windows_vt()
    runner = Runner(args.port, args.no_full_flash)
    rc = 0
    try:
        runner.run()
    except KeyboardInterrupt:
        rc = 130
        runner.log("")
        runner.log("FATAL: interrupted by user")
    except Exception as exc:
        rc = 1
        runner.log("")
        runner.log(f"FATAL: {exc}")
    finally:
        log_path = runner.log_path
        runner.close()
        sys.stdout.write("\n")
        sys.stdout.write(COLORS["cyan"] + "Saved log:\n" + RESET)
        sys.stdout.write(COLORS["cyan"] + str(log_path) + "\n" + RESET)
        sys.stdout.write(COLORS["yellow"] + "Send this .log file to ChatGPT.\n" + RESET)
        sys.stdout.flush()
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
