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
    exe = Path(folder) / ("display-test.exe" if os.name == "nt" else "display-test")
    subprocess.run(command + ["-std=c99", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                   "-I", str(root / "firmware/display"), str(root / "tests/display_native.c"),
                   "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
