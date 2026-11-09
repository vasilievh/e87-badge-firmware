import struct

import pytest

from qix_ble.frame import QixFrame
from qix_ble.power import PowerClient, PowerError
from tests.conftest import MockTransport


def frame(page, data):
    return QixFrame(flags=2, cmd=0x93, payload=b"BP" + bytes((1, page)) + data)


def responses(flags=24):
    return [frame(0, struct.pack("<BBHIBB", flags, 9, 3450, 44000, 0, 0)),
            frame(1, struct.pack("<BBHHI", 100, 11, 4200, 3470, 43200)),
            frame(2, struct.pack("<IIH", 0, 43201, 0)),
            frame(3, struct.pack("<IIbb", 240000000, 24000000, 0, 0))]


def test_status_has_sleep_evidence_and_only_sends_small_read_commands():
    transport = MockTransport()
    transport.queue_responses(*responses())
    status = PowerClient(transport).status()
    assert status["battery_mv"] == 3450 and status["percent"] == 9
    assert status["last_lock"] == {"seconds": 43200, "before_percent": 100,
                                   "after_percent": 11, "before_mv": 4200, "after_mv": 3470}
    assert status["lowpower_config"] == 0 and status["last_idle_hz"] == 24000000
    assert status["sys_hz"] == 240000000 and status["ble_wanted"]
    assert transport.sent == [(0x92, b"BP" + bytes((1, page)), 2) for page in range(4)]
    assert transport.write_modes == [(0x92, True, False)] * 4
    assert all(len(reply.encode()) == 20 for reply in responses())


def test_no_completed_lock_is_distinct_from_zero_second_lock():
    transport = MockTransport()
    transport.queue_responses(*responses(flags=8))
    assert PowerClient(transport).status()["last_lock"] is None


def test_stale_page_is_skipped():
    transport = MockTransport()
    transport.queue_responses(responses()[1], responses()[0])
    assert PowerClient(transport).page(0) == responses()[0].payload[4:]


@pytest.mark.parametrize("length", [4, 13, 15, 100])
def test_truncated_or_extended_response_is_rejected(length):
    transport = MockTransport()
    transport.queue_response(QixFrame(flags=2, cmd=0x93, payload=(b"BP\1\0" + bytes(100))[:length]))
    with pytest.raises(PowerError, match="length"):
        PowerClient(transport).page(0)


@pytest.mark.parametrize("field,value", [(0, 128), (1, 101), (9, 1)])
def test_invalid_status_fields_are_rejected(field, value):
    pages = responses()
    data = bytearray(pages[0].payload)
    data[4 + field] = value
    pages[0] = QixFrame(flags=2, cmd=0x93, payload=bytes(data))
    transport = MockTransport()
    transport.queue_responses(*pages)
    with pytest.raises(PowerError, match="fields"):
        PowerClient(transport).status()


def test_invalid_page_sends_nothing():
    transport = MockTransport()
    with pytest.raises(ValueError):
        PowerClient(transport).page(4)
    assert not transport.sent


def test_cli_does_not_connect_when_shared_transport_pairing_is_enabled(monkeypatch):
    from qix_ble.power import main
    monkeypatch.setenv("QIX_PAIR", "1")
    def unexpected_connection(*args, **kwargs):
        pytest.fail("must reject pairing before connecting")
    monkeypatch.setattr("qix_ble.transport.QixTransport", unexpected_connection)
    with pytest.raises(SystemExit) as error:
        main(["00:11:22:33:44:55"])
    assert error.value.code == 2


def test_cli_reads_and_closes_without_bootstrap(monkeypatch, capsys):
    import json
    from qix_ble.power import main
    transport = MockTransport()
    transport.queue_responses(*responses())
    closed = []
    class Connection:
        def __enter__(self):
            return transport
        def __exit__(self, *args):
            closed.append(True)
    monkeypatch.delenv("QIX_PAIR", raising=False)
    monkeypatch.setattr("qix_ble.transport.QixTransport", lambda address: Connection())
    assert main(["00:11:22:33:44:55"]) == 0
    assert closed and json.loads(capsys.readouterr().out)["battery_mv"] == 3450
    assert [cmd for cmd, _, _ in transport.sent] == [0x92] * 4
