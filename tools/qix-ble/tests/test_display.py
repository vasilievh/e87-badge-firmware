import struct
import pytest
from qix_ble.display import Card, DisplayClient, DisplayError, HELLO, CARD, CLEAR, STATUS, main
from qix_ble.frame import QixFrame


class DisplayTransport:
    """Emulates firmware: max_version=1 is the original Display v1 firmware."""

    def __init__(self, max_version=2, unicode=True):
        self.sent = []
        self.status = 0
        self.bad_sequence = False
        self.max_version, self.unicode = max_version, unicode

    def send_command(self, cmd, payload, **kwargs):
        self.sent.append((cmd, payload, kwargs))
        version, op, seq = payload[2], payload[3], payload[4:6]
        if self.bad_sequence: seq = b"\xff\xff"
        if version > self.max_version:  # old firmware: v1 header, status 2
            return QixFrame(0, 0x91, b"BD\x01" + bytes((op | 0x80,)) + seq + b"\x02\x00\x00")
        if op == HELLO:
            tail = (struct.pack("<HHBBB", 360, 360, 48, 96, 64) if version == 1 else
                    struct.pack("<BBBBH", 64, 192, 96, 1 if self.unicode else 0, 0xBEEF if self.unicode else 0))
        else:
            tail = b"\x01\x00"
        return QixFrame(0, 0x91, b"BD" + bytes((version, op | 0x80)) + seq + bytes((self.status,)) + tail)

    def wait_for_cmd(self, *args, **kwargs):
        raise DisplayError("no matching display acknowledgement")


def test_card_wire_layout():
    assert Card("Hi", "PC\nBLE", "ok", "010203", "040506", "070809", 42).encode().hex() == (
        "0102030405060708092a020602486950430a424c456f6b")


def test_utf8_wire_layout_counts_bytes():
    encoded = Card("Пр", "ß", "").encode((64, 192, 96), unicode=True)
    assert encoded[10:13] == bytes((4, 2, 0)) and encoded[13:] == "Прß".encode()


def test_largest_v1_frame_fits_one_244_byte_write():
    transport = DisplayTransport(max_version=1)
    DisplayClient(transport).show(Card("A"*48, "B"*96, "C"*64))
    cmd, payload, kwargs = transport.sent[-1]
    assert len(payload) == 227 and payload[2] == 1
    assert len(QixFrame(6, cmd, payload).encode()) == 233
    assert kwargs["response"] is True


def test_largest_v2_frame_fits_firmware_buffers():
    transport = DisplayTransport()
    DisplayClient(transport).show(Card("Я"*32, "ж"*96, "ё"*48))
    cmd, payload, _ = transport.sent[-1]
    assert payload[2] == 2 and len(payload) == 371
    assert len(QixFrame(6, cmd, payload).encode()) <= 512  # one WinRT write, < 1100-byte RX buffer


@pytest.mark.parametrize("card", [Card(title="A"*65), Card(body="B"*193), Card(footer="C"*97),
    Card(title="Я"*33), Card(title="\n"), Card(body="\x00"), Card(footer="\x7f"), Card(title="😀"),
    Card(title="\ud800"), Card(progress=-1), Card(progress=101), Card(progress=True),
    Card(background="1000000"), Card(body=None)])
def test_invalid_input_never_touches_transport(card):
    transport = DisplayTransport()
    with pytest.raises(ValueError): DisplayClient(transport).show(card)
    assert not transport.sent


def test_v2_badge_with_font_pack_accepts_unicode():
    transport = DisplayTransport()
    client = DisplayClient(transport)
    assert client.show(Card("Привет", "Größe: 5 °C", "«ёлка»")) == 1
    hello, card = transport.sent
    assert hello[1][2] == 2 and card[1][2] == 2 and card[1][3] == CARD
    assert client.capabilities["unicode"] and client.capabilities["font_pack"] == "BEEF"


@pytest.mark.parametrize("max_version,unicode", [(1, False), (2, False)])
def test_ascii_only_badges_reject_unicode_before_sending_card(max_version, unicode):
    transport = DisplayTransport(max_version, unicode)
    client = DisplayClient(transport)
    with pytest.raises(ValueError, match="ASCII"):
        client.show(Card("Привет"))
    assert [p[3] for _, p, _ in transport.sent if p[3] != HELLO] == []
    assert client.show(Card("Hello")) == 1


def test_old_firmware_falls_back_to_v1_once():
    transport = DisplayTransport(max_version=1)
    client = DisplayClient(transport)
    assert client.show(Card("Hello")) == 1
    assert client.clear() == 1
    assert [(p[2], p[3]) for _, p, _ in transport.sent] == [(2, HELLO), (1, HELLO), (1, CARD), (1, CLEAR)]
    assert client.capabilities["version"] == 1 and client.capabilities["charset"] == "ASCII"


def test_v1_limits_apply_after_fallback():
    transport = DisplayTransport(max_version=1)
    with pytest.raises(ValueError, match="limit is 48"):
        DisplayClient(transport).show(Card("A"*49))
    assert [p[3] for _, p, _ in transport.sent] == [HELLO, HELLO]


def test_hello_then_card_no_vendor_or_flash_commands():
    transport = DisplayTransport()
    client = DisplayClient(transport)
    assert client.show(Card("Hello")) == 1
    assert client.clear() == 1
    assert [p[3] for _, p, _ in transport.sent] == [HELLO, CARD, CLEAR]
    assert all(cmd == 0x90 for cmd, _, _ in transport.sent)


def test_stale_acknowledgement_is_not_success():
    transport = DisplayTransport(); transport.bad_sequence = True
    with pytest.raises(DisplayError, match="matching"):
        DisplayClient(transport).hello()


@pytest.mark.parametrize("status", [1, 3, 4, 5, 6])
def test_device_error_is_propagated_without_fallback(status):
    transport = DisplayTransport(); transport.status = status
    with pytest.raises(DisplayError) as error: DisplayClient(transport).hello()
    assert error.value.status == status and len(transport.sent) == 1


def test_sequence_wraps_as_u16():
    transport = DisplayTransport(); client = DisplayClient(transport)
    client.sequence = 65535
    client.hello()
    assert transport.sent[0][1][4:6] == b"\0\0"


def test_cli_validates_before_connecting(capsys):
    assert main(["00:11:22:33:44:55", "card", "--title", "😀"]) == 1
    assert "unsupported character" in capsys.readouterr().err
