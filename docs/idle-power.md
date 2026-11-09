# E87 Lock power investigation

Prepared in the isolated local branch `fix/lock-power`, worktree `work/badge-power`.
The existing `work/badge-dev` branch and its local documentation edit are preserved.

Status: implementation and host/native verification only. No firmware containing
these changes has been built with the JieLi toolchain or installed on a badge.
Hardware current, wake reliability and battery-life improvement are unmeasured.
Before resource integration: 312 Python tests passed, 2 skipped; all five native
C harnesses passed. Combined validation is recorded below.
After integration: 338 Python tests passed, 2 skipped; all six native C harnesses
passed, including interrupted A/B writes and idle/wake handling. Both combined
overlays also applied and passed their manifests on the full pinned SDK sources.

The reported symptom was a fall from a displayed 100% to 11% after about twelve
hours with the tools closed and LCD off, followed by 9% after waking. Percent is
the SDK's voltage-derived, smoothed estimate. It is not a charge/current meter;
these readings alone do not establish the energy consumed or a new regression.

## Findings and changes

The inherited Lock path powers down the LCD/shared touch rail and skips LVGL,
but leaves hardware low-power mode disabled (`TCFG_LOWPOWER_LOWPOWER_SEL=0`).
The upstream patch documents boot hangs with mode 2 on its tested hardware.
This change does not enable that mode or change charge/protection thresholds.

The clock is left to the SDK. An earlier version of this branch called
`clock_unlock("ui_show")` after `lcd_sleep_ctrl(1)` and `clock_lock()` before
`lcd_sleep_ctrl(0)`, assuming the custom Lock path never released the UI's
max-frequency lock. Reading the SDK (`cpu/br35/ui_driver/lcd_drive/lcd_drive.c`)
showed `lcd_sleep_ctrl()` already does exactly that, and the clock manager's locks
are per owner name, so the extra calls only duplicated it and were removed. The
diagnostics still sample the system clock and record the lowest value seen while
locked; the clock API result fields now always read `127` (not attempted).

The UI previously polled every ~30 ms while locked; it now waits on a semaphore
for a debounced key transition, with a 1-second timeout for USB/power sampling.
The original ADKEY scanner and debounce are retained. A post immediately before
the wait is retained by the semaphore, so a short press is not dependent on the
1-second timeout. The touch task previously woke every ~20 ms even when disabled;
it now waits until the UI enables it, then performs its existing re-init.
If semaphore creation fails, bounded polling remains available.

These are task/clock improvements, **not CPU deep sleep**. The key scanner, SDK
tick, battery monitoring, clock calibration and other SDK tasks remain active.
Neither a specific standby current nor multi-day life is promised.

## Read-only diagnostics

After a future test firmware is built and explicitly installed, wake the badge,
open **Bluetooth Display**, and use the tool from this checkout:

```sh
cd tools/qix-ble
python -m qix_ble.power <BLE-address>
```

The module sends only `0x92` read requests. It does not run the OEM bootstrap,
send a Display card, enable pairing, reset, erase or flash. It works with Display
open or Update waiting without an active transfer; prefer Display. The idle radio
stays off after normal exit from a tool. Reading requires waking and opening the
radio; the last completed Lock interval's cached measurements survive that step.
Do not run diagnostics concurrently with an OTA/resource transfer.
If the OTA environment toggle `QIX_PAIR` is enabled, this command refuses to
connect; leave it unset or `0` for this read-only tool.

Output includes cached voltage in mV, SDK percent, USB/full flags, desired BLE
state, configured hardware low-power mode, system clock, lowest system clock
sampled during the most recent Lock interval, and clock API results (always `127`,
not attempted: the SDK manages the clock). `ble_wanted` reports software intent, not
proof that the controller/analog radio is physically powered off.

`last_lock` records duration and start/end mV/percent; the end is captured before
LCD power-up. Values come from the SDK cache, not a fresh ADC conversion. A zero
voltage early at boot is uninitialized/unknown. The current and lowest locked
clock may differ due to adaptive calibration. Snapshots are RAM-only, reset on
reboot, and individually atomic per page, not across all four requests. Millisecond
timer arithmetic assumes a Lock interval shorter than 2^32 ms (~49.7 days).

Protocol: requests are `BP`, version `1`, page `0..3`; responses use `0x93` and
repeat that 4-byte header followed by 10 bytes. Each complete Qix response is 20
bytes and fits ATT MTU 23. Integers are little-endian.

| Page | Bytes after header |
| --- | --- |
| 0 | flags u8, percent u8, mV u16, uptime seconds u32, low-power config u8, reserved zero u8 |
| 1 | last start percent u8, last end percent u8, last start mV u16, last end mV u16, last Lock seconds u32 |
| 2 | current Lock seconds u32 (zero if awake), UI Lock waits since boot u32, reserved zero u16 |
| 3 | current system Hz u32, lowest most-recent Lock Hz u32, unlock result i8, restore result i8 |

Flags: bit 0 locked, bit 1 USB present, bit 2 SDK full flag, bit 3 BLE desired,
bit 4 completed Lock interval available. Without bit 4, page 1 is not a completed
measurement. Malformed/unsupported requests are ignored without state changes.

## Validation and next hardware check

Host tests check the exact overlay anchors, refusal to partially modify a drifted
SDK, protocol sizes/parsing and that diagnostics send only read commands. The C
harness compiles the actual headers against an OS/clock model, covering retained
wakes, timeout/allocation-failure paths, clock-owner refusal, interval rollover,
repeated locks and malformed requests. It does not emulate the SDK's PMU/RTOS.
The full pinned SDK archive was SHA256-verified, imported, patched and both
overlays/manifests successfully checked locally, beyond the partial-source test
fixtures. That validates source integration, not target compilation.

Apply the updated Display/system overlays to a **fresh pinned SDK** for a full
JieLi build. An old installed system overlay is deliberately rejected by its
manifest rather than silently mixed with these headers. The resource catalogue
and bounded BLE writes from `dev`, formerly `feat/ble-display` (`6d906f1`), are now integrated into
this power branch. The shared overlay installs and validates both resource and
idle headers, and the native runner includes both harnesses. There is no new
battery-life result or application-size measurement for this code yet.

After a successful build, size/integrity checks and a separately authorized
installation: first verify several short Lock/wake cycles, display/touch and
normal tool operation. Then compare equal unplugged Lock intervals at the same
brightness/starting charge, keeping tools closed. Read `last_lock` after waking
and check that the observed idle clock actually falls. Compare cached mV as well
as percent; a direct current measurement would be needed to quantify consumption.
Do not start with another unattended overnight run until wake/stability pass.
