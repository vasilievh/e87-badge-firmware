"""Verify system integration anchors against the pinned patch and reject drift."""
import importlib.util
from pathlib import Path
import pytest
from test_display_overlay import overlay as display, postimage

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("system_overlay", ROOT / "scripts/prepare-badge-system.py")
system = importlib.util.module_from_spec(spec)
spec.loader.exec_module(system)


@pytest.fixture
def sdk(tmp_path):
    for name in (display.UI, display.QIX, system.ADV, system.CFG):
        target = tmp_path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(postimage(name), encoding="utf8")
    # This unchanged vendor line is outside the upstream patch's hunks. Keep the
    # exact anchor from pinned SDK d0167685; the real clean SDK build validates it too.
    cfg = tmp_path / system.CFG
    cfg.write_text('    sprintf(bt_cfg.edr_name, "%s", TCFG_JL_WALLPAPER_ADV_NAME);\n'
                   + cfg.read_text(encoding="utf8"), encoding="utf8")
    display.prepare(tmp_path)
    return tmp_path


def test_layers_are_idempotent_and_validate_together(sdk):
    system.prepare(sdk)
    system.check(sdk)
    display.check(sdk)
    before = (sdk / system.UI).read_bytes()
    system.prepare(sdk)
    display.prepare(sdk)
    assert (sdk / system.UI).read_bytes() == before
    ui = before.decode()
    assert "badge_status_build();" in ui and "badge_system_poll();" in ui
    assert "while (g_charge_active)" not in ui
    assert '#include "lock_wallpaper.h"' not in ui and 'g_lock_img_buf' not in ui
    assert '#include "badge_wallpaper.h"' in ui and 'if (wp) {' in ui
    assert '#define QIX_STAGE_END     0x2A0000u' in (sdk / system.QIX).read_text(encoding="utf8")
    assert "badge_update_battery_gate()) { return; }   /* OTA" in ui


def test_mismatched_advertisement_does_not_partially_write(sdk):
    adv = sdk / system.ADV
    adv.write_text(adv.read_text(encoding="utf8").replace('const char *nm = "EC-MOUSE";', 'const char *nm = "other";'), encoding="utf8")
    before = (sdk / system.UI).read_bytes()
    with pytest.raises(ValueError, match="occurrences"):
        system.prepare(sdk)
    assert (sdk / system.UI).read_bytes() == before
    assert not (sdk / system.MANIFEST).exists()


def test_check_rejects_identity_source_drift(sdk):
    system.prepare(sdk)
    cfg = sdk / system.CFG
    cfg.write_bytes(cfg.read_bytes() + b"\n/* unexpected edit */\n")
    with pytest.raises(ValueError, match="outside"):
        system.check(sdk)
