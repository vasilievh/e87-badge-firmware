#!/usr/bin/env python3
"""Apply the display overlay after setup-badge-build.sh, without editing upstream's patch.

Only expected, exact SDK snippets are changed; repeat runs are no-ops. No device I/O.
"""
from pathlib import Path
import argparse
import hashlib
import json
import sys

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / "tools/community-re/jieli-sdks/e_badge_707_sdk_200"
UI = "SDK/apps/common/ui/lvgl_v8/lvgl_main.c"
QIX = "SDK/apps/common/third_party_profile/jieli/rcsp/qix_ota_server.c"
MARK = "/* BADGE_DISPLAY_OVERLAY_V1 */"
MANIFEST = ".badge-display-overlay.json"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def check(sdk, allow_headers=False):
    record = json.loads((sdk / MANIFEST).read_text(encoding="utf8"))
    if record["base_patch_sha256"] != digest((ROOT / "patches/badge/badge-menu.patch").read_bytes()):
        raise ValueError("Upstream patch changed; re-create the SDK before applying this overlay")
    for name, expected in record["files"].items():
        if allow_headers and name.endswith(".h"):
            continue
        if digest((sdk / name).read_bytes()) != expected:
            raise ValueError(f"SDK display source changed outside the overlay: {name}")
        if name.endswith(".h") and digest((ROOT / "firmware/display" / Path(name).name).read_bytes()) != expected:
            raise ValueError("Display header changed; rerun scripts/prepare-display.py")
    return record


def replace(text, old, new, count=1):
    found = text.count(old)
    if found != count:
        raise ValueError(f"Expected {count} occurrences, found {found}: {old[:80]!r}")
    return text.replace(old, new)


def prepare(sdk):
    ui = (sdk / UI).read_text(encoding="utf8")
    qix = (sdk / QIX).read_text(encoding="utf8")
    if (MARK in ui) != (MARK in qix):
        raise ValueError("Partial overlay detected; restore the two SDK files before retrying")
    if MARK in ui:
        check(sdk, allow_headers=True)
        refresh_headers(sdk)
        print("Display overlay already applied; headers refreshed")
        return
    ui = replace(ui, "static lv_obj_t *g_menu_btns[4];", "static lv_obj_t *g_menu_btns[5];")
    ui = replace(ui, "volatile u8 g_badge_mouse_mode;", "volatile u8 g_badge_mouse_mode;\nextern volatile u8 g_badge_display_mode;")
    ui = replace(ui, "    g_ble_want_on = 0;\n    ble_module_enable(0);",
                 "    g_ble_want_on = 0;\n    g_badge_display_mode = 0; /* includes USB/charge exits */\n    ble_module_enable(0);")
    ui = replace(ui, "static void badge_menu_build(void)\n{", MARK + '\n#include "badge_display_ui.h"\n\nstatic void badge_menu_build(void)\n{')
    ui = replace(ui, "static const char *const items[4]", "static const char *const items[5]")
    ui = replace(ui, '"Bluetooth Spoofing", "Bluetooth Mouse", "Bluetooth Scanner", "Settings"',
                 '"Bluetooth Spoofing", "Bluetooth Mouse", "Bluetooth Scanner", "Settings", "Bluetooth Display"')
    ui = replace(ui, "for (int i = 0; i < 4; i++) {\n        if (g_menu_btns[i])", "for (int i = 0; i < 5; i++) {\n        if (g_menu_btns[i])")
    ui = replace(ui, "for (int i = 0; i < 4; i++) {\n        lv_obj_t *btn = g_menu_btns[i]", "for (int i = 0; i < 5; i++) {\n        lv_obj_t *btn = g_menu_btns[i]")
    ui = replace(ui, "lv_obj_set_size(btn, 280, 56);", "lv_obj_set_size(btn, 240, 44);")
    ui = replace(ui, "0, 70 + i * 68);", "0, 66 + i * 50);")
    ui = replace(ui, "i == 1 ? mouse_open_cb : (i == 2 ? scanner_open_cb : (i == 3 ? settings_open_cb : spoof_open_cb))",
                 "i == 4 ? display_open_cb : (i == 1 ? mouse_open_cb : (i == 2 ? scanner_open_cb : (i == 3 ? settings_open_cb : spoof_open_cb)))")
    ui = replace(ui, "            lv_timer_handler();\n            os_time_dly(1);",
                 "            badge_display_tick();\n            lv_timer_handler();\n            os_time_dly(1);")
    needle = "static void qix_handle_frame(u8 cmd, const u8 *p, u16 plen)\n{\n#ifdef CONFIG_BADGE_MENU"
    qix = replace(qix, needle, """static void qix_handle_frame(u8 cmd, const u8 *p, u16 plen)
{
#ifdef CONFIG_BADGE_MENU
    /* BADGE_DISPLAY_OVERLAY_V1 */
    /* Display accepts only its own RAM-only protocol.
     * OTA and raw flash remain accessible exclusively through the Update screen. */
    extern volatile u8 g_badge_display_mode;
    extern int badge_display_command(u8 cmd, const u8 *p, u16 plen);
    if (g_badge_display_mode) {
        badge_display_command(cmd, p, plen);
        return;
    }
    if (cmd == 0x90) { badge_display_command(cmd, p, plen); return; }""")
    # qix_send_frame is private; expose only the dedicated response opcode.
    qix += """
#ifdef CONFIG_BADGE_MENU
void badge_display_reply(const u8 *data, u16 len)
{
    qix_send_frame(0x91, data, len);
}
#endif
"""
    # Validate every replacement before touching any SDK file.
    for name in ("badge_display_protocol.h", "badge_display_ui.h"):
        (ROOT / "firmware/display" / name).read_bytes()
    (sdk / UI).write_text(ui, encoding="utf8", newline="\n")
    (sdk / QIX).write_text(qix, encoding="utf8", newline="\n")
    refresh_headers(sdk)
    print("Display overlay applied; build the E87 OTA image next")


def refresh_headers(sdk):
    names = [UI, QIX]
    for name in ("badge_display_protocol.h", "badge_display_ui.h"):
        target = (sdk / UI).with_name(name)
        target.write_bytes((ROOT / "firmware/display" / name).read_bytes())
        names.append(target.relative_to(sdk).as_posix())
    record = {"base_patch_sha256": digest((ROOT / "patches/badge/badge-menu.patch").read_bytes()),
              "files": {name: digest((sdk / name).read_bytes()) for name in names}}
    (sdk / MANIFEST).write_text(json.dumps(record, indent=2), encoding="utf8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=SDK)
    parser.add_argument("--check", action="store_true", help="validate installed overlay without modifying it")
    args = parser.parse_args()
    try:
        check(args.sdk) if args.check else prepare(args.sdk)
    except (OSError, ValueError, KeyError) as error:
        print(f"Display setup failed: {error}", file=sys.stderr)
        sys.exit(1)
