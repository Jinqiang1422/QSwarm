"""Waiting for QGC must separate a dead simulator instance from a QGC defect.

Every downgrade guard in _minority_instance_stall gets a counter-example here: a
discriminator with untested guards is not a discriminator, it is an excuse machine.
"""

import unittest

from strict_regression import _minority_instance_stall, _telemetry_complete


def vehicle(vid, *, connected=True, telemetry=True):
    v = {"id": vid, "initial_connect_complete": connected}
    if telemetry:
        v.update({"lat": 47.39, "lon": 8.54, "alt_rel_m": 0.5})
    else:
        v.update({"lat": 47.39, "lon": 8.54, "alt_rel_m": None})
    return v


def status(vehicles, *, ok=True):
    return {"ok": ok, "vehicle_count": len(vehicles), "vehicles": vehicles}


class TelemetryCompleteTests(unittest.TestCase):
    def test_none_altitude_is_incomplete(self):
        self.assertFalse(_telemetry_complete(vehicle(7, telemetry=False)))

    def test_nan_is_incomplete(self):
        v = vehicle(7)
        v["alt_rel_m"] = float("nan")
        self.assertFalse(_telemetry_complete(v))

    def test_finite_triple_is_complete(self):
        self.assertTrue(_telemetry_complete(vehicle(7)))


class MinorityStallTests(unittest.TestCase):
    def test_single_stalled_instance_in_eight_is_a_block(self):
        """The observed scenario-01 failure: vehicle 7 never reported altitude."""
        fleet = [vehicle(i) for i in range(1, 9)]
        fleet[6] = vehicle(7, telemetry=False)
        stall = _minority_instance_stall(status(fleet), 8)
        self.assertIsNotNone(stall)
        self.assertEqual(stall["stalled_ids"], [7])
        self.assertEqual(stall["fleet_size"], 8)

    def test_whole_fleet_missing_telemetry_is_not_a_block(self):
        """If QGC's telemetry pipeline were broken, every vehicle would be missing."""
        fleet = [vehicle(i, telemetry=False) for i in range(1, 9)]
        self.assertIsNone(_minority_instance_stall(status(fleet), 8))

    def test_exactly_half_stalled_is_not_a_block(self):
        """Half is not a minority: there is no majority left to serve as the reference."""
        fleet = [vehicle(i) for i in range(1, 5)] + [
            vehicle(i, telemetry=False) for i in range(5, 9)
        ]
        self.assertIsNone(_minority_instance_stall(status(fleet), 8))

    def test_incomplete_handshake_is_not_a_block(self):
        """A vehicle QGC never finished connecting to is QGC's problem, not the sim's."""
        fleet = [vehicle(i) for i in range(1, 9)]
        fleet[6] = vehicle(7, connected=False, telemetry=False)
        self.assertIsNone(_minority_instance_stall(status(fleet), 8))

    def test_missing_vehicles_is_not_a_block(self):
        """Under-reporting the fleet size is a QGC connection defect and must stay FAIL."""
        fleet = [vehicle(i) for i in range(1, 8)]
        fleet[6] = vehicle(7, telemetry=False)
        self.assertIsNone(_minority_instance_stall(status(fleet), 8))

    def test_status_not_ok_is_not_a_block(self):
        fleet = [vehicle(i) for i in range(1, 9)]
        fleet[6] = vehicle(7, telemetry=False)
        self.assertIsNone(_minority_instance_stall(status(fleet, ok=False), 8))

    def test_single_vehicle_fleet_never_blocks(self):
        """One vehicle has no peers to be a minority of."""
        self.assertIsNone(_minority_instance_stall(status([vehicle(1, telemetry=False)]), 1))

    def test_two_vehicle_fleet_with_one_stalled_is_not_a_block(self):
        """One of two is exactly half, not a minority."""
        fleet = [vehicle(1), vehicle(2, telemetry=False)]
        self.assertIsNone(_minority_instance_stall(status(fleet), 2))

    def test_healthy_fleet_is_not_a_block(self):
        fleet = [vehicle(i) for i in range(1, 9)]
        self.assertIsNone(_minority_instance_stall(status(fleet), 8))

    def test_empty_status_is_not_a_block(self):
        self.assertIsNone(_minority_instance_stall({}, 8))


if __name__ == "__main__":
    unittest.main()
