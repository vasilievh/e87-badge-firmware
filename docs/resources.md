# E87 resource catalogue A/B (BRC1)

Development implementation; not yet built with the vendor SDK or checked on a
physical badge. It is included by the system overlay after the Display overlay.
Existing confirmed build sizes in the other documents predate this change.
No personal resource is compiled into the firmware or used by CI.

## Use

On firmware with this catalogue, open **Settings -> Update**, install the current
PC client (`python -m pip install -e tools/qix-ble`) and run from the repository root:

```sh
python -m qix_ble.resources ADDRESS list
python -m qix_ble.resources ADDRESS upload wallpaper /private/wallpaper.bin --reboot
python -m qix_ble.resources ADDRESS upload fonts /private/fonts.bin --reboot
python -m qix_ble.resources ADDRESS upload identity /private/identity.bin --reboot
```

Generate blobs with `tools/make_lockimg.py`, `tools/make_badge_fonts.py` and
`tools/make_badge_identity.py`. Keep them and their source images outside Git.
The default addresses are in the table below; `--addr 0x...` can select another
sector-aligned address within that resource's window. A 144px legacy wallpaper
can use `--addr 0x300000`.

The client probes protocol support before BEGIN or any erase/write. Old firmware
does not support these commands; use its documented `qix rawflash` provisioning
path until installing the new firmware. There is no automatic legacy-write fallback.
`list` reads metadata; it does not recheck all resource bytes or alter flash.
It reports an unavailable entry with address/length/CRC all zero.

## Flash layout and costs

All ranges exclude the end address. Only the E87 layout is supported; the
implementation rejects a DG01 build rather than writing its different resource map.

| Use / ID | Allowed allocation window | Default address | Maximum record |
| --- | --- | --- | --- |
| Wallpaper / 1 | `0x2A0000..0x2E0000` | `0x2A0000` | 259,216 bytes |
| Legacy wallpaper / same ID 1 | `0x300000..0x30B000` | explicit `--addr` | 41,488 bytes |
| Fonts / 2 | `0x310000..0x33E000` | `0x310000` | 188,416 bytes |
| Identity / 3 | `0x30B000..0x30C000` | `0x30B000` | 32 bytes |
| Catalogue A | `0x2E0000..0x2E1000` | — | one 4 KiB erase sector |
| Catalogue B | `0x2E1000..0x2E2000` | — | one 4 KiB erase sector |

The two catalogue sectors reserve **8 KiB**; each current record uses at most
104 bytes (three entries). The firmware uses one small cached catalogue, an
upload descriptor, validation flags and a 128-byte streaming CRC buffer plus
bounded stack scratch. There is no full-resource RAM buffer or second payload
slot. The actual application-size increase must be measured with a new SDK build.

OTA staging ends at `0x2A0000`, so a fixed-geometry app-only OTA does not erase the
catalogues or resources. A full flash/resource replacement or older firmware
that does not implement this layout can overwrite them.

## Upload and interruption behavior

1. BEGIN validates ID, format, length and sector-aligned allocation. The allowed
   windows isolate resource types, catalogues, OTA staging and VIRFAT/config data.
   Only Update mode accepts resource commands. It locks Back before metadata
   writes, serialized with leaving the screen, so UI resource readers cannot run
   during replacement.
2. Before erasing payload, the firmware commits an **unavailable** entry in the
   inactive catalogue. This also protects the first migration of an unchecked
   legacy resource. A completed BEGIN invalidates the old resource even if the
   uploader disconnects before erasing it.
3. Existing raw erase/write commands transfer the payload only inside the active
   upload's bounds. Erase bounds include all affected 4 KiB sectors. Direct raw
   mutations in `0x2A0000..0x33E000`, including either catalogue, are rejected
   outside the matching upload. Raw reads remain available. Writes outside that
   region retain the existing firmware's protection rules.
4. FINISH reads **every byte** of the resource and checks CRC32 plus its basic
   format header/length. It writes the replacement entry into the inactive
   catalogue, reads it back, writes its commit marker last and verifies it again.
5. On boot, the newest structurally valid, CRC-correct committed catalogue wins.
   Generation comparison handles uint32 wrap. The selected entry's full resource
   CRC is checked on its first use and cached in RAM; it is not recomputed each frame.

Only the inactive catalogue sector is erased. A torn metadata erase/program or
commit leaves the previous valid catalogue usable. A failed data upload leaves
the resource unavailable: wallpaper falls back to the black home screen, fonts
to the built-in 14px font, identity to the default name. The previous payload is
not preserved. Reboot after a successful upload to reload fonts and identity.

On disconnect the in-memory write authorization is cleared. Start another upload
from BEGIN; raw writes cannot resume by themselves. Mutating requests are never
automatically retried after an uncertain acknowledgement. Each successful upload
normally advances the catalogue generation twice, once for invalidation and once
for publication. ABORT ends the write session but does not restore old contents.
BEGIN locks Back on the Update screen, including after ABORT, but the existing
disconnect handler clears that lock (`g_badge_update_active = 0`), so Back works
again once the PC disconnects. Fonts and identity loaded at boot are not reloaded
in RAM: until a reboot, text may be drawn from the old font metadata over the new
or partially written pack (bounded reads, garbled glyphs, no crash). Always finish
an upload with `--reboot`, or reboot before leaving the Update screen.

## Legacy compatibility and validation limits

An ID absent from the catalogue still uses its existing fixed-address loader.
Those legacy bytes have no catalogue CRC32 until re-uploaded with this client;
the font/identity formats retain their own existing checks. A registered ID with
a bad CRC or an unavailable entry **never falls back to unchecked bytes** at its
old address. Updating one ID leaves other registered entries and unregistered
legacy resources alone.

CRC detects accidental corruption; it is not authentication. Existing format
loaders still enforce their detailed constraints after the outer CRC check. The
three type windows are intentionally fixed in BRC1; this is not a general-purpose
filesystem or allocator. Upload resources serially while Update is open.

## Format and protocol

Words are little-endian. Catalogue header is eight u32 fields:
`magic=BRC1 | generation | count | length | crc32 | reserved=0 | reserved=0 | commit=DONE`.
It is followed by up to three sorted unique 24-byte entries, each six u32 fields:
`id | format=1 | address | length | crc32 | reserved=0`.
An unavailable entry has address, length and CRC all zero.

CRC32 uses the reflected IEEE polynomial `0xEDB88320`, initial/final XOR
`0xFFFFFFFF` (Python `zlib.crc32`). Catalogue CRC covers bytes 0..15, 20..27 and
all entries, excluding its CRC field and commit marker. Payload CRC covers the
entire resource record including its own header.

Requests use Qix opcode `0x94`, replies `0x95`, on existing FD02/FD01:
`BR | version:u8=1 | operation:u8 | sequence:LE16 | body`.
Operations: BEGIN=1 (one entry), FINISH=2, ABORT=3, PROBE=4 (no body for the latter three).
Reply: `BR | 1 | operation OR 0x80 | sequence:LE16 | status:u8 | generation:LE32`
(11-byte payload, 17-byte frame, fits ATT MTU 23). PROBE is read-only.
Statuses: 0 success, 1 malformed/version, 2 bounds/value, 3 busy, 4 flash I/O,
5 CRC/header mismatch, 6 no upload, 7 Update inactive. Display mode rejects resource/raw-flash
commands; it remains RAM-only.

## Offline tests

```sh
python -m pytest -q tools/qix-ble/tests/test_resources.py tests/test_system_overlay.py
python scripts/test-display-native.py
```

The native resource harness uses the actual implementation on fake NOR flash,
including torn catalogue erases, interruption at each programmed byte/commit
byte, generation wrap, full-payload corruption, failed reads, legacy migration,
disconnects and allocation/write/erase bounds. These do not replace a clean
vendor build or a physical power-loss test on the badge.
