# Device-local home wallpaper

Wallpapers are data outside the application code. Keep personal images and blobs
outside the repository; the build/CI does not need them.

| Firmware | Image | Flash address |
| --- | --- | --- |
| Streaming system overlay | 360x360 RGB565, native screen resolution | `0x2A0000` |
| Earlier menu/Display firmware | Up to 144x144 RGB565, enlarged to screen | `0x300000` |

The streaming firmware prefers the full-resolution slot, falls back to the legacy
small image and finally to a black screen with instructions. It has no embedded
default bitmap. The image is read a row at a time; 360x360 needs 259,216 bytes of
flash including its 16-byte header, without a full-image RAM buffer.

## Full resolution

Install the streaming system overlay firmware first (or provision the new slot
immediately before that OTA). Open **Settings -> Update**, then:

```sh
python tools/make_lockimg.py /private/my-picture.png /private/wallpaper.bin 360
python -m qix_ble.resources ADDRESS upload wallpaper /private/wallpaper.bin --reboot
```

This command requires the new [resource catalogue firmware](resources.md).
It checks the full record CRC and commits metadata through A/B. On older
streaming firmware, use
`qix rawflash ADDRESS /private/wallpaper.bin --addr 0x2A0000 --verify --reboot`;
that older verification samples bytes rather than checking the full CRC.

The converter requires Pillow. A normal fixed-geometry E87 application OTA
preserves the full-resolution slot and the separate device identity sector.
The new firmware limits OTA staging to end before the full-resolution slot.
Do not enlarge a 144px record at `0x300000`: a 360px record there would overwrite
the identity sector and the beginning of VIRFAT. See `badge-system.md` for bounds.

## Legacy compatibility

Use size `144` and address `0x300000` with older firmware. The converter retains
144 as its default for that compatibility. The record is `LKMG`, width/height
LE16, pixel-byte count LE32, reserved LE32, then RGB565 little-endian pixels.
Existing fixed-address records remain readable without a catalogue entry. Their
pixels have no stored CRC until re-uploaded through the catalogue client. With
catalogue firmware, `--addr 0x300000` uploads/registers a legacy-size record safely.
