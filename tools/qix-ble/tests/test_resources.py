import struct
import zlib

import pytest

from qix_ble.frame import QixFrame
from qix_ble.errors import TimeoutError as QixTimeoutError
from qix_ble.resources import (
    ABORT, BEGIN, FINISH, PROBE, COMMIT, MAGIC, TABLE_A, TABLE_B,
    Entry, ResourceClient, ResourceError, decode_catalog, make_entry,
)
from tests.conftest import MockTransport


def wallpaper():
    return struct.pack("<4sHHII", b"LKMG", 16, 16, 512, 0) + bytes(range(256)) * 2


def catalog(generation, entries):
    body = b"".join(entry.encode() for entry in entries)
    header = struct.pack("<8I", MAGIC, generation, len(entries), 32 + len(body), 0, 0, 0, COMMIT)
    crc = zlib.crc32(header[:16] + header[20:28] + body)
    return header[:16] + struct.pack("<I", crc) + header[20:] + body


def ack(op, sequence, status=0, generation=1):
    return QixFrame(flags=0, cmd=0x95, payload=b"BR\x01" + bytes((op | 0x80,))
                    + struct.pack("<HB I", sequence, status, generation))


def test_catalog_round_trip_and_crc_covers_metadata_and_payload():
    entry = make_entry("wallpaper", wallpaper())
    data = catalog(9, [entry])
    assert decode_catalog(data) == (9, [entry])
    for index in range(len(data)):
        bad = bytearray(data)
        bad[index] ^= 1
        with pytest.raises(ValueError):
            decode_catalog(bytes(bad))


@pytest.mark.parametrize("entry", [
    Entry(1, 1, TABLE_A, 528, 0),
    Entry(1, 1, 0x2A0001, 528, 0),
    Entry(1, 1, 0x2A0000, 0xFFFFFFFF, 0),
    Entry(2, 1, 0x30B000, 32, 0),
    Entry(3, 1, 0x310000, 32, 0),
    Entry(3, 2, 0x30B000, 32, 0),
    Entry(4, 1, 0x2A0000, 528, 0),
    Entry(1, 1, 0x2A0000, 0, 0),
    Entry(1, 1, 0, 0, 1),
])
def test_invalid_entries_rejected(entry):
    with pytest.raises(ValueError):
        entry.validate()


def test_unavailable_and_slot_edges():
    Entry(1, 1, 0, 0, 0).validate()
    Entry(1, 1, 0x2A0000, 259216, 0).validate()
    Entry(1, 1, 0x300000, 41488, 0).validate()
    Entry(2, 1, 0x310000, 188416, 0).validate()
    Entry(3, 1, 0x30B000, 32, 0).validate()


def test_unknown_firmware_and_invalid_blob_cannot_write(monkeypatch):
    transport = MockTransport()
    client = ResourceClient(transport)
    with pytest.raises(ValueError):
        client.upload("wallpaper", b"invalid")
    assert not transport.sent
    # No capability ACK -> no BEGIN, raw erase/write, OTA or fallback writes.
    with pytest.raises(QixTimeoutError):
        client.upload("wallpaper", wallpaper())
    assert [cmd for cmd, _, _ in transport.sent] == [0x94]
    assert transport.sent[0][1][3] == PROBE


def test_upload_orders_probe_begin_data_full_crc_commit(monkeypatch):
    transport = MockTransport()
    for seq, op in enumerate((PROBE, BEGIN, FINISH), 1):
        transport.queue_response(ack(op, seq, generation=seq))
    client = ResourceClient(transport)
    calls = []

    def flash(address, data, verify):
        assert [p[3] for _, p, _ in transport.sent] == [PROBE, BEGIN]
        calls.append((address, data, verify))

    monkeypatch.setattr(client.raw, "flash_region", flash)
    assert client.upload("wallpaper", wallpaper()) == 3
    assert calls == [(0x2A0000, wallpaper(), False)]
    assert transport.sent[1][1][6:] == make_entry("wallpaper", wallpaper()).encode()
    assert all(response and not fragment for _, response, fragment in transport.write_modes)


def test_failed_upload_aborts_without_commit_or_reboot(monkeypatch):
    transport = MockTransport()
    for seq, op in enumerate((PROBE, BEGIN, ABORT), 1):
        transport.queue_response(ack(op, seq))
    client = ResourceClient(transport)

    def fail(*args, **kwargs):
        raise OSError("link dropped during raw write")

    monkeypatch.setattr(client.raw, "flash_region", fail)
    with pytest.raises(OSError):
        client.upload("wallpaper", wallpaper(), reboot=True)
    assert [p[3] for _, p, _ in transport.sent] == [PROBE, BEGIN, ABORT]


def test_failed_commit_is_reported_and_aborted(monkeypatch):
    transport = MockTransport()
    transport.queue_responses(ack(PROBE, 1), ack(BEGIN, 2), ack(FINISH, 3, status=5), ack(ABORT, 4))
    client = ResourceClient(transport)
    monkeypatch.setattr(client.raw, "flash_region", lambda *a, **kw: None)
    with pytest.raises(ResourceError, match="CRC"):
        client.upload("wallpaper", wallpaper())
    assert [p[3] for _, p, _ in transport.sent] == [PROBE, BEGIN, FINISH, ABORT]


@pytest.mark.parametrize("bad_ack", [ack(PROBE, 7), ack(BEGIN, 1),
                                      QixFrame(flags=0, cmd=0x95, payload=b"BR")])
def test_bad_sequence_operation_or_length_rejected(bad_ack):
    transport = MockTransport()
    transport.queue_response(bad_ack)
    with pytest.raises(ResourceError, match="acknowledgement"):
        ResourceClient(transport).request(PROBE)


@pytest.mark.parametrize("generations,expected", [((7, 8), 8), ((0xFFFFFFFF, 0), 0), ((8, 7), 8)])
def test_read_catalog_selects_newest_and_handles_wrap(monkeypatch, generations, expected):
    transport = MockTransport()
    transport.queue_response(ack(PROBE, 1))
    client = ResourceClient(transport)
    entry = make_entry("wallpaper", wallpaper())
    tables = {TABLE_A: catalog(generations[0], [entry]), TABLE_B: catalog(generations[1], [entry])}

    def read(address, length):
        base = TABLE_A if address < TABLE_B else TABLE_B
        return tables[base][address-base:address-base+length]

    monkeypatch.setattr(client.raw, "read", read)
    assert client.catalog()["generation"] == expected


def test_torn_catalog_is_ignored(monkeypatch):
    transport = MockTransport()
    transport.queue_response(ack(PROBE, 1))
    client = ResourceClient(transport)
    entry = make_entry("wallpaper", wallpaper())
    a, b = catalog(1, [entry]), bytearray(catalog(2, [entry]))
    b[28] = 0xFF
    tables = {TABLE_A: a, TABLE_B: bytes(b)}
    monkeypatch.setattr(client.raw, "read", lambda address, n:
                        tables[TABLE_A if address < TABLE_B else TABLE_B][address % 4096:address % 4096+n])
    assert client.catalog()["generation"] == 1


def test_catalog_read_fits_default_notification_mtu_and_rejects_short_reads(monkeypatch):
    client = ResourceClient(MockTransport())
    observed = []

    def read(address, length):
        observed.append(length)
        return bytes(length)

    monkeypatch.setattr(client.raw, "read", read)
    assert client._read(TABLE_A, 104) == bytes(104)
    assert max(observed) == 13
    monkeypatch.setattr(client.raw, "read", lambda *args: b"")
    with pytest.raises(ResourceError, match="short raw read"):
        client._read(TABLE_A, 32)
