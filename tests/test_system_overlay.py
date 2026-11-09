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
    for name in (display.UI, display.QIX, system.ADV, system.CFG, system.CONF, system.BOARD, system.ADKEY):
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
    assert '#include "badge_fonts.h"' in ui and "badge_fonts_init();" in ui
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


def test_idle_keeps_key_scanner_and_orders_clock_changes_around_lcd_power(sdk):
    system.prepare(sdk)
    ui = (sdk / system.UI).read_text(encoding="utf8")
    enter = ui[ui.index("static void badge_lock_enter("):ui.index("static void badge_lock_wake(")]
    wake = ui[ui.index("static void badge_lock_wake("):ui.index("/* Any swipe on the lock image")]
    assert enter.index("lcd_sleep_ctrl(1)") < enter.index("badge_idle_release_clock();")
    assert enter.index("badge_idle_prepare_lock();") < enter.index("g_badge_locked = 1;")
    assert wake.index("badge_idle_exit();") < wake.index("badge_idle_restore_clock();") < wake.index("lcd_sleep_ctrl(0)")
    assert "if (g_badge_locked) badge_idle_wait();" in ui
    assert "if (badge_idle_take_key_wake() || g_badge_adkey_kv != NO_KEY)" in ui
    assert "os_time_dly(3);   /* ~30 ms poll" not in ui
    board = (sdk / system.BOARD).read_text(encoding="utf8")
    assert board.index("badge_idle_event_init(&badge_touch_event);") < board.index('task_create(axs_poll_task, NULL, "axs_poll")')
    assert "badge_idle_event_wait(&badge_touch_event, 0)" in board
    assert "if (!was_enabled) badge_idle_event_signal(&badge_touch_event)" in board
    assert (sdk / system.BOARD).with_name("badge_idle_event.h").read_bytes() == (ROOT / "firmware/display/badge_idle_event.h").read_bytes()
    adkey = (sdk / system.ADKEY).read_text(encoding="utf8")
    assert "if (g_badge_adkey_kv != kv)" in adkey and "badge_idle_key_changed();" in adkey
    # Power diagnostics are read-only and bypass the Display mode's command filter.
    qix = (sdk / system.QIX).read_text(encoding="utf8")
    assert qix.index("if (cmd == 0x92)") < qix.index("if (g_badge_display_mode)")
    assert "qix_send_frame(0x93, data, len);" in qix


@pytest.mark.parametrize("source,old,new", [
    (system.BOARD, "if (g_tp_reinit_req && g_axs_want)", "if (changed)"),
    (system.ADKEY, "g_badge_adkey_kv = kv;", "g_badge_adkey_kv = 0;"),
])
def test_idle_source_drift_rejected_before_any_sdk_write(sdk, source, old, new):
    path = sdk / source
    path.write_text(path.read_text(encoding="utf8").replace(old, new), encoding="utf8")
    before = {name: (sdk / name).read_bytes() for name in (system.UI, system.QIX, system.ADKEY, system.BOARD)}
    with pytest.raises(ValueError, match="occurrences"):
        system.prepare(sdk)
    assert before == {name: (sdk / name).read_bytes() for name in before}
    assert not (sdk / system.MANIFEST).exists()


def test_check_covers_board_copy_of_semaphore_header(sdk):
    system.prepare(sdk)
    target = (sdk / system.BOARD).with_name("badge_idle_event.h")
    target.write_bytes(target.read_bytes() + b"\n/* drift */\n")
    with pytest.raises(ValueError, match="outside"):
        system.check(sdk)
