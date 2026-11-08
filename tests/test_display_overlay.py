"""Integration anchors are checked against post-images of the pinned upstream patch.

This does NOT emulate the SDK compiler. It exercises the exact added/context lines
from the real patch, including idempotence and modified-source rejection.
"""
import importlib.util
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("prepare_display", ROOT / "scripts/prepare-display.py")
overlay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(overlay)


def postimage(filename):
    lines = (ROOT / "patches/badge/badge-menu.patch").read_text(encoding="utf8").splitlines()
    active = in_hunk = False
    result = []
    for line in lines:
        if line.startswith("diff --git "):
            active = line == f"diff --git a/{filename} b/{filename}"
            in_hunk = False
        elif active and line.startswith("@@"):
            in_hunk = True
        elif active and in_hunk and line[:1] in (" ", "+"):
            result.append(line[1:])
    assert result, filename
    return "\n".join(result) + "\n"


@pytest.fixture
def sdk(tmp_path):
    for name in (overlay.UI, overlay.QIX):
        target = tmp_path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(postimage(name), encoding="utf8")
    return tmp_path


def test_applies_then_is_idempotent_and_checkable(sdk):
    overlay.prepare(sdk)
    overlay.check(sdk)
    before = (sdk / overlay.UI).read_bytes()
    overlay.prepare(sdk)
    assert (sdk / overlay.UI).read_bytes() == before
    ui = before.decode()
    assert "g_menu_btns[5]" in ui and "Bluetooth Display" in ui
    assert "badge_display_tick();" in ui
    assert (sdk / overlay.UI).with_name("badge_display_ui.h").is_file()
    qix = (sdk / overlay.QIX).read_text(encoding="utf8")
    assert qix.index("if (g_badge_display_mode)") < qix.index("if (qix_raw_flash_handle(")
    assert "g_badge_display_mode = 0; /* includes USB/charge exits */" in ui


def test_source_mismatch_does_not_partially_modify_sdk(sdk):
    target = sdk / overlay.UI
    target.write_text(target.read_text(encoding="utf8").replace("g_menu_btns[4]", "g_menu_btns[99]"), encoding="utf8")
    qix_before = (sdk / overlay.QIX).read_bytes()
    with pytest.raises(ValueError, match="occurrences"):
        overlay.prepare(sdk)
    assert (sdk / overlay.QIX).read_bytes() == qix_before
    assert not (sdk / overlay.MANIFEST).exists()


def test_check_detects_source_drift(sdk):
    overlay.prepare(sdk)
    target = sdk / overlay.QIX
    target.write_bytes(target.read_bytes() + b"\n/* changed */\n")
    with pytest.raises(ValueError, match="outside"):
        overlay.check(sdk)
