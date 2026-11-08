# Battery status and device-local personalization

Development overlay, not yet hardware-tested. Apply after `prepare-display.py`:

```sh
python scripts/prepare-badge-system.py
bash scripts/build-badge-fw.sh --board e87 --clean --ota firmware-builds/e87-display-v1.ufw
```

Reference clean SDK build: source `29d7d72`,
[Actions run 42534138963](https://github.com/vasilievh/jieli-ble-badge-research/actions/runs/42534138963).
The application is 991,456 bytes in the fixed 995,616-byte code slot (4,160 bytes
remaining). The Qix OTA image SHA256 is
`307057e204675e7457f0a68b85789812982e44037fabf2b429ce95e83c14117c`.
Offline CRC, geometry and OEM loader/configuration preservation checks passed.
Battery/USB UI behavior and local identity still need physical confirmation.

The host allows 30 seconds for the initial C0 ATT write acknowledgement: staging
erase is synchronous and can exceed the old five-second write timeout. This is
separate from the C1 notification timeout. Ordinary chunk writes retain the
five-second timeout. It does not add automatic retries of staging initialization.

The home screen and menu show the SDK battery percentage and BATTERY, CHARGING
or FULL. Charger presence combines the SDK flag with the LDO5V/LVCMP hardware
detectors, sampled every 100 ms with three matching samples per transition.
FULL comes from the SDK charge-full event; this is not a current measurement.
The percentage is the SDK's voltage-based estimate, not a calibrated fuel gauge.

USB attachment updates the indicator without stopping Bluetooth tools, changing
the active screen or changing saved brightness. All tools remain usable on USB.
Update retains its existing 10% dimmer while waiting on USB and its transfer/session
deferral. No charger voltage/current limits, power-off thresholds or PMU settings
are changed by this overlay. Operation of each tool on USB still needs hardware
verification on the target badge.

## Keep personal values out of Git and CI

Firmware is generic. No personal name or wallpaper is compiled into it. Generate
private records **outside the repository** and provision them directly over BLE
from the Update screen. Do not use Display mode for raw-flash commands.

The existing wallpaper slot starts at `0x300000`, accepts at most 144x144 RGB565
and scales to the 360x360 screen. Use `tools/make_lockimg.py`; see `lock-image.md`.
The separate identity sector starts at `0x30B000`, after the largest wallpaper's
last byte (`0x30A20F`), below VIRFAT (`0x33E000`) and above the OTA staging range.
An ordinary app-only OTA preserves both. Full resource/flash replacements may not.

```sh
python tools/make_badge_identity.py YOUR_NAME /private/location/identity.bin
qix rawflash ADDRESS /private/location/identity.bin --addr 0x30B000 --verify --reboot
```

Identity record: 32 bytes, `BID1`, version u8=1, name length u8, CRC16-CCITT LE16,
24 name/padding bytes. CRC uses seed 0xffff over version/length then name bytes.
Only 1..20 printable ASCII name bytes are accepted. Invalid/erased records leave
the default names unchanged. Mouse mode appends ` Mouse` to an accepted name;
the normal GAP name uses the base name. MAC addresses, GATT UUIDs and protocol
identifiers remain unchanged. Hosts may cache a previously paired device name.

The build workflow neither reads nor uploads device personalization. The SDK,
overlay manifest and generated private blobs must not be added to Git.
