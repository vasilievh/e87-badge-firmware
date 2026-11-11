# Hypotheses log

Every explanation we act on starts here as a hypothesis. Keep rejected ones: they save the
next person from chasing them again. Newest incident first.

Status: **open** (not yet tested), **supported** (evidence agrees, not proven),
**confirmed** (measured), **rejected** (evidence contradicts).

## 2026-11-11 (fifth): hang on wake after charging and a cool-down

Symptom: same firmware. Charged while suspended, unplugged, left to cool, woken: black
screen with the backlight on, no response (so past the panel re-init, as on the first
2026-11-10 hang). All charging so far was from a computer USB port. Sixth hang in a row within the first actions after a cold boot.

| Hypothesis | Status | Evidence / how to test |
|---|---|---|
| Heat right after charging causes the hangs | rejected (one try) | Cooled down before waking; hung anyway. |
| A current-limited supply (computer USB port) lets the 5 V rail sag while charging plus running; upstream's "sagging 5V rail" | open | Every USB-attached hang was on a computer port (typically 500-900 mA), often while charging from empty at full current. Upstream blames the 5 V rail; the HT30 manual likewise asks for a standard 5 V / 1-2 A adapter. The owner has always charged from a computer port, including the weeks without hangs, so this cannot explain why hangs became frequent; at most it adds to another cause (e.g. a weaker battery). Test: the same steps on a standard 5 V / 1-2 A adapter. |
| The battery has degraded (repeated drains to empty since 2026-11-10) and its voltage sags under load spikes (panel power-up and backlight at wake, a full-screen draw, the radio starting), locking the board up | open | The hang rate went from rare (hours of use on 2026-11-09/10) to nearly every attempt after five or six deep drains, and the hangs sit at load spikes. A lock-up of the chip would also explain no reset and no radio. The original 2026-11-09 hang predates the drains, so this would add to, not replace, the first cause. Test: wake on a full battery with a standard 5 V / 1-2 A adapter connected, so USB carries the spike; `qix_ble.power` battery readings under load once Update is reachable. |

## 2026-11-11 (fourth): hang again on going from the lock image to the menu

Symptom: same firmware. Drained, charged for an unknown time while suspended, unplugged,
woken, Button to the menu: hung in the menu, battery shown as 7%. Third hang in a row at
the same step (lock image -> menu), after one at Update. This time the menu frame was drawn
completely before the hang, so the cut line is not fixed: no sign of one piece of frame
content that always stops it.

## 2026-11-11 (third): frame half drawn, on battery at 100%

Symptom: same firmware (`eb5a1f5`). Charged to 100% while suspended, USB unplugged, then
woken and Button pressed to go from the lock image to the menu. It hung with the frame
half drawn: the top of the screen showed the new menu (title, battery 100%, three buttons
and the edge of a fourth), the bottom still showed the lock image. Touch and buttons
ignored, backlight on. The owner photographed it (not kept in the repository: it shows
their wallpaper).

| Hypothesis | Status | Evidence / how to test |
|---|---|---|
| The UI stops part-way through a frame | supported | The photo: the panel received the top bands of the new frame and nothing after. Either the transfer of the next band never completed, or rendering the next band never finished (both leave earlier bands on screen); which one is open. The same cut line in every hang would point at the frame's content (rendering), a varying one at the transfer. With `TCFG_LCD_FB_CNT 2` LVGL sends a frame in bands; one band's completion never arrived, so the next was never sent. Fits the review of `lcd_lock()` / `lcd_wait_busy()` (unbounded waits). The CPU should still be running, so the fixed watchdog in `fix/wake-diag` should catch it (7 pulses). |
| USB is required | rejected | This one was on battery at 100%. USB may still raise the odds; not shown. |

## 2026-11-11 (second): hang on pressing Update again, empty battery on USB

Symptom: same firmware. Drained, booted on the charger, woken and taken straight to
Update: hung at once, as in the previous entry. Unplugged, it died immediately (battery
empty).

Upstream documents this: `badge-menu.patch` README, "powering the device from USB and battery
at the same time can hang it", and `badge_charge_screen_enter()` parked the whole UI on USB
with backlight and radio off to keep the radio off "the sagging 5V rail that browns the
board out". The system overlay removes that charge screen in every build that hung
(`468a76e` and `eb5a1f5`), and bypasses the low-battery BLE gate on USB.

| Hypothesis | Status | Evidence / how to test |
|---|---|---|
| Running on USB with the battery sharing the supply locks the board up (upstream's documented brownout), made likely by our removal of the charge screen | open (was marked supported; the 2026-11-11 third hang was on battery at 100%) | Five of six hangs were on USB: 2026-11-09 (woken on USB), 2026-11-10 second (woken after suspend on USB), third (awake in the menu on USB), and both 2026-11-11 (Update pressed on USB, radio starting, battery empty). The one on battery (first 2026-11-10, 47%) is the exception. Never a reset and no radio afterwards fits an electrical lockup better than a software wait. Test: keep the badge asleep and untouched on USB, unplug, then use and flash on battery only. |

Owner's experiment the same night, battery near empty after the cold boot on the charger:
unplugging USB while awake either froze the badge on the current screen, or opened the
Display or Bluetooth Scanner tool on its own (the first and last menu entries). Unplugged
on the lock image it always froze.

| Hypothesis | Status | Evidence / how to test |
|---|---|---|
| With a near-empty battery, unplugging USB is a brownout: the board locks up instead of resetting | open | Fits the frozen screens and the earlier "died immediately" on unplug. Expected with an empty cell; says nothing yet about a charged one. Test: the same unplug with a charged battery. |
| Supply transitions produce phantom key presses: the buttons are read through an ADC ladder (`adkey`), whose reading shifts when the supply drops | open | Tools opening by themselves on unplug. Test: unplug with a charged battery in the menu; a phantom selection then would point at the key ladder, not only at the empty cell. |

(Later the same day a hang on battery at 100% showed a half-drawn frame; see the entry
above. USB is not required.)

## 2026-11-11: hang on pressing Update

Symptom: firmware `fix/lock-power` `eb5a1f5`, shortly after the drain-and-charge recovery
from the 2026-11-10 (third) hang, on USB, charging from empty. It hung as soon as Update
was pressed; touch and buttons ignored. `qix_ble.safety` and `qix_ble.power` did not find
the badge, so the radio was not running. Fifth hang in about a day; three of the last four
came soon after charging from empty.

| Hypothesis | Status | Evidence / how to test |
|---|---|---|
| Low battery while charging from empty makes the hangs much more likely | open | 2026-11-10 (second), (third) and this one all came soon after a cold boot on the charger from empty; the first 2026-11-10 hang was on battery at 47%, 2026-11-09 on USB at 100%. Test: charge suspended overnight without waking, unplug, then wake and flash on battery above 30%. |
| Opening Update itself triggers the hang | open | One case; Update has worked many times before on the same firmware. Likely coincidence with the high hang rate, not shown. |

## 2026-11-10 (third): hang while awake on USB

Symptom: firmware `fix/lock-power` `eb5a1f5`. After the drain-and-charge recovery the badge
was left awake in the menu at minimum brightness, charging, never suspended. Later it was
found hung: touch and both buttons ignored, the status bar showing 9%. When it hung is
unknown. USB removed to let it drain.

| Hypothesis | Status | Evidence / how to test |
|---|---|---|
| The hangs need a sleep/wake transition | rejected | This one happened awake, with no suspend since boot. The wake path is still where three of four hangs happened, but the fault can also hit during ordinary drawing. |
| The fault is a display transfer whose completion is lost (see the wake-path review rows) | open | Fits a hang during ordinary drawing as well as around sleep and wake. `fix/wake-diag` would report it as 7 pulses when awake. |
| Low battery while charging matters | open | Charging from empty this time; 2026-11-09 was at 100% on USB, and the first 2026-11-10 hang was on battery at 47%. |

## 2026-11-10 (second): no wake after suspending on USB

Symptom: same firmware as below (`fix/lock-power` `eb5a1f5`). After the drain-and-charge
recovery the badge worked normally on USB; it was suspended while charging. The next
wake attempt got no reaction at all: backlight stayed off, keys and the button-hold reset
did nothing. USB was then removed to let the battery drain; after the drain it booted
normally on the charger (third time drain-then-charge recovered it).

Firmware identity (artifact digests of the Badge Display runs match the owner's local
copies): the 2026-11-09 hang ran `feat/ble-display` `468a76e` (no `badge_safety.h`, no
`badge_idle_*`); both 2026-11-10 hangs ran `fix/lock-power` `eb5a1f5`. `patches/` (the wake
path in `badge-menu.patch`) is identical between the two.

| Hypothesis | Status | Evidence / how to test |
|---|---|---|
| The fault is in the wake path shared by all three builds (upstream `badge_lock_wake()`, SDK `lcd_sleep_ctrl(0)` / re-init / first redraw), not in the safety net or the idle wait | supported | Three hangs on wake from suspend, on two builds whose wake code is identical; the one without the safety net and idle wait hung the same way. |
| This time the hang was before the backlight: `lcd_reinit_wait_finish()` (SDK `lcd_drive.c`) pends with no timeout, and `lcd_init_task()` can stall before posting: after the patch's `lcd_init(__this)`, reset and command list it clears the panel, and `lcd_drv_clear_screen()` ends in `lcd_wait_busy()`, which (per a review of `gpu.a` `dbi.c`) retries with no overall limit | open | Code reading: the clear-screen path and the patch's extra `lcd_init(__this)` checked here; the unbounded `lcd_wait_busy()` / `lcd_lock()` loops are from a review of the library bitcode, not re-checked. A transfer whose completion never arrives (`dbi_spi_isr()` clears the busy state) would stall the re-init (backlight off) or the first redraw (backlight on). |
| The badge never finished entering sleep, so the next press found nobody to handle it | open | Code reading: `badge_lock_enter()` sets `g_badge_locked` first, and `lcd_sleep_ctrl(1)` turns the backlight off before the panel sleep commands (`lcd_write_cmd()` -> `lcd_lock()`) and the other waits. A dark badge therefore does not prove the sleep completed. Needs a "sleep entry done" mark. |
| At boot the SDK runs one `lv_timer_handler()` after `jl_gui_init()` has already put the panel to sleep (`badge_lock_enter()`), before the badge loop that skips LVGL while locked | open | Code reading (SDK `lvgl_main.c`, patch keeps it): a redraw is started against a powered-down panel. Link to the hangs not shown. Changing it touches the boot path, so it needs care: a boot that never shows the menu cannot reach Update. |
| USB attached while suspended matters | open | Two of three hangs followed a suspend on USB (2026-11-09, this one); the first 2026-11-10 hang was on battery, so USB is not required. |

## 2026-11-10: hang on key wake during the button-hold reset test

Symptom: firmware `fix/lock-power` with the safety net (`badge_safety.h`), on battery, not
charging. Diagnostics read from the Update screen shortly before: 3824 mV (47%), uptime
about 39 min. The badge was then suspended (dark) and woken with Button 2 for the
button-hold reset test; the owner may have pressed twice and then held. The backlight came
on, but nothing was ever drawn: black screen, touch and both buttons ignored, a second long
hold changed nothing. Unlike 2026-11-09 it never reached the wallpaper or the menu. No reset
happened at any point (the backlight stayed on; a reset boots suspended with it off). It
stayed like that until the battery was fully drained; after charging it boots and works
normally. Second confirmation of drain-then-charge as the only recovery. The radio was off,
so `qix_ble.safety` / `qix_ble.power` could not be read during the hang.

| Hypothesis | Status | Evidence / how to test |
|---|---|---|
| The safety net did not act during this hang: the UI watchdog timer stopped, did not see a stall, or `bs_reset()` ran and neither the hardware watchdog nor the `cpu_reset()` fallback reset the chip | open | The fallback alone fires within minutes of `bs_reset()`, yet nothing reset in hours, so most likely `bs_reset()` never ran. The timer was registered and running before the test: the safety report read `watchdog_exempt: true` on the Update screen, and only the timer callback writes that flag. SDK `interface/system/timer.h`: `sys_hi_timer_add` is `usr_timer_add(..., 1)`, called from a hardware-timer interrupt. Not yet explained why it stopped acting. |
| The hang is after the panel re-init, not inside it | supported | Code reading: `badge_lock_wake()` turns the backlight on only after `lcd_reinit_wait_finish()` returns, and the backlight was on. SDK `lcd_drive.c` checked: `lcd_sleep_ctrl(0)` leaves the backlight off (its call is commented out), and `lcd_init_task()` ends by clearing the panel to black, which explains the black lit screen. |
| The touch re-enable at wake blocks | rejected | `badge_touch_set_enable(1)` only sets a request flag; `axs_poll_task` does the re-init in its own task. |
| The first redraw never completes: `lv_disp_flush_wait_finish()` (SDK `lv_port_disp.c`) waits for the flush-done callback with no way out | rejected | Not compiled: it and its `wait_cb` registration sit under `#if ((TCFG_LCD_FB_CNT==1) \|\| LV_USE_GPU_LIST_DRAW)`, and this build has `TCFG_LCD_FB_CNT 2` (`lv_port_disp.h`) and `LV_USE_GPU_LIST_DRAW 0` (`lv_conf.h`), unchanged by the patch and scripts. A similar unbounded retry exists lower in the real frame path (`gpu.a` `dbi.c` `lcd_lock()`, called from `lcd_draw_continue()`); that one is open. |
| The UI watchdog's window can never fill: 13 samples hold at most 12 periods, and a stall is judged only once a sample at least `BS_WD_STALL_MS` (60000) old exists. If the callbacks arrive even 1 ms faster than 5000 ms by `timer_get_ms()`, 12 periods are under 60 s, the oldest sample is overwritten first, and `bs_watchdog_check()` never returns 1 | supported | Reproduced with the real `bs_watchdog_check()` and a fully stopped UI loop: a 5000 or 5001 ms period fires at call 12; 4999 or 4990 ms never fires (2000 calls). The native tests only use exact 5000 ms periods. Fits the absence of any reset; the real period on the badge is not measured. Fixed in `fix/wake-diag` (decides on a full history, and on zero progress whatever the span); not yet tested on hardware. |
| Every Button 1 suspend is followed by one LVGL pass against the sleeping panel | open | Code reading: in the menu loop the lock-image edge handler calls `badge_lock_enter()` and the same pass falls through to `badge_display_tick(); lv_timer_handler();`, after `lv_scr_load(g_lock_scr)` invalidated the screen. Same pattern as the boot-time draw. All three hangs followed a suspend. `fix/wake-diag` marks it as its own stage (8 pulses). |
| The key scan timer (also `usr_timer`) stopped with the UI watchdog, so the UI sat in the wait-for-release loop with a stale "pressed" key | open | Alternative explanation for a lit black screen with no redraw. Code reading only. |
| The SDK feeds the hardware watchdog from the OS layer (idle task or tick), so a blocked UI task never trips it | open | Code reading: `wdt_clear` is defined only in `cpu.a` (`wdt_p33.c`); the closed `system.a` (`os_cpu_c_overwrite_v2.c`) references `os_wdt_clr` among idle-task symbols. Fits both hangs lasting until the battery drained. Only interrupts off or a spinning CPU would then trip it. |
| A second key press while `badge_lock_wake()` is still running causes the hang | open | The owner may have pressed twice. No mechanism found yet: the extra press only sets `badge_key_wake`, which the next Lock clears. Test: single short presses only for a while, then compare. |

## 2026-11-09: hang after waking on USB

Symptom: badge suspended with Button 1 while charging at 100%, woken with Button 2. Screen
stayed black, then showed the wallpaper and later the menu, but the UI updated only every
few tens of seconds, ignored touch and almost all key presses, and the badge got warm.
Unplugging USB did not help; holding both buttons for 15 s did not reset it. It stayed in
that state until the battery was completely drained (about 3.5 h); after charging it booted
normally. This confirms the last-resort recovery for a hang on this unit: drain fully, then
charge for a cold boot. Firmware: `feat/ble-display` `468a76e` (identified by artifact digest on
2026-11-10).

| Hypothesis | Status | Evidence / how to test |
|---|---|---|
| Floating panel TE pin causes an interrupt storm while the panel is off: `lcd_drv_power_off()` sets PC00 to `PORT_HIGHZ` but its falling-edge IRQ stays armed (SDK `lcd_drive.c`) | open | Code reading only. Guard and counter added in `badge_safety.h`; after a suspend, `qix_ble.safety` `last_sleep.te_per_second` near 0 with the guard. An overnight lock drain much lower than the earlier 100% → 11% would support it. |
| Display or touch re-init failed at wake, leaving every frame flush waiting on the panel | open | Fits black-then-image and slowness that survived unplugging; not measured. The TE counter and the UI watchdog give evidence and a way out next time. |
| Radio on USB browns the board out (upstream's reason for the charge screen) | open | Documented upstream; the system overlay removed that protection. Test: drained battery, USB, Scanner/Display open 10-15 min. |
| BLE stack hogging the CPU | rejected | BLE is off in the menu; "BLUETOOTH LOCKED" only refuses to turn it on and changes no radio state. |
| Touch I2C bit-banging starves the UI | rejected as sole cause | Reads are bounded (200 µs clock-stretch cap), worst case ~15 ms of every 20 ms poll; cannot stop the UI by itself. |
| ADKEY scan blocks on a PMU ADC conversion in timer context | rejected | E87 path uses `adc_get_value()`, which reads the sampling queue (SDK `gpadc.h`), not a blocking conversion. |
| Charge-screen loop spinning | rejected | That loop is removed by `prepare-badge-system.py`. |
| `fix/lock-power` clock release saves power | rejected | `lcd_sleep_ctrl()` already releases and re-takes the `ui_show` clock lock; the extra calls were removed. |
| The SDK hardware watchdog (`WDT_LRC_4S` at run time) resets the badge when interrupts stay off; the safety reset relies on it | open | Code reading: this build's SDK `ASSERT` is `local_irq_disable(); while (1);` and nothing else resets it. Test: hold button 2 on the dark lock screen; a reset within about 35-40 s confirms the path (the `cpu_reset()` fallback would take far longer). 2026-11-10: the test run hung on wake instead and never reset (see that entry), so the result says nothing about this path on its own. |
