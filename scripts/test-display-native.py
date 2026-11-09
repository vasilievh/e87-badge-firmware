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
    # The font harness also runs against a freshly generated pack when Pillow and
    # fontTools are installed (as in CI); otherwise only its erased-slot checks run.
    pack = Path(folder) / "fonts.bin"
    try:
        subprocess.run([sys.executable, str(root / "tools/make_badge_fonts.py"), str(pack)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    except subprocess.CalledProcessError as error:
        if b"No module named" not in error.stderr:
            sys.stderr.write(error.stderr.decode(errors="replace"))
            raise
        print("fonts: Pillow/fontTools missing; generated-pack checks skipped")
    for source in ("display_native.c", "system_native.c", "wallpaper_native.c",
                   "fonts_native.c", "resources_native.c", "idle_native.c", "safety_native.c"):
        exe = Path(folder) / (source + (".exe" if os.name == "nt" else ".out"))
        subprocess.run(command + ["-std=c99", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                       "-I", str(root / "firmware/display"), str(root / "tests" / source),
                       "-o", str(exe)], check=True)
        arguments = [str(pack)] if source == "fonts_native.c" and pack.exists() else []
        subprocess.run([str(exe), *arguments], check=True)
