import hashlib
import importlib.util
from pathlib import Path
import stat
import zipfile
import pytest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("import_sdk", ROOT / "scripts/import-sdk.py")
sdk_import = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sdk_import)


def test_wrong_archive_does_not_create_destination(tmp_path):
    archive = tmp_path / "wrong.zip"
    archive.write_bytes(b"not the pinned SDK")
    destination = tmp_path / "sdk"
    with pytest.raises(ValueError, match="checksum mismatch"):
        sdk_import.import_sdk(archive, destination)
    assert not destination.exists()


@pytest.mark.parametrize("name", ["root/../escape", "/absolute", "root/C:stream", "root\\escape", "other/file"])
def test_unsafe_zip_paths_are_rejected(name):
    # ZipInfo's constructor normalizes backslashes on Windows; exercise the
    # validator itself with a raw filename on either host platform.
    info = zipfile.ZipInfo("placeholder")
    info.filename = name
    with pytest.raises(ValueError, match="Unsafe"):
        sdk_import.validate_member(info, "root/")


def test_symlinks_are_rejected():
    info = zipfile.ZipInfo("root/link")
    info.external_attr = (stat.S_IFLNK | 0o777) << 16
    with pytest.raises(ValueError, match="symlinks"):
        sdk_import.validate_member(info, "root/")


def test_verified_archive_import_preserves_files_and_existing_work(tmp_path, monkeypatch):
    archive = tmp_path / "fixture.zip"
    prefix = "fixture-" + sdk_import.SDK_COMMIT + "/"
    with zipfile.ZipFile(archive, "w") as zipped:
        zipped.writestr(prefix + "SDK/Makefile", "all:\n\t@true\n")
        info = zipfile.ZipInfo(prefix + "SDK/tools/run.sh")
        info.external_attr = (stat.S_IFREG | 0o755) << 16
        zipped.writestr(info, "#!/bin/sh\nexit 0\n")
    monkeypatch.setattr(sdk_import, "SDK_SHA256", hashlib.sha256(archive.read_bytes()).hexdigest())
    git_calls = []
    monkeypatch.setattr(sdk_import.subprocess, "run", lambda command, **kwargs: git_calls.append(command))
    destination = tmp_path / "sdk"
    sdk_import.import_sdk(archive, destination)
    assert (destination / "SDK/Makefile").read_text() == "all:\n\t@true\n"
    assert (destination / ".badge-sdk-archive.json").exists()
    assert len(git_calls) == 3
    assert git_calls[-1][-3:] == ["add", "origin", "https://gitlab.zh-jieli.com/e_badge/e_badge_707_sdk_200.git"]
    before = (destination / "SDK/Makefile").read_bytes()
    with pytest.raises(ValueError, match="must be empty"):
        sdk_import.import_sdk(archive, destination)
    assert (destination / "SDK/Makefile").read_bytes() == before
