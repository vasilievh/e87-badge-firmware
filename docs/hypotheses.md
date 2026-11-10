# Hypotheses log

Every explanation we act on starts here as a hypothesis. Keep rejected ones: they save the
next person from chasing them again. Newest incident first.

Status: **open** (not yet tested), **supported** (evidence agrees, not proven),
**confirmed** (measured), **rejected** (evidence contradicts).

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
| The first redraw never completes: `lv_disp_flush_wait_finish()` (SDK `lv_port_disp.c`) waits for the flush-done callback with no way out, retrying every 100 ms and printing `lv_disp_flush_wait_finish timeout..` | open | Code reading only. Would freeze the UI task while the rest of the system (and the idle task) keeps running. Test: a debug UART log of a hang, or a bounded wait that records how often it gives up. |
| The SDK feeds the hardware watchdog from the OS layer (idle task or tick), so a blocked UI task never trips it | open | Code reading: `wdt_clear` is defined only in `cpu.a` (`wdt_p33.c`); the closed `system.a` (`os_cpu_c_overwrite_v2.c`) references `os_wdt_clr` among idle-task symbols. Fits both hangs lasting until the battery drained. Only interrupts off or a spinning CPU would then trip it. |
| A second key press while `badge_lock_wake()` is still running causes the hang | open | The owner may have pressed twice. No mechanism found yet: the extra press only sets `badge_key_wake`, which the next Lock clears. Test: single short presses only for a while, then compare. |

## 2026-11-09: hang after waking on USB

Symptom: badge suspended with Button 1 while charging at 100%, woken with Button 2. Screen
stayed black, then showed the wallpaper and later the menu, but the UI updated only every
few tens of seconds, ignored touch and almost all key presses, and the badge got warm.
Unplugging USB did not help; holding both buttons for 15 s did not reset it. It stayed in
that state until the battery was completely drained (about 3.5 h); after charging it booted
normally. This confirms the last-resort recovery for a hang on this unit: drain fully, then
charge for a cold boot. Firmware: `dev` before the resource catalogue.

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
