#!/usr/bin/env python3
"""Compile/run the actual display C headers with a native compiler, no SDK needed."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
command = [cc] if cc else [sys.executable, "-m", "ziglang", "cc"]
with tempfile.TemporaryDirectory(prefix="badge-display-") as folder:
    for source in ("display_native.c", "system_native.c", "wallpaper_native.c"):
        exe = Path(folder) / (source + (".exe" if os.name == "nt" else ".out"))
        subprocess.run(command + ["-std=c99", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                       "-I", str(root / "firmware/display"), str(root / "tests" / source),
                       "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
