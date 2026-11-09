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
    for name in (display.UI, display.QIX, system.ADV, system.CFG, system.CONF):
        target = tmp_path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(postimage(name), encoding="utf8")
    # This unchanged vendor line is outside the upstream patch's hunks. Keep the
    # exact anchor from pinned SDK d0167685; the real clean SDK build validates it too.
    cfg = tmp_path / system.CFG
    cfg.write_text('    sprintf(bt_cfg.edr_name, "%s", TCFG_JL_WALLPAPER_ADV_NAME);\n'
                   + cfg.read_text(encoding="utf8"), encoding="utf8")
    # Unchanged vendor TE configuration that badge_safety.h depends on (pinned SDK).
    for name, text in ((system.SDK_CONFIG, "#define TCFG_LCD_PIN_TE IO_PORTC_00 // LCD TE\r\n"),
                       (system.BOARD_CFG, "#define TCFG_LCD_TE_USED_PEND 1\n"
                                          "#define TCFG_LCD_TE_IO      TCFG_LCD_PIN_TE\n")):
        target = tmp_path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(text.encode())
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
    qix=(sdk / system.QIX).read_text(encoding="utf8")
    assert '#define BADGE_RESOURCES_IMPLEMENTATION' in qix
    assert 'badge_resource_command(cmd,p,plen)' in qix
    assert 'badge_resource_protect_raw(cmd,p,plen)' in qix
    assert 'badge_resource_abort();' in qix
    assert 'int badge_resources_lock_update(void)' in ui
    assert 'if (!locked) g_on_update_scr=0;' in ui
    for target in system.RESOURCE_TARGETS:
        assert (sdk / target).read_bytes() == (ROOT / 'firmware/display/badge_resources.h').read_bytes()
    assert "badge_update_battery_gate()) { return; }   /* OTA" in ui
    assert '#include "badge_fonts.h"' in ui and "badge_fonts_init();" in ui
    assert '#define BADGE_SAFETY_IMPLEMENTATION\n#include "badge_safety.h"' in ui
    assert ui.index('g_on_update_scr;') < ui.index('#include "badge_safety.h"')
    assert ui.index('volatile u8 g_badge_update_active;') < ui.index('#include "badge_safety.h"')
    assert ui.count('badge_safety_ui_tick();') == 1 and ui.count('badge_safety_start();') == 1
    assert ui.index('badge_safety_start();') < ui.index('badge_safety_ui_tick();')
    assert 'if (cmd == 0x97) { badge_safety_command(cmd, p, plen); return; }' in qix
    assert qix.index('cmd == 0x97') < qix.index('if (g_badge_display_mode)')
    assert 'qix_send_frame(0x98, data, len);' in qix
    assert (sdk / system.HEADERS['badge_safety.h']).read_bytes() == (ROOT / 'firmware/display/badge_safety.h').read_bytes()
    assert ui.index('#include "badge_fonts.h"') < ui.index('#include "badge_display_ui.h"')
    conf = (sdk / system.CONF).read_text(encoding="utf8")
    assert "#define LV_FONT_MONTSERRAT_36 0   /* BADGE_SYSTEM_OVERLAY_V1 */ /* titles use the flash font pack */\n" in conf
    assert "#define LV_FONT_MONTSERRAT_36 1" not in conf


def test_mismatched_advertisement_does_not_partially_write(sdk):
    adv = sdk / system.ADV
    adv.write_text(adv.read_text(encoding="utf8").replace('const char *nm = "EC-MOUSE";', 'const char *nm = "other";'), encoding="utf8")
    before = (sdk / system.UI).read_bytes()
    with pytest.raises(ValueError, match="occurrences"):
        system.prepare(sdk)
    assert (sdk / system.UI).read_bytes() == before
    assert not (sdk / system.MANIFEST).exists()


def test_check_rejects_font_config_drift(sdk):
    system.prepare(sdk)
    conf = sdk / system.CONF
    conf.write_text(conf.read_text(encoding="utf8").replace("MONTSERRAT_36 0", "MONTSERRAT_36 1", 1), encoding="utf8")
    with pytest.raises(ValueError, match="outside"):
        system.check(sdk)


def test_check_rejects_identity_source_drift(sdk):
    system.prepare(sdk)
    cfg = sdk / system.CFG
    cfg.write_bytes(cfg.read_bytes() + b"\n/* unexpected edit */\n")
    with pytest.raises(ValueError, match="outside"):
        system.check(sdk)


def test_te_pin_configuration_drift_is_rejected_before_writes(sdk):
    cfg = sdk / system.BOARD_CFG
    cfg.write_text(cfg.read_text(encoding="utf8").replace("TE_USED_PEND 1", "TE_USED_PEND 0"),
                   encoding="utf8")
    before = (sdk / system.UI).read_bytes()
    with pytest.raises(ValueError, match="TE pin configuration"):
        system.prepare(sdk)
    assert (sdk / system.UI).read_bytes() == before
