#!/usr/bin/env python3
"""Import the exact vendor SDK ZIP as a local working tree, without Git cloning.

SDK contents stay outside commits. The published branch contains only this
importer, the vendor URL and checksums. No account credentials are required.
"""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import shutil
import stat
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SDK_REL = "tools/community-re/jieli-sdks/e_badge_707_sdk_200"
SDK_COMMIT = "d0167685d032d745d88fe50233302edd46941622"
SDK_SHA256 = "bdad4eee49456f3d2d3abf1a763b14d8bb0b0fd6f1fde104d7504fc364350683"


def validate_member(info, prefix):
    path = PurePosixPath(info.filename)
    if (path.is_absolute() or ".." in path.parts or "\\" in info.filename
            or any(":" in part for part in path.parts) or not info.filename.startswith(prefix)):
        raise ValueError(f"Unsafe SDK archive entry: {info.filename}")
    mode = info.external_attr >> 16
    if stat.S_ISLNK(mode):
        raise ValueError("SDK archive symlinks are not accepted")
    return PurePosixPath(info.filename[len(prefix):])


def import_sdk(archive, destination):
    with archive.open("rb") as source:
        hasher = hashlib.sha256()
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            hasher.update(chunk)
        digest = hasher.hexdigest()
    if digest != SDK_SHA256:
        raise ValueError(f"SDK archive checksum mismatch: {digest}; expected {SDK_SHA256}")
    if destination.exists() and any(destination.iterdir()):
        raise ValueError("SDK destination must be empty; existing work is never overwritten")
    with zipfile.ZipFile(archive) as zipped:
        infos = zipped.infolist()
        roots = {item.filename.split("/")[0] for item in infos}
        if len(roots) != 1:
            raise ValueError("Expected one SDK root directory")
        prefix = roots.pop() + "/"
        if SDK_COMMIT not in prefix or prefix + "SDK/Makefile" not in zipped.namelist():
            raise ValueError("Not the pinned E87 SDK archive")
        # Validate every entry before creating any output files.
        members = [(item, validate_member(item, prefix)) for item in infos]
        destination.mkdir(parents=True, exist_ok=True)
        for item, relative in members:
            target = destination / Path(*relative.parts)
            if item.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with zipped.open(item) as source, target.open("xb") as output:
                shutil.copyfileobj(source, output)
            mode = item.external_attr >> 16
            target.chmod(0o755 if mode & 0o111 else 0o644)
    # git apply requires a repository rooted here when inside the parent clone.
    # No vendor commit is invented; provenance is the verified vendor ZIP hash.
    subprocess.run(["git", "init", "--quiet", str(destination)], check=True)
    subprocess.run(["git", "-C", str(destination), "config", "core.autocrlf", "false"], check=True)
    (destination / ".badge-sdk-archive.json").write_text(
        json.dumps({"vendor_commit": SDK_COMMIT, "archive_sha256": digest}, indent=2), encoding="utf8")
    print(f"SDK archive verified and imported ({len(members)} entries)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--destination", type=Path, default=ROOT / SDK_REL)
    args = parser.parse_args()
    try:
        import_sdk(args.archive, args.destination)
    except (OSError, ValueError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        parser.exit(1, f"SDK import failed: {error}\n")
