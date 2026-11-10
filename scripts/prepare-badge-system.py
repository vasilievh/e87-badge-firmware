#!/usr/bin/env python3
"""Apply battery status, device-local identity, wallpaper and font pack after Display v1.

No personal settings/assets are read by this build step. Device provisioning is
separate. Exact anchors and a manifest reject source drift before any writes.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / "tools/community-re/jieli-sdks/e_badge_707_sdk_200"
UI = "SDK/apps/common/ui/lvgl_v8/lvgl_main.c"
ADV = "SDK/apps/common/third_party_profile/jieli/rcsp/adv/ble_rcsp_adv.c"
CFG = "SDK/apps/watch/user_cfg.c"
QIX = "SDK/apps/common/third_party_profile/jieli/rcsp/qix_ota_server.c"
CONF = "SDK/apps/common/ui/lvgl_v8/lv_conf.h"
BOARD = "SDK/apps/watch/board/br35/board_ac707n_demo/board_ac707n_demo.c"
ADKEY = "SDK/apps/common/device/key/adkey.c"
SDK_CONFIG = "SDK/apps/watch/board/br35/sdk_config.h"
BOARD_CFG = "SDK/apps/watch/board/br35/board_ac707n_demo/board_ac707n_demo_cfg.h"
APP_MAIN = "SDK/apps/watch/app_main.c"
MANIFEST = ".badge-system-overlay.json"
HEADERS = {
    "badge_system_ui.h": str(Path(UI).with_name("badge_system_ui.h")),
    "badge_power_status.h": str(Path(UI).with_name("badge_power_status.h")),
    "badge_identity.h": str(Path(CFG).with_name("badge_identity.h")),
    "badge_wallpaper.h": str(Path(UI).with_name("badge_wallpaper.h")),
    "badge_fonts.h": str(Path(UI).with_name("badge_fonts.h")),
    "badge_idle_ui.h": str(Path(UI).with_name("badge_idle_ui.h")),
    "badge_idle_status.h": str(Path(UI).with_name("badge_idle_status.h")),
    "badge_idle_event.h": str(Path(UI).with_name("badge_idle_event.h")),
    "badge_safety.h": str(Path(UI).with_name("badge_safety.h")),
    "badge_resources_wire.h": str(Path(QIX).with_name("badge_resources_wire.h")),
}
RESOURCE_TARGETS = [str(Path(name).with_name("badge_resources.h")) for name in (UI, CFG, QIX)]
MARK = "/* BADGE_SYSTEM_OVERLAY_V1 */"


def header_files():
    yield from HEADERS.items()
    yield "badge_idle_event.h", str(Path(BOARD).with_name("badge_idle_event.h"))


def digest(data):
    return hashlib.sha256(data).hexdigest()


def replace(text, old, new, count=1):
    found = text.count(old)
    if found != count:
        raise ValueError(f"Expected {count} occurrences, found {found}: {old[:80]!r}")
    return text.replace(old, new)


def check(sdk):
    record = json.loads((sdk / MANIFEST).read_text(encoding="utf8"))
    if record["display_manifest_sha256"] != digest((sdk / ".badge-display-overlay.json").read_bytes()):
        raise ValueError("Display overlay changed; recreate the SDK before applying system overlay")
    for name, expected in record["files"].items():
        if digest((sdk / name).read_bytes()) != expected:
            raise ValueError(f"SDK system source changed outside overlay: {name}")
    for name, target in header_files():
        if digest((ROOT / "firmware/display" / name).read_bytes()) != record["files"][target]:
            raise ValueError("System header changed; recreate the SDK and reapply overlays")
    for target in RESOURCE_TARGETS:
        if digest((ROOT / "firmware/display/badge_resources.h").read_bytes()) != record["files"][target]:
            raise ValueError("Resource header changed; recreate the SDK and reapply overlays")
    return record


def prepare(sdk):
    if (sdk / MANIFEST).exists():
        check(sdk)
        print("System overlay already applied and verified")
        return
    spec = importlib.util.spec_from_file_location("display_overlay", ROOT / "scripts/prepare-display.py")
    display = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(display)
    display.check(sdk)
    originals = {name: (sdk / name).read_bytes() for name in (UI, ADV, CFG, QIX, CONF, BOARD, ADKEY)}
    ui, adv, cfg, qix, conf, board, adkey = (originals[name].decode("utf8").replace("\r\n", "\n")
                                           for name in (UI, ADV, CFG, QIX, CONF, BOARD, ADKEY))
    if any(MARK in text for text in (ui, adv, cfg)):
        raise ValueError("Partial system overlay detected")
    # badge_safety.h hardcodes the panel TE pin and relies on the SDK arming its
    # interrupt; refuse to build if the board configuration says otherwise.
    for name, needle in ((SDK_CONFIG, "#define TCFG_LCD_PIN_TE IO_PORTC_00 "),
                         (BOARD_CFG, "#define TCFG_LCD_TE_USED_PEND 1\n"),
                         (BOARD_CFG, "#define TCFG_LCD_TE_IO      TCFG_LCD_PIN_TE\n")):
        if needle not in (sdk / name).read_text(encoding="utf8", errors="replace").replace("\r\n", "\n"):
            raise ValueError(f"TE pin configuration changed in {name}; review badge_safety.h")
    # The safety watchdog resets by disabling interrupts and letting the SDK's
    # hardware watchdog fire (badge_safety.h, bs_reset). Refuse to build without it.
    for name, pattern in ((BOARD_CFG, r"^#define\s+WDT_APP_INIT_TIME\s+WDT_16S\b"),
                          (BOARD_CFG, r"^#define\s+WDT_APP_RUN_TIME\s+WDT_LRC_4S\b"),
                          (APP_MAIN, r"^\s*wdt_init\(WDT_APP_RUN_TIME\);")):
        if not re.search(pattern, (sdk / name).read_text(encoding="utf8", errors="replace"), re.M):
            raise ValueError(f"Hardware watchdog configuration changed in {name}; review badge_safety.h")
    # Remove the exclusive black charge page; existing main/menu screens remain usable.
    start = ui.index("static void badge_charge_render(void)")
    end = ui.index("/* ---- BLE-updatable lock wallpaper", start)
    ui = ui[:start] + MARK + '\n#include "badge_system_ui.h"\n\n' + ui[end:]
    for line in (
        '#define BADGE_CHARGE_PEEK_MS     5000   /* how long a key keeps the charge % visible */\n',
        'static lv_obj_t *g_charge_scr;\n',
        'static lv_obj_t *g_charge_lbl;         /* the big "NN%" */\n',
        'static lv_obj_t *g_charge_status_lbl;  /* "CHARGING" / "CHARGED" under it */\n',
        'static u8 g_charge_bkl_on = 0;         /* peek: backlight currently lit */\n',
        'static u32 g_charge_bkl_ms = 0;        /* when the peek started */\n',
    ):
        ui = replace(ui, line, "")
    ui = replace(ui, "static u8 badge_ble_battery_gate(void)",
                 "static u8 badge_usb_online(void);\nstatic u8 badge_update_battery_gate(void)")
    ui = replace(ui, 'if (get_vbat_value() < BADGE_BLE_MIN_MV && !get_charge_online_flag())',
                 'if (get_vbat_value() < BADGE_BLE_MIN_MV && !badge_usb_online())')
    # Remove the now unused declaration inside the renamed gate.
    ui = replace(ui, '    extern u16 get_vbat_value(void);\n    extern u8 get_charge_online_flag(void);',
                 '    extern u16 get_vbat_value(void);')
    at = ui.index(MARK)
    ui = ui[:at] + '''static u8 badge_ble_battery_gate(void)
{
    return badge_update_battery_gate();
}

''' + ui[at:]
    ui = replace(ui, 'if (badge_ble_battery_gate()) { return; }   /* low battery gate: warn instead of entering OTA mode */',
                 'if (badge_update_battery_gate()) { return; }   /* OTA remains available on USB */')
    ui = replace(ui, '    /* Hand the hardware buttons to LVGL:',
                 '    badge_fonts_init();   /* optional flash font pack; built-in 14px otherwise */\n'
                 '    badge_status_build();\n\n    /* Hand the hardware buttons to LVGL:')
    ui = replace(ui, '            extern volatile u8 g_badge_adkey_kv;\n',
                 '            extern volatile u8 g_badge_adkey_kv;\n            badge_system_poll();\n'
                 '            badge_safety_ui_tick();   /* UI liveness watchdog */\n')
    # Watchdog starts before the boot-time screen build and suspend (badge_safety.h):
    # badge_lock_enter() there can wait on the LCD init semaphore without a timeout.
    ui = replace(ui, '    /* Badge tool UI: build all screens, then boot SUSPENDED',
                 '    badge_safety_start();   /* UI watchdog before the boot-time suspend */\n'
                 '    /* Badge tool UI: build all screens, then boot SUSPENDED')
    ui = replace(ui, '    g_badge_locked = 1;\n',
                 '    badge_idle_prepare_lock();\n    g_badge_locked = 1;\n    badge_idle_enter();\n')
    ui = replace(ui, '                /* Suspended (black): any key press wakes to the lock image. */\n'
                     '                if (g_badge_adkey_kv != NO_KEY) {',
                 '                /* Retain a short press even if released before this task ran. */\n'
                 '                if (badge_idle_take_key_wake() || g_badge_adkey_kv != NO_KEY) {')
    ui = replace(ui, '    lcd_sleep_ctrl(0);            /* powers the panel back on (kicks off the re-init task) */',
                 '    badge_idle_exit();             /* sample BEFORE the display/radio load */\n'
                 '    lcd_sleep_ctrl(0);            /* powers the panel back on (kicks off the re-init task) */')
    ui = replace(ui, '                os_time_dly(3);   /* ~30 ms poll; CPU mostly idle while locked */',
                 '                if (g_badge_locked) badge_idle_wait(); /* key event or 1 s power tick */')
    ui = replace(ui, '                extern u8 get_charge_online_flag(void);\n', '')
    ui = replace(ui, 'u8 chg_now = get_charge_online_flag();', 'u8 chg_now = badge_usb_online();')
    # Stage marks for the safety watchdog (badge_safety.h, BS_STAGE_*): a stall blinks
    # the stage number before the reset. Marks only; no step is reordered or changed.
    ui = replace(ui, '    lcd_sleep_ctrl(1);            /* backlight 0 + panel to sleep + power down */\n',
                 '    badge_stage(BS_STAGE_SLEEP_ENTER);\n'
                 '    lcd_sleep_ctrl(1);            /* backlight 0 + panel to sleep + power down */\n'
                 '    badge_stage(BS_STAGE_ASLEEP);\n')
    ui = replace(ui, '    lcd_sleep_ctrl(0);            /* powers the panel back on (kicks off the re-init task) */\n'
                     '    lcd_reinit_wait_finish();     /* blocks until the panel re-init finishes */\n',
                 '    badge_stage(BS_STAGE_WAKE_POWER);\n'
                 '    lcd_sleep_ctrl(0);            /* powers the panel back on (kicks off the re-init task) */\n'
                 '    badge_stage(BS_STAGE_WAKE_REINIT);\n'
                 '    lcd_reinit_wait_finish();     /* blocks until the panel re-init finishes */\n'
                 '    badge_stage(BS_STAGE_WAKE_DONE);\n')
    ui = replace(ui, '                    badge_lock_wake();\n'
                     '                    while (g_badge_adkey_kv != NO_KEY) { os_time_dly(2); }  /* wait for release */\n',
                 '                    badge_lock_wake();\n'
                 '                    while (g_badge_adkey_kv != NO_KEY) { os_time_dly(2); }  /* wait for release */\n'
                 '                    badge_stage(BS_STAGE_AWAKE);\n')
    ui = replace(ui, '            badge_display_tick();\n            lv_timer_handler();\n',
                 '            badge_display_tick();\n'
                 '            badge_stage(g_badge_locked ? BS_STAGE_ASLEEP_DRAW : BS_STAGE_DRAW);\n'
                 '            lv_timer_handler();\n'
                 '            badge_stage(g_badge_locked ? BS_STAGE_ASLEEP : BS_STAGE_AWAKE);\n')
    # Boot: the SDK runs one lv_timer_handler() between jl_gui_init() and the menu loop,
    # after the boot-time sleep. That SDK line is outside the patch, so mark around it.
    ui = replace(ui, '#else\n    badge_lock_enter();\n#endif\n    return;\n',
                 '#else\n    badge_lock_enter();\n    badge_stage(BS_STAGE_ASLEEP_DRAW);   /* the SDK draws once before the menu loop */\n'
                 '#endif\n    return;\n')
    ui = replace(ui, '    ui_sysinfo_init();\n\n#ifdef CONFIG_BADGE_MENU\n',
                 '    ui_sysinfo_init();\n\n#ifdef CONFIG_BADGE_MENU\n'
                 '    badge_stage(g_badge_locked ? BS_STAGE_ASLEEP : BS_STAGE_AWAKE);\n')
    # Claim Update before any catalogue erase; serialize with the Back callback
    # so a BLE upload cannot race the UI into reading resources being replaced.
    ui = replace(ui, 'static u8 g_on_update_scr;             /* the update screen is showing */', '''static volatile u8 g_on_update_scr; /* shared with resource upload dispatch */
int badge_resources_update_open(void) { return g_on_update_scr != 0; }
int badge_resources_lock_update(void)
{
    int open;
    extern volatile u8 g_badge_update_active;
    OS_SR_ALLOC(); OS_ENTER_CRITICAL();
    open=g_on_update_scr;
    if (open) g_badge_update_active=1;
    OS_EXIT_CRITICAL();
    return open;
}
''')
    ui = replace(ui, '    if (g_badge_update_active) { return; }   /* locked while an update is in progress */', '''    {
        int locked;
        OS_SR_ALLOC(); OS_ENTER_CRITICAL();
        locked=g_badge_update_active;
        if (!locked) g_on_update_scr=0;
        OS_EXIT_CRITICAL();
        if (locked) return;
    }''')
    start = ui.index('            while (g_charge_active) {')
    end = ui.index('            if (g_warn_active)', start)
    ui = ui[:start] + ui[end:]
    # Drop the 41 KB embedded wallpaper and the 41 KB full-image RAM buffer.
    ui = replace(ui, '#include "lock_wallpaper.h"             /* embedded 144x144 RGB565 lock-screen wallpaper */',
                 '/* Wallpaper is device-local data, not an embedded application bitmap. */')
    start = ui.index('/* ---- BLE-updatable lock wallpaper')
    end = ui.index('/* ---- Lock / screensaver: display + touch OFF', start)
    ui = ui[:start] + ('#include "badge_wallpaper.h"\n#include "badge_fonts.h"\n'
                       '#define BADGE_SAFETY_IMPLEMENTATION\n#include "badge_safety.h"\n\n') + ui[end:]
    ui = replace(ui, '        const lv_img_dsc_t *wp = lock_img_resolve();\n',
                 '        const lv_img_dsc_t *wp = lock_img_resolve();\n        if (wp) {\n')
    ui = replace(ui, '        lv_obj_add_flag(img, LV_OBJ_FLAG_GESTURE_BUBBLE);\n',
                 '        lv_obj_add_flag(img, LV_OBJ_FLAG_GESTURE_BUBBLE);\n        }\n')
    # Preserve the full-resolution wallpaper sector range during staging erase.
    qix = replace(qix, '#define QIX_STAGE_END     0x3BE000u', '#define QIX_STAGE_END     0x2A0000u')
    qix = replace(qix, '/* Dispatch a complete, validated Qix frame. */', '''#ifdef CONFIG_BADGE_MENU
extern void wdt_clear(void);
#define BR_WATCHDOG() wdt_clear()
#define BADGE_RESOURCES_IMPLEMENTATION
#include "badge_resources.h"
#include "badge_resources_wire.h"
#endif

/* Dispatch a complete, validated Qix frame. */''')
    qix = replace(qix, '    extern int qix_raw_flash_handle(u8 cmd, const u8 *p, u16 plen);', '''    extern int qix_raw_flash_handle(u8 cmd, const u8 *p, u16 plen);
    if (badge_resource_command(cmd,p,plen)) return;
    if (badge_resource_protect_raw(cmd,p,plen)) return;''')
    # Read-only safety diagnostics (0x97 -> 0x98) work in Display and Update alike.
    qix = replace(qix, '    extern volatile u8 g_badge_display_mode;\n',
                  '    extern int badge_safety_command(u8 cmd, const u8 *p, u16 n);\n'
                  '    if (cmd == 0x97) { badge_safety_command(cmd, p, plen); return; }\n'
                  '    extern volatile u8 g_badge_display_mode;\n')
    qix += '''
#ifdef CONFIG_BADGE_MENU
void badge_safety_reply(const u8 *data, u16 len)
{
    qix_send_frame(0x98, data, len);
}
#endif
'''
    # A disconnected uploader must BEGIN again, never inherit a stale write window.
    qix = replace(qix, 'void qix_ota_server_on_disconnect(void)\n{',
                   'void badge_resource_abort(void);\nvoid qix_ota_server_on_disconnect(void)\n{\n'
                   '#ifdef CONFIG_BADGE_MENU\n    badge_resource_abort();\n#endif')
    # Titles now come from the device-local font pack; the charge page that also
    # used the built-in 36px font was removed above. This frees ~14 KB of code.
    conf = replace(conf, '#define LV_FONT_MONTSERRAT_36 1   /* badge charge screen: real 36px thick font (glyphs stripped to ASCII 32..90) */',
                   '#define LV_FONT_MONTSERRAT_36 0   ' + MARK + ' /* titles use the flash font pack */')
    # The label uses a fixed width; scan response limits remain below 31 bytes.
    cfg = '#ifdef CONFIG_BADGE_MENU\n' + MARK + '\n#include "badge_identity.h"\n#endif\n' + cfg
    cfg = replace(cfg, '    sprintf(bt_cfg.edr_name, "%s", TCFG_JL_WALLPAPER_ADV_NAME);',
                  '    sprintf(bt_cfg.edr_name, "%s", TCFG_JL_WALLPAPER_ADV_NAME);\n#ifdef CONFIG_BADGE_MENU\n    badge_identity_load((char *)bt_cfg.edr_name);\n#endif')
    adv = replace(adv, '        const char *nm = "EC-MOUSE";',
                  '        ' + MARK + '\n        extern const char *badge_mouse_local_name(void);\n        const char *nm = badge_mouse_local_name();')
    # E87 only (the Display build already rejects DG01). Keep the original ADKEY
    # scanner/debounce; post only on transitions so a short tap wakes a blocked UI.
    adkey = replace(adkey, '        g_badge_adkey_kv = kv;',
                    '        extern void badge_idle_key_changed(void);\n'
                    '        if (g_badge_adkey_kv != kv) {\n'
                    '            g_badge_adkey_kv = kv;\n'
                    '            badge_idle_key_changed();\n'
                    '        }', count=2)
    board = replace(board, 'static volatile u8 g_tp_reinit_req = 0;   /* wake path asks the poll task to re-init */',
                    'static volatile u8 g_tp_reinit_req = 0;   /* wake path asks the poll task to re-init */\n'
                    '#include "badge_idle_event.h"\nstatic badge_idle_event badge_touch_event;')
    board = replace(board, '    g_axs_want = en ? 1 : 0;',
                    '    u8 was_enabled = g_axs_want;\n    g_axs_want = en ? 1 : 0;')
    board = replace(board, '        g_tp_reinit_req = 1;    /* the poll task owns the bus; ask it to re-init */',
                    '        g_tp_reinit_req = 1;    /* the poll task owns the bus; ask it to re-init */\n'
                    '        if (!was_enabled) badge_idle_event_signal(&badge_touch_event);')
    board = replace(board, '        if (g_tp_reinit_req && g_axs_want) {',
                    '        if (!g_axs_want) {\n'
                    '            g_tp_cdown = 0;\n'
                    '            badge_idle_event_wait(&badge_touch_event, 0); /* wait until enabled */\n'
                    '            continue;\n'
                    '        }\n        if (g_tp_reinit_req && g_axs_want) {')
    board = replace(board, '    task_create(axs_poll_task, NULL, "axs_poll");',
                    '    badge_idle_event_init(&badge_touch_event);\n'
                    '    task_create(axs_poll_task, NULL, "axs_poll");')
    qix = replace(qix, '    extern volatile u8 g_badge_display_mode;\n',
                  '    extern int badge_power_command(u8 cmd, const u8 *p, u16 n);\n'
                  '    if (cmd == 0x92) { badge_power_command(cmd, p, plen); return; }\n'
                  '    extern volatile u8 g_badge_display_mode;\n')
    qix += '''\n#ifdef CONFIG_BADGE_MENU
void badge_power_reply(const u8 *data, u16 len)
{
    qix_send_frame(0x93, data, len);
}
#endif
'''
    output = {UI: ui.encode(), ADV: adv.encode(), CFG: cfg.encode(), QIX: qix.encode(),
              CONF: conf.encode(), BOARD: board.encode(), ADKEY: adkey.encode()}
    output.update({target: (ROOT / "firmware/display" / name).read_bytes() for name, target in header_files()})
    output.update({target: (ROOT / "firmware/display/badge_resources.h").read_bytes() for target in RESOURCE_TARGETS})
    record = {
        "display_manifest_sha256": digest((sdk / ".badge-display-overlay.json").read_bytes()),
        "base_files": {name: digest(data) for name, data in originals.items()},
        "files": {name: digest(data) for name, data in output.items()},
    }
    for name, data in output.items():
        (sdk / name).write_bytes(data)
    (sdk / MANIFEST).write_text(json.dumps(record, indent=2), encoding="utf8")
    print("System overlay applied; identity, wallpaper and fonts are provisioned separately")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=SDK)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        check(args.sdk) if args.check else prepare(args.sdk)
    except (OSError, ValueError, KeyError) as error:
        print(f"System setup failed: {error}", file=sys.stderr)
        sys.exit(1)
