from types import SimpleNamespace
import pytest
from qix_ble.frame import QixFrame
from qix_ble.transport import QixTransport


@pytest.mark.parametrize("command,expected", [(0xC0, 30.0), (0xC2, 5.0), (0x90, 5.0)])
def test_staging_erase_gets_time_before_att_ack(monkeypatch, command, expected):
    observed = []

    async def write(*args, **kwargs):
        pass

    def submit(coroutine, loop):
        coroutine.close()
        return SimpleNamespace(result=lambda timeout: observed.append(timeout))

    monkeypatch.setattr("qix_ble.transport.asyncio.run_coroutine_threadsafe", submit)
    transport = QixTransport("00:11:22:33:44:55")
    transport._client = SimpleNamespace(is_connected=True, write_gatt_char=write)
    transport.send_raw(QixFrame(flags=0, cmd=command, payload=b""), response=True)
    assert observed == [expected]
