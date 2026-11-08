import struct
import pytest
from qix_ble.display import Card, DisplayClient, DisplayError, HELLO, CARD, CLEAR, STATUS
from qix_ble.frame import QixFrame


class DisplayTransport:
    def __init__(self):
        self.sent = []
        self.status = 0
        self.bad_sequence = False

    def send_command(self, cmd, payload, **kwargs):
        self.sent.append((cmd, payload, kwargs))
        op, seq = payload[3], payload[4:6]
        if self.bad_sequence: seq = b"\xff\xff"
        tail = struct.pack("<HHBBB", 360, 360, 48, 96, 64) if op == HELLO else b"\x01\x00"
        return QixFrame(0, 0x91, b"BD\x01" + bytes((op | 0x80,)) + seq + bytes((self.status,)) + tail)

    def wait_for_cmd(self, *args, **kwargs):
        raise DisplayError("no matching display acknowledgement")


def test_card_wire_layout():
    assert Card("Hi", "PC\nBLE", "ok", "010203", "040506", "070809", 42).encode().hex() == (
        "0102030405060708092a020602486950430a424c456f6b")


def test_largest_frame_fits_one_244_byte_write():
    transport = DisplayTransport()
    DisplayClient(transport).show(Card("A"*48, "B"*96, "C"*64))
    cmd, payload, kwargs = transport.sent[-1]
    assert len(payload) == 227
    assert len(QixFrame(6, cmd, payload).encode()) == 233
    assert kwargs["response"] is True


@pytest.mark.parametrize("card", [Card(title="A"*49), Card(body="B"*97), Card(footer="C"*65),
    Card(title="\n"), Card(body="\x00"), Card(title="ä"), Card(title="Привет"),
    Card(progress=-1), Card(progress=101), Card(progress=True), Card(background="1000000"), Card(body=None)])
def test_invalid_input_never_touches_transport(card):
    transport = DisplayTransport()
    with pytest.raises(ValueError): DisplayClient(transport).show(card)
    assert not transport.sent


def test_hello_then_card_no_vendor_or_flash_commands():
    transport = DisplayTransport()
    client = DisplayClient(transport)
    assert client.show(Card("Hello")) == 1
    assert client.clear() == 1
    assert [p[3] for _, p, _ in transport.sent] == [HELLO, CARD, CLEAR]
    assert all(cmd == 0x90 for cmd, _, _ in transport.sent)
    assert client.capabilities["charset"] == "ASCII"


def test_stale_acknowledgement_is_not_success():
    transport = DisplayTransport(); transport.bad_sequence = True
    with pytest.raises(DisplayError, match="matching"):
        DisplayClient(transport).hello()


@pytest.mark.parametrize("status", range(1, 7))
def test_device_error_is_propagated(status):
    transport = DisplayTransport(); transport.status = status
    with pytest.raises(DisplayError): DisplayClient(transport).hello()


def test_sequence_wraps_as_u16():
    transport = DisplayTransport(); client = DisplayClient(transport)
    client.sequence = 65535
    client.hello()
    assert transport.sent[0][1][4:6] == b"\0\0"
