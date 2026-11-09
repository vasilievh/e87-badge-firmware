# Battery status and device-local personalization

Development overlay. On 2026-11-09 the owner confirmed a sharp wallpaper on the
physical screen and no battery indicator on the home screen. Menu charging status
and Bluetooth tool operation on USB still need confirmation. Apply after `prepare-display.py`:

```sh
python scripts/prepare-badge-system.py
bash scripts/build-badge-fw.sh --board e87 --clean --ota firmware-builds/e87-display-v1.ufw
```

Current confirmed Display v2 build `49f91a4`: application 936,544 bytes in the
995,616-byte code slot, leaving 59,072 bytes; see [build status](ble-display.md).
The new [resource catalogue A/B](resources.md) is a later development change;
its vendor build, application size and hardware behavior are not yet verified.
The two builds below are historical. After changing overlay headers, use a fresh
SDK checkout/import and reapply Display followed by this overlay; `--clean` alone
does not refresh installed overlays.

Previous clean SDK build (before streaming wallpaper): source `29d7d72`,
[Actions run 42534138963](https://github.com/vasilievh/jieli-ble-badge-research/actions/runs/42534138963).
The application is 991,456 bytes in the fixed 995,616-byte code slot (4,160 bytes
remaining). The Qix OTA image SHA256 is
`307057e204675e7457f0a68b85789812982e44037fabf2b429ce95e83c14117c`.
Offline CRC, geometry and OEM loader/configuration preservation checks passed.
Battery/USB UI behavior and local identity still need physical confirmation.

Streaming build: source `57d879d`,
[Actions run 42536832540](https://github.com/vasilievh/jieli-ble-badge-research/actions/runs/42536832540).
The application is 950,048 bytes, leaving 45,568 bytes in the same code slot.
Image SHA256: `6397c0a4dd9f21943887b6c8bd29e8afed19eec5b6ed4e5844fdcbcb680f7868`.
The clean SDK build and offline integrity/geometry/preservation checks passed.
The owner confirmed the new decoder's image was sharp and the home indicator absent.

The host allows 30 seconds for the initial C0 ATT write acknowledgement: staging
erase is synchronous and can exceed the old five-second write timeout. This is
separate from the C1 notification timeout. Ordinary chunk writes retain the
five-second timeout. It does not add automatic retries of staging initialization.

The menu shows the SDK battery percentage and BATTERY, CHARGING or FULL. The home
screen is reserved for the wallpaper. Charger presence combines the SDK flag with the LDO5V/LVCMP hardware
detectors, sampled every 100 ms with three matching samples per transition.
FULL comes from the SDK charge-full event; this is not a current measurement.
The percentage is the SDK's voltage-based estimate, not a calibrated fuel gauge.

USB attachment updates the indicator without stopping Bluetooth tools, changing
the active screen or changing saved brightness. All tools remain usable on USB.
Update retains its existing 10% dimmer while waiting on USB and its transfer/session
deferral. No charger voltage/current limits, power-off thresholds or PMU settings
are changed by this overlay. Operation of each tool on USB still needs hardware
verification on the target badge.

**Open risk.** Upstream's charge screen deliberately kept the radio off for the
whole USB session: its comment in `badge-menu.patch` (`badge_charge_screen_enter`)
documents a brownout lockup when the radio runs while USB and the battery share
the supply. This overlay removes that screen, and the low-battery BLE gate is
bypassed while USB is present, so the radio can now run in exactly that state.
Test with a drained battery on USB (Scanner or Display open for 10–15 minutes)
before relying on it.

## Safety net (`badge_safety.h`)

Added after a hang on 2026-11-09: the badge was suspended (panel off) on USB, was
woken with Button 2 and stayed black, then ran the UI only every few tens of
seconds, warm, ignoring touch and buttons, until the battery was drained. There is
no reset button and no wired recovery on this unit, so the firmware now protects
itself. RAM only; nothing is written to flash.

- **TE guard.** The SDK's `lcd_drv_power_off()` leaves the panel TE pin (PC00) in
  `PORT_HIGHZ` while its falling-edge interrupt stays armed, so the input floats
  for the whole suspend. A registered LCD sleep handler pulls it down after
  power-off; the SDK's `lcd_init_task()` restores the pull-up on wake. Leading
  hypothesis for the hang and for the fast drain while locked, not yet confirmed.
- **TE counter.** A strong `lv_lcd_te_sync_int_handler()` (the SDK's is an empty
  weak hook in the LVGL build) counts TE interrupts, in total and per panel sleep.
- **UI watchdog.** A hardware-timer callback samples the LVGL loop counter every 5 s and
  resets the CPU (`local_irq_disable(); cpu_reset();`, the SDK's own ASSERT path) when,
  over the last minute of wall time, the loop made fewer than 15 passes. Normal: ~100/s
  awake, ~33/s suspended (~1/s with the fix/lock-power wait). It starts in `jl_gui_init()`,
  before the boot-time screen build and suspend, and is exempt while the Update screen is
  open or an update runs. Holding a button for roughly 45-60 s in a "wait for release"
  loop also resets.
- **Diagnostics.** With Display or Update open: `python -m qix_ble.safety ADDRESS`
  (Qix 0x97 -> 0x98, read-only). After a suspend, `last_sleep.te_per_second` near
  zero means the guard works; thousands per second would confirm the storm.

## Keep personal values out of Git and CI

Firmware is generic. No personal name or wallpaper is compiled into it. Generate
private records **outside the repository** and provision them directly over BLE
from the Update screen. Do not use Display mode for raw-flash commands.

The full-resolution wallpaper slot is `0x2A0000..0x2E0000`. A 360x360 RGB565
record occupies 259,216 bytes, ending at `0x2DF48F`. The new firmware caps OTA
staging at `0x2A0000`; the fixed E87 OTA image ends at `0x285828` when staged,
leaving a gap before the wallpaper. It cannot overlap the identity sector at
`0x30B000` or VIRFAT at `0x33E000`. The original `0x300000` wallpaper slot remains
a fallback for records up to 144x144; it is not enlarged into adjacent data.

The LVGL custom decoder reads visible row spans directly from flash. At 360x360
there is no image resampling and no full-image RAM allocation. A 720-byte scratch
row supports older smaller images; LVGL also uses its own temporary row buffer.
The 41,472-byte embedded bitmap and old 41,472-byte static RAM image buffer are
removed. With no valid private wallpaper, the existing black home/instructions
remain usable. Use `tools/make_lockimg.py`; see `lock-image.md`.

An ordinary fixed-geometry app-only OTA preserves these records. Full
resource/flash replacements may not.

The optional font pack lives at `0x310000..0x33E000` and replaces the built-in
36px font in code; see [flash-fonts.md](flash-fonts.md).

```sh
python tools/make_badge_identity.py YOUR_NAME /private/location/identity.bin
python -m qix_ble.resources ADDRESS upload identity /private/location/identity.bin --reboot
```

The catalogue firmware uses the checked resource uploader above; direct raw
writes to the resource area are blocked. With older firmware only, use
`qix rawflash ADDRESS /private/location/identity.bin --addr 0x30B000 --verify --reboot`.
Existing unregistered identity/wallpaper/font records remain readable at their
fixed addresses, with their original format checks, until re-uploaded.

Identity record: 32 bytes, `BID1`, version u8=1, name length u8, CRC16-CCITT LE16,
24 name/padding bytes. CRC uses seed 0xffff over version/length then name bytes.
Only 1..20 printable ASCII name bytes are accepted. Invalid/erased records leave
the default names unchanged. Mouse mode appends ` Mouse` to an accepted name;
the normal GAP name uses the base name. MAC addresses, GATT UUIDs and protocol
identifiers remain unchanged. Hosts may cache a previously paired device name.

The build workflow neither reads nor uploads device personalization. The SDK,
overlay manifest and generated private blobs must not be added to Git.
