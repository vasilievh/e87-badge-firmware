# Hypotheses log

Every explanation we act on starts here as a hypothesis. Keep rejected ones: they save the
next person from chasing them again. Newest incident first.

Status: **open** (not yet tested), **supported** (evidence agrees, not proven),
**confirmed** (measured), **rejected** (evidence contradicts).

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
| The SDK hardware watchdog (`WDT_LRC_4S` at run time) resets the badge when interrupts stay off; the safety reset relies on it | open | Code reading: this build's SDK `ASSERT` is `local_irq_disable(); while (1);` and nothing else resets it. Test: hold button 2 on the dark lock screen; a reset within about 35-40 s confirms the path (the `cpu_reset()` fallback would take far longer). |
