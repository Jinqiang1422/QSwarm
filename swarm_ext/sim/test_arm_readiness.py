"""Arming must separate a dead simulator sensor bridge from a real arming defect."""

import unittest

from run_scenario import CANDIDATE_SENSOR_ENUMS, Fleet


class FakeMavlink:
    """Only the enums Fleet touches; values mirror the MAVLink SYS_STATUS bitmask."""

    MAV_SYS_STATUS_SENSOR_3D_GYRO = 1 << 0
    MAV_SYS_STATUS_SENSOR_3D_ACCEL = 1 << 1
    MAV_SYS_STATUS_SENSOR_3D_MAG = 1 << 2
    MAV_SYS_STATUS_SENSOR_ABSOLUTE_PRESSURE = 1 << 3
    MAV_SYS_STATUS_PREARM_CHECK = 1 << 16
    MAV_CMD_COMPONENT_ARM_DISARM = 400
    MAV_RESULT_ACCEPTED = 0
    MAV_RESULT_TEMPORARILY_REJECTED = 1


GYRO = FakeMavlink.MAV_SYS_STATUS_SENSOR_3D_GYRO
ACCEL = FakeMavlink.MAV_SYS_STATUS_SENSOR_3D_ACCEL
MAG = FakeMavlink.MAV_SYS_STATUS_SENSOR_3D_MAG
BARO = FakeMavlink.MAV_SYS_STATUS_SENSOR_ABSOLUTE_PRESSURE
ALL_SENSORS = GYRO | ACCEL | MAG | BARO
# What PX4 actually reports on the SITL fleet used by scenarios 03/04/05: no accel bit.
PX4_SITL_PRESENT = GYRO | MAG | BARO


class FakeFleet(Fleet):
    """Fleet with the MAVLink connection replaced by canned SYS_STATUS state."""

    def __init__(self, sys_status, online=None):
        # Deliberately skip Fleet.__init__ so no pymavlink import or socket is needed.
        self.mavutil = type("_Mavutil", (), {"mavlink": FakeMavlink})
        self.sys_status = dict(sys_status)
        self._online = sorted(online if online is not None else sys_status)
        self.broadcasts = []

    def online_ids(self):
        return list(self._online)

    def pump(self, duration=0.0):
        return None

    def broadcast(self, command, **kwargs):
        self.broadcasts.append((command, kwargs))
        targets = self.online_ids()
        return {
            "targets": targets,
            "ack_results": dict.fromkeys(targets, FakeMavlink.MAV_RESULT_ACCEPTED),
            "ack_history": {sid: [FakeMavlink.MAV_RESULT_ACCEPTED] for sid in targets},
            "missing_ack": [],
            "missing_position": [],
            "rejected": {},
            "pass": True,
        }


def _status(present, health=None):
    return {"present": present, "enabled": present, "health": present if health is None else health}


def _fleet(*present_values, health=None):
    return FakeFleet(
        {index + 1: _status(value, health) for index, value in enumerate(present_values)}
    )


class ArmReadinessTest(unittest.TestCase):
    def test_uniform_fleet_proceeds_to_broadcast(self):
        fleet = _fleet(PX4_SITL_PRESENT, PX4_SITL_PRESENT, PX4_SITL_PRESENT)
        result = fleet.arm_all(retry_timeout=1.0)
        self.assertTrue(result["pass"])
        self.assertNotIn("environment_block", result)
        self.assertTrue(result["sensor_readiness"]["pass"])
        self.assertTrue(fleet.broadcasts, "传感器与同伴一致时必须实际下发 ARM")

    def test_bit_no_peer_reports_is_not_required(self):
        # Regression for the real SITL fleet: PX4 never sets the 3D_ACCEL present bit,
        # so a fixed required-bit list blocked eight healthy vehicles.
        fleet = _fleet(PX4_SITL_PRESENT, PX4_SITL_PRESENT, PX4_SITL_PRESENT)
        readiness = fleet.wait_sensors_present(1.0)
        self.assertTrue(readiness["pass"])
        self.assertNotIn(
            CANDIDATE_SENSOR_ENUMS["MAV_SYS_STATUS_SENSOR_3D_ACCEL"],
            readiness["expected_sensors"],
        )

    def test_outlier_missing_peer_sensors_blocks_without_sending_arm(self):
        fleet = _fleet(PX4_SITL_PRESENT, PX4_SITL_PRESENT, BARO)
        result = fleet.arm_all(retry_timeout=1.0)
        self.assertFalse(result["pass"])
        self.assertTrue(result["environment_block"])
        self.assertEqual(result["environment_block_reason"], "preflight_sensors_missing")
        self.assertEqual(result["sensor_readiness"]["missing_vehicles"], [3])
        self.assertEqual(
            result["sensor_readiness"]["per_vehicle"][3]["missing"],
            sorted(
                [
                    CANDIDATE_SENSOR_ENUMS["MAV_SYS_STATUS_SENSOR_3D_GYRO"],
                    CANDIDATE_SENSOR_ENUMS["MAV_SYS_STATUS_SENSOR_3D_MAG"],
                ]
            ),
        )
        self.assertEqual(fleet.broadcasts, [], "环境阻塞时不得下发 ARM")

    def test_single_vehicle_fleet_never_blocks(self):
        # With no peer there is no reference; fail open rather than invent one.
        fleet = _fleet(BARO)
        result = fleet.arm_all(retry_timeout=1.0)
        self.assertNotIn("environment_block", result)
        self.assertFalse(result["sensor_readiness"]["quorum"])
        self.assertTrue(fleet.broadcasts)

    def test_whole_fleet_degraded_equally_is_not_a_block(self):
        # Every peer agrees, so nothing stands out; the ACK decides instead.
        fleet = _fleet(BARO, BARO, BARO)
        result = fleet.arm_all(retry_timeout=1.0)
        self.assertNotIn("environment_block", result)
        self.assertTrue(fleet.broadcasts)

    def test_unhealthy_but_present_sensors_are_not_an_environment_block(self):
        # A failed preflight is a real failure: keep the original path and let the
        # ACK decide, never downgrade it to an environment block.
        fleet = _fleet(ALL_SENSORS, ALL_SENSORS, ALL_SENSORS, health=0)
        result = fleet.arm_all(retry_timeout=1.0)
        self.assertNotIn("environment_block", result)
        self.assertTrue(fleet.broadcasts, "present 齐全时必须照常下发 ARM 并由 ACK 判定")

    def test_prearm_check_bit_is_not_consulted(self):
        # Gating on the prearm bit would mean 'arming always succeeds', which would
        # excuse every real arming defect.
        fleet = _fleet(ALL_SENSORS, ALL_SENSORS, ALL_SENSORS)
        readiness = fleet.wait_sensors_present(1.0)
        self.assertTrue(readiness["pass"])

    def test_no_sys_status_evidence_never_blocks(self):
        fleet = FakeFleet({}, online=(1, 2, 3))
        result = fleet.arm_all(retry_timeout=1.0)
        self.assertNotIn("environment_block", result)
        self.assertFalse(result["sensor_readiness"]["evidence"])
        self.assertTrue(fleet.broadcasts, "没有 SYS_STATUS 证据时行为必须与改动前一致")

    def test_partial_evidence_never_blocks(self):
        # One vehicle reports SYS_STATUS, the others none at all: evidence is
        # incomplete, so no block.
        fleet = FakeFleet({1: _status(PX4_SITL_PRESENT)}, online=(1, 2, 3))
        result = fleet.arm_all(retry_timeout=1.0)
        self.assertNotIn("environment_block", result)
        self.assertFalse(result["sensor_readiness"]["evidence"])
        self.assertTrue(fleet.broadcasts)


if __name__ == "__main__":
    unittest.main()
