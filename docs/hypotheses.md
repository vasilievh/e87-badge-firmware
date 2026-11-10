# Hypotheses log

Every explanation we act on starts here as a hypothesis. Keep rejected ones: they save the
next person from chasing them again. Newest incident first.

Status: **open** (not yet tested), **supported** (evidence agrees, not proven),
**confirmed** (measured), **rejected** (evidence contradicts).

## 2026-11-10 (second): no wake after suspending on USB

Symptom: same firmware as below (`fix/lock-power` `eb5a1f5`). After the drain-and-charge
recovery the badge worked normally on USB; it was suspended while charging. The next
wake attempt got no reaction at all: backlight stayed off, keys and the button-hold reset
did nothing. USB was then removed to let the battery drain.

Firmware identity (artifact digests of the Badge Display runs match the owner's local
copies): the 2026-11-09 hang ran `feat/ble-display` `468a76e` (no `badge_safety.h`, no
`badge_idle_*`); both 2026-11-10 hangs ran `fix/lock-power` `eb5a1f5`. `patches/` (the wake
path in `badge-menu.patch`) is identical between the two.

| Hypothesis | Status | Evidence / how to test |
|---|---|---|
| The fault is in the wake path shared by all three builds (upstream `badge_lock_wake()`, SDK `lcd_sleep_ctrl(0)` / re-init / first redraw), not in the safety net or the idle wait | supported | Three hangs on wake from suspend, on two builds whose wake code is identical; the one without the safety net and idle wait hung the same way. |
| This time the hang was before the backlight: `lcd_reinit_wait_finish()` (SDK `lcd_drive.c`) pends on the re-init semaphore with no timeout | open | Code reading only: the backlight stayed off, and the only earlier steps are the re-init wait and the `while (is_lcd_busy);` spin at the top of `lcd_sleep_ctrl()`. The key may also simply not have been taken. |
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
| The UI watchdog's window can never fill: 13 samples hold at most 12 periods, and a stall is judged only once a sample at least `BS_WD_STALL_MS` (60000) old exists. If the callbacks arrive even 1 ms faster than 5000 ms by `timer_get_ms()`, 12 periods are under 60 s, the oldest sample is overwritten first, and `bs_watchdog_check()` never returns 1 | supported | Reproduced with the real `bs_watchdog_check()` and a fully stopped UI loop: a 5000 or 5001 ms period fires at call 12; 4999 or 4990 ms never fires (2000 calls). The native tests only use exact 5000 ms periods. Fits the absence of any reset; the real period on the badge is not measured. |
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
