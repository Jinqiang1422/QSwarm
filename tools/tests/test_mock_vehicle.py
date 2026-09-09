"""Exercise the simulator with the real MAVLink encoder, without network I/O."""

from io import BytesIO
from unittest.mock import MagicMock

import pytest
from simulation.mock_vehicle import MockVehicle, mavlink


def test_armed_telemetry_encodes_fractional_battery() -> None:
    vehicle = MockVehicle()
    stream = BytesIO()
    connection = MagicMock()
    connection.mav = mavlink.MAVLink(stream)
    vehicle.connection = connection
    vehicle.state.armed = True
    vehicle.update_simulation(0.1)
    assert 99 < vehicle.state.battery_remaining < 100

    vehicle.send_sys_status()
    vehicle.send_battery_status()

    messages = mavlink.MAVLink(None).parse_buffer(stream.getvalue())
    assert messages is not None
    assert [m.get_type() for m in messages] == ["SYS_STATUS", "BATTERY_STATUS"]
    assert isinstance(messages[0], mavlink.MAVLink_sys_status_message)
    assert isinstance(messages[1], mavlink.MAVLink_battery_status_message)
    assert messages[0].battery_remaining == messages[1].battery_remaining == 99


@pytest.mark.parametrize("rate", [1, 10, 50])
def test_battery_drain_depends_on_elapsed_time(rate: int) -> None:
    vehicle = MockVehicle()
    vehicle.state.armed = True
    for _ in range(rate):
        vehicle.update_simulation(1.0 / rate)
    assert vehicle.state.battery_remaining == pytest.approx(99.9)


@pytest.mark.parametrize("method", ["send_heartbeat", "handle_commands", "run"])
def test_disconnected_simulator_reports_error(method: str) -> None:
    vehicle = MockVehicle()
    with pytest.raises(RuntimeError, match="Connect the mock vehicle"):
        getattr(vehicle, method)()
    assert not vehicle.running


@pytest.mark.parametrize("rate", [0, -1, float("inf"), float("nan")])
def test_invalid_telemetry_rate(rate: float) -> None:
    vehicle = MockVehicle()
    with pytest.raises(ValueError, match="Telemetry rate"):
        vehicle.run(rate)
    assert not vehicle.running


@pytest.mark.parametrize("dt", [-1, float("inf"), float("nan")])
def test_invalid_timestep_preserves_state(dt: float) -> None:
    vehicle = MockVehicle()
    vehicle.state.armed = True
    with pytest.raises(ValueError, match="timestep"):
        vehicle.update_simulation(dt)
    assert vehicle.state.battery_remaining == 100.0
