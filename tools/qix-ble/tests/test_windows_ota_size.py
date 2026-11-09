import struct
import pytest
from bleak.exc import BleakError
from types import SimpleNamespace
from qix_ble.errors import BleConnectionError
from qix_ble.frame import QixFrame
from qix_ble.service import CHAR_FD02_WRITE
from qix_ble.transport import QixTransport
from qix_ble.update_manager import QixUpdater
from tests.conftest import MockTransport
from tests.test_update_manager import make_valid_ufw

FIRMWARE_RX_BUFFER = 1100  # qix_rx_buf in qix_ota_server.c


def run_flash(monkeypatch, platform, size, offsets, safe=None):
    monkeypatch.setattr("qix_ble.update_manager.sys.platform", platform)
    if safe is None:
        monkeypatch.delenv("QIX_OTA_SAFE", raising=False)
    else:
        monkeypatch.setenv("QIX_OTA_SAFE", safe)
    transport = MockTransport()
    transport.queue_response(QixFrame(flags=0, cmd=0xc1, payload=struct.pack(">BII", 1, 1024, 0)))
    for offset in offsets:
        transport.queue_response(QixFrame(flags=0, cmd=0xc3, payload=struct.pack("<BI", 0, offset)))
    transport.queue_response(QixFrame(flags=0, cmd=0xc5, payload=b"\x00"))
    QixUpdater(transport).flash(make_valid_ufw(size))
    frames = [QixFrame(flags=flags, cmd=cmd, payload=data).encode()
              for cmd, data, flags in transport.sent if cmd == 0xc2]
    modes = [(response, fragment) for cmd, response, fragment in transport.write_modes if cmd == 0xc2]
    return frames, modes


def test_windows_sends_full_chunks_as_fragmented_unacked_writes(monkeypatch):
    frames, modes = run_flash(monkeypatch, "win32", 2500, (1024, 2048))
    assert len(frames) == 3 and max(map(len, frames)) == 6 + 8 + 1024
    assert all(len(frame) <= FIRMWARE_RX_BUFFER for frame in frames)
    assert set(modes) == {(False, True)}


def test_windows_safe_mode_keeps_previous_512_byte_writes(monkeypatch):
    frames, modes = run_flash(monkeypatch, "win32", 1000, (480, 960), safe="1")
    assert len(frames) == 3 and max(map(len, frames)) == 494
    assert all(len(frame) <= 512 for frame in frames)
    assert set(modes) == {(True, False)}


def test_linux_mode_is_unchanged(monkeypatch):
    frames, modes = run_flash(monkeypatch, "linux", 2500, (1024, 2048))
    assert max(map(len, frames)) == 6 + 8 + 1024
    assert set(modes) == {(True, False)}


def make_transport(monkeypatch, mtu, fail_at=None, max_write_size=512):
    writes = []

    async def write(char, data, response):
        writes.append((bytes(data), response))

    def submit(coroutine, loop):
        try:
            coroutine.send(None)
        except StopIteration:
            pass
        if fail_at is not None and len(writes) == fail_at:
            raise BleakError("link lost")
        return SimpleNamespace(result=lambda timeout: None)

    monkeypatch.setattr("qix_ble.transport.asyncio.run_coroutine_threadsafe", submit)
    transport = QixTransport("00:11:22:33:44:55")

    def get_characteristic(uuid):
        assert uuid == CHAR_FD02_WRITE
        return SimpleNamespace(max_write_without_response_size=max_write_size)

    transport._client = SimpleNamespace(
        is_connected=True, write_gatt_char=write, mtu_size=mtu,
        services=SimpleNamespace(get_characteristic=get_characteristic),
    )
    return transport, writes


@pytest.mark.parametrize("mtu,max_write_size,expected_step", [
    (23, 20, 20),
    (185, 182, 182),
    (247, 244, 244),
    (517, 512, 512),
    (517, 244, 244),
    (247, 100, 100),
    (517, 1024, 512),
])
def test_fragmented_frame_is_reassembled_byte_exactly(
    monkeypatch, mtu, max_write_size, expected_step,
):
    transport, writes = make_transport(monkeypatch, mtu, max_write_size=max_write_size)
    frame = QixFrame(flags=6, cmd=0xC2, payload=bytes(range(256)) * 4 + b"12345678")
    transport.send_raw(frame, fragment=True)
    assert b"".join(data for data, _ in writes) == frame.encode()
    assert max(len(data) for data, _ in writes) == expected_step
    assert all(len(data) <= expected_step and response is False for data, response in writes)


def test_fragment_write_failure_becomes_resumable_link_error(monkeypatch):
    transport, _ = make_transport(monkeypatch, 247, fail_at=2)
    with pytest.raises(BleConnectionError):
        transport.send_raw(QixFrame(flags=6, cmd=0xC2, payload=bytes(1032)), fragment=True)
