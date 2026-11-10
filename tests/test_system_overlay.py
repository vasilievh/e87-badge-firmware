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
    # Unchanged vendor TE configuration that badge_safety.h depends on (pinned SDK).
    for name, text in ((system.SDK_CONFIG, "#define TCFG_LCD_PIN_TE IO_PORTC_00 // LCD TE\r\n"),
                       (system.BOARD_CFG, "#define TCFG_LCD_TE_USED_PEND 1\n"
                                          "#define TCFG_LCD_TE_IO      TCFG_LCD_PIN_TE\n"
                                          "#define   WDT_APP_INIT_TIME\t\t\tWDT_16S\t\t//\r\n"
                                          "#define   WDT_APP_RUN_TIME\t\t\tWDT_LRC_4S\t//\r\n"),
                       (system.APP_MAIN, "    //\n    wdt_init(WDT_APP_RUN_TIME);\n")):
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
    gui = ui[ui.index('void jl_gui_init(void)'):]
    assert gui.index('badge_safety_start();') < gui.index('badge_menu_build();') < gui.index('badge_lock_enter();')
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


def test_idle_keeps_key_scanner_and_leaves_clock_to_the_sdk(sdk):
    system.prepare(sdk)
    ui = (sdk / system.UI).read_text(encoding="utf8")
    enter = ui[ui.index("static void badge_lock_enter("):ui.index("static void badge_lock_wake(")]
    wake = ui[ui.index("static void badge_lock_wake("):ui.index("/* Any swipe on the lock image")]
    # lcd_sleep_ctrl() already releases/re-takes the ui_show clock lock.
    assert "clock_unlock" not in enter and "clock_lock" not in wake
    assert "badge_idle_release_clock" not in ui and "badge_idle_restore_clock" not in ui
    assert enter.index("badge_idle_prepare_lock();") < enter.index("g_badge_locked = 1;")
    assert wake.index("badge_idle_exit();") < wake.index("lcd_sleep_ctrl(0)")
    # Both watchdog hooks coexist with the idle wait (~1 loop/s, floor 0.5/s).
    assert ui.count("badge_safety_ui_tick();") == 1 and ui.count("badge_safety_start();") == 1
    assert "if (g_badge_locked) badge_idle_wait();" in ui
    assert "if (badge_idle_take_key_wake() || g_badge_adkey_kv != NO_KEY)" in ui
    assert "os_time_dly(3);   /* ~30 ms poll" not in ui
    # Stage marks around the sleep and wake steps, in order, and nothing else moved.
    assert enter.index("badge_stage(BS_STAGE_SLEEP_ENTER);") < enter.index("lcd_sleep_ctrl(1);") \
        < enter.index("badge_stage(BS_STAGE_ASLEEP);")
    assert wake.index("badge_stage(BS_STAGE_WAKE_POWER);") < wake.index("lcd_sleep_ctrl(0);") \
        < wake.index("badge_stage(BS_STAGE_WAKE_REINIT);") < wake.index("lcd_reinit_wait_finish();") \
        < wake.index("badge_stage(BS_STAGE_WAKE_DONE);") < wake.index("lcd_drv_backlight_ctrl(g_bright_pct")
    draw = ui.index("badge_stage(g_badge_locked ? BS_STAGE_ASLEEP_DRAW : BS_STAGE_DRAW);")
    assert draw < ui.index("lv_timer_handler();", draw) \
        < ui.index("badge_stage(g_badge_locked ? BS_STAGE_ASLEEP : BS_STAGE_AWAKE);", draw)
    gui = ui[ui.index("void jl_gui_init(void)"):]
    assert gui.index("badge_lock_enter();") < gui.index("badge_stage(BS_STAGE_ASLEEP_DRAW);")
    assert ui.index('#include "badge_safety.h"') < ui.index("badge_stage(")
    assert ui.count("badge_stage(") == 10
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


@pytest.mark.parametrize("name,old,new", [
    ("BOARD_CFG", "WDT_LRC_4S", "WDT_LRC_32S"),
    ("BOARD_CFG", "WDT_16S", "WDT_32S"),
    ("APP_MAIN", "wdt_init(WDT_APP_RUN_TIME);", "/* wdt_init(WDT_APP_RUN_TIME); */"),
])
def test_hardware_watchdog_drift_is_rejected_before_writes(sdk, name, old, new):
    path = sdk / getattr(system, name)
    path.write_text(path.read_text(encoding="utf8").replace(old, new), encoding="utf8")
    before = (sdk / system.UI).read_bytes()
    with pytest.raises(ValueError, match="Hardware watchdog"):
        system.prepare(sdk)
    assert (sdk / system.UI).read_bytes() == before


def test_te_pin_configuration_drift_is_rejected_before_writes(sdk):
    cfg = sdk / system.BOARD_CFG
    cfg.write_text(cfg.read_text(encoding="utf8").replace("TE_USED_PEND 1", "TE_USED_PEND 0"),
                   encoding="utf8")
    before = (sdk / system.UI).read_bytes()
    with pytest.raises(ValueError, match="TE pin configuration"):
        system.prepare(sdk)
    assert (sdk / system.UI).read_bytes() == before
