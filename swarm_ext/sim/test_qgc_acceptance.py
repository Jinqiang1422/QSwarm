#!/usr/bin/env python3
"""Unit tests for QGC acceptance geometry and COP truth matching."""

import unittest

from qgc_acceptance import (
    cop_truth_metrics,
    destination,
    distance_m,
    formation_geometry_metrics,
)
from run_scenario import QGCFleet


class FakeArmClient:
    def __init__(self, ready_after: int, accepted: bool = True):
        self.ready_after = ready_after
        self.accepted = accepted
        self.arm_requests = 0

    def request(self, command: str) -> dict:
        if command != "arm_all":
            raise AssertionError(f"unexpected command: {command}")
        self.arm_requests += 1
        return {"ok": self.accepted}

    def wait_status(self, predicate, _timeout_sec: float) -> dict:
        status = {
            "ok": True,
            "vehicles": [
                {
                    "id": 1,
                    "armed": self.arm_requests >= self.ready_after,
                    "initial_connect_complete": True,
                    "ready_to_fly_available": True,
                    "ready_to_fly": True,
                }
            ],
        }
        predicate(status)
        return status


class NullTelemetryClient:
    def request(self, command: str) -> dict:
        if command != "status":
            raise AssertionError(f"unexpected command: {command}")
        return {
            "ok": True,
            "vehicles": [
                {
                    "id": 1,
                    "lat": None,
                    "lon": None,
                    "alt_rel_m": None,
                    "alt_amsl_m": None,
                }
            ],
        }


class QGCAcceptanceMetricsTest(unittest.TestCase):
    def test_distance_and_destination_are_consistent(self):
        lat, lon = destination(39.9, 116.4, 37.0, 250.0)
        self.assertAlmostEqual(distance_m(39.9, 116.4, lat, lon), 250.0, delta=0.1)

    def test_wedge_formation_geometry(self):
        follower_2 = destination(39.9, 116.4, 150.0, 15.0)
        follower_3 = destination(39.9, 116.4, 210.0, 15.0)
        status = {
            "formation": {
                "active": True,
                "leader_id": 1,
                "spacing_m": 15.0,
                "vertical_step_m": 3.0,
            },
            "vehicles": [
                {
                    "id": 1,
                    "lat": 39.9,
                    "lon": 116.4,
                    "heading_deg": 0.0,
                    "alt_amsl_m": 100.0,
                },
                {
                    "id": 2,
                    "lat": follower_2[0],
                    "lon": follower_2[1],
                    "alt_amsl_m": 103.0,
                },
                {
                    "id": 3,
                    "lat": follower_3[0],
                    "lon": follower_3[1],
                    "alt_amsl_m": 103.0,
                },
            ],
        }
        metrics = formation_geometry_metrics(status)
        self.assertTrue(metrics["pass"])
        self.assertLess(metrics["max_horizontal_error_m"], 0.1)
        self.assertEqual(metrics["max_vertical_error_m"], 0.0)

    def test_cop_tracks_match_truth_by_numeric_category(self):
        scenario = {
            "truths": [
                {"category": "vehicle", "waypoints": [[0, 39.9, 116.4]]},
                {"category": "person", "waypoints": [[0, 39.901, 116.401]]},
            ]
        }
        tracks = [
            {"categoryId": 1, "lat": 39.90101, "lon": 116.40101},
            {"categoryId": 2, "lat": 39.90001, "lon": 116.40001},
        ]
        metrics = cop_truth_metrics(scenario, tracks)
        self.assertTrue(metrics["pass"])
        self.assertEqual(metrics["matched_truths"], 2)
        self.assertLess(metrics["max_error_m"], 2.0)

    def test_qgc_arm_retries_transient_preflight_rejection(self):
        client = FakeArmClient(ready_after=3)

        result = QGCFleet(client).arm_all(retry_timeout=1.0)

        self.assertTrue(result["pass"])
        self.assertEqual(client.arm_requests, 3)
        self.assertEqual(len(result["attempts"]), 3)

    def test_qgc_arm_does_not_retry_explicit_rejection(self):
        client = FakeArmClient(ready_after=3, accepted=False)

        result = QGCFleet(client).arm_all(retry_timeout=1.0)

        self.assertFalse(result["pass"])
        self.assertEqual(client.arm_requests, 1)

    def test_qgc_fleet_tolerates_initial_null_telemetry(self):
        fleet = QGCFleet(NullTelemetryClient())

        fleet.pump()

        self.assertEqual(fleet.online_ids(), [1])
        self.assertEqual(fleet.position, {})
        self.assertEqual(fleet.rel_alt, {})
        self.assertEqual(fleet.amsl_alt, {})


if __name__ == "__main__":
    unittest.main()
