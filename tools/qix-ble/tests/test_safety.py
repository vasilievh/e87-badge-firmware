import struct

import pytest

from qix_ble.frame import QixFrame
from qix_ble.safety import SafetyClient, SafetyError
from tests.conftest import MockTransport


def frame(page, data):
    return QixFrame(flags=2, cmd=0x98, payload=b"BS" + bytes((1, page)) + data)


def responses(flags=2, last=(1800000, 3600), now=(0, 0)):
    return [frame(0, struct.pack("<IIBB", 1800120, 987654, flags, 0)),
            frame(1, struct.pack("<IIH", *last, 0)),
            frame(2, struct.pack("<IIH", *now, 0))]


def test_status_reports_te_rate_of_last_sleep_with_read_only_commands():
    transport = MockTransport()
    transport.queue_responses(*responses())
    status = SafetyClient(transport).status()
    assert status["last_sleep"] == {"seconds": 3600, "te_interrupts": 1800000,
                                    "te_per_second": 500.0}
    assert status["current_sleep"] is None and status["ui_loops"] == 987654
    assert transport.sent == [(0x97, b"BS" + bytes((1, page)), 2) for page in range(3)]
    assert transport.write_modes == [(0x97, True, False)] * 3
    assert all(len(reply.encode()) == 20 for reply in responses())


def test_no_completed_sleep_and_zero_length_sleep():
    transport = MockTransport()
    transport.queue_responses(*responses(flags=1, now=(0, 0)))
    status = SafetyClient(transport).status()
    assert status["last_sleep"] is None
    assert status["current_sleep"] == {"seconds": 0, "te_interrupts": 0, "te_per_second": None}


def test_stale_page_is_skipped():
    transport = MockTransport()
    transport.queue_responses(responses()[1], responses()[0])
    assert SafetyClient(transport).page(0) == responses()[0].payload[4:]


@pytest.mark.parametrize("length", [4, 13, 15, 100])
def test_truncated_or_extended_response_is_rejected(length):
    transport = MockTransport()
    transport.queue_response(QixFrame(flags=2, cmd=0x98, payload=(b"BS\1\0" + bytes(100))[:length]))
    with pytest.raises(SafetyError):
        SafetyClient(transport, timeout=0.01).page(0)


def test_unknown_flags_are_rejected():
    transport = MockTransport()
    transport.queue_responses(*responses(flags=8))
    with pytest.raises(SafetyError):
        SafetyClient(transport).status()


def test_invalid_page_is_refused_locally():
    with pytest.raises(ValueError):
        SafetyClient(MockTransport()).page(3)
