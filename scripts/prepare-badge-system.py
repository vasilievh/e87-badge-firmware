#!/usr/bin/env python3
"""Apply battery/home status and optional device-local identity after Display v1.

No personal settings/assets are read by this build step. Device provisioning is
separate. Exact anchors and a manifest reject source drift before any writes.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import sys

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / "tools/community-re/jieli-sdks/e_badge_707_sdk_200"
UI = "SDK/apps/common/ui/lvgl_v8/lvgl_main.c"
ADV = "SDK/apps/common/third_party_profile/jieli/rcsp/adv/ble_rcsp_adv.c"
CFG = "SDK/apps/watch/user_cfg.c"
MANIFEST = ".badge-system-overlay.json"
HEADERS = {
    "badge_system_ui.h": str(Path(UI).with_name("badge_system_ui.h")),
    "badge_power_status.h": str(Path(UI).with_name("badge_power_status.h")),
    "badge_identity.h": str(Path(CFG).with_name("badge_identity.h")),
}
MARK = "/* BADGE_SYSTEM_OVERLAY_V1 */"


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
    for name, target in HEADERS.items():
        if digest((ROOT / "firmware/display" / name).read_bytes()) != record["files"][target]:
            raise ValueError("System header changed; recreate the SDK and reapply overlays")
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
    originals = {name: (sdk / name).read_bytes() for name in (UI, ADV, CFG)}
    ui, adv, cfg = (originals[name].decode("utf8") for name in (UI, ADV, CFG))
    if any(MARK in text for text in (ui, adv, cfg)):
        raise ValueError("Partial system overlay detected")
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
                 '    badge_status_build();\n\n    /* Hand the hardware buttons to LVGL:')
    ui = replace(ui, '            extern volatile u8 g_badge_adkey_kv;\n',
                 '            extern volatile u8 g_badge_adkey_kv;\n            badge_system_poll();\n')
    ui = replace(ui, '                extern u8 get_charge_online_flag(void);\n', '')
    ui = replace(ui, 'u8 chg_now = get_charge_online_flag();', 'u8 chg_now = badge_usb_online();')
    start = ui.index('            while (g_charge_active) {')
    end = ui.index('            if (g_warn_active)', start)
    ui = ui[:start] + ui[end:]
    # The label uses a fixed width; scan response limits remain below 31 bytes.
    cfg = '#ifdef CONFIG_BADGE_MENU\n' + MARK + '\n#include "badge_identity.h"\n#endif\n' + cfg
    cfg = replace(cfg, '    sprintf(bt_cfg.edr_name, "%s", TCFG_JL_WALLPAPER_ADV_NAME);',
                  '    sprintf(bt_cfg.edr_name, "%s", TCFG_JL_WALLPAPER_ADV_NAME);\n#ifdef CONFIG_BADGE_MENU\n    badge_identity_load((char *)bt_cfg.edr_name);\n#endif')
    adv = replace(adv, '        const char *nm = "EC-MOUSE";',
                  '        ' + MARK + '\n        extern const char *badge_mouse_local_name(void);\n        const char *nm = badge_mouse_local_name();')
    output = {UI: ui.encode(), ADV: adv.encode(), CFG: cfg.encode()}
    output.update({target: (ROOT / "firmware/display" / name).read_bytes() for name, target in HEADERS.items()})
    record = {
        "display_manifest_sha256": digest((sdk / ".badge-display-overlay.json").read_bytes()),
        "base_files": {name: digest(data) for name, data in originals.items()},
        "files": {name: digest(data) for name, data in output.items()},
    }
    for name, data in output.items():
        (sdk / name).write_bytes(data)
    (sdk / MANIFEST).write_text(json.dumps(record, indent=2), encoding="utf8")
    print("System overlay applied; identity and wallpaper are provisioned separately")


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
