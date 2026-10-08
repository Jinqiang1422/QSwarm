#!/usr/bin/env python3
"""Unit tests for QGC acceptance geometry and COP truth matching."""

import os
import tempfile
import unittest
from pathlib import Path

from qgc_acceptance import (
    acceptance_secret_file,
    cop_truth_metrics,
    destination,
    distance_m,
    formation_geometry_metrics,
    formation_sync_metrics,
    read_acceptance_token,
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

    def test_wedge_formation_geometry_uses_reported_slots_and_heading(self):
        # Vehicle 3 holds slot 1 (left) and vehicle 2 slot 2 (right); the formation faces east while the
        # leader's own heading is irrelevant.
        left = destination(39.9, 116.4, 90.0 + 150.0, 15.0)
        right = destination(39.9, 116.4, 90.0 + 210.0, 15.0)
        status = {
            "formation": {
                "active": True,
                "leader_id": 1,
                "spacing_m": 15.0,
                "vertical_step_m": 3.0,
                "heading_deg": 90.0,
                "slots": {"2": 2, "3": 1},
            },
            "vehicles": [
                {"id": 1, "lat": 39.9, "lon": 116.4, "heading_deg": 10.0, "alt_amsl_m": 100.0},
                {"id": 2, "lat": right[0], "lon": right[1], "alt_amsl_m": 103.0},
                {"id": 3, "lat": left[0], "lon": left[1], "alt_amsl_m": 103.0},
            ],
        }
        metrics = formation_geometry_metrics(status)
        self.assertTrue(metrics["pass"])
        self.assertLess(metrics["max_horizontal_error_m"], 0.1)

    def test_formation_sync_metrics(self):
        a_start = (39.9, 116.4)
        b_start = destination(*a_start, 0.0, 20.0)
        a_end = destination(*a_start, 90.0, 100.0)
        b_end = destination(*b_start, 90.0, 100.0)
        members = [
            {
                "id": 2,
                "speed_mps": 5.0,
                "start_lat": a_start[0],
                "start_lon": a_start[1],
                "end_lat": a_end[0],
                "end_lon": a_end[1],
                "end_alt_m": 100.0,
            },
            {
                "id": 3,
                "speed_mps": 5.0,
                "start_lat": b_start[0],
                "start_lon": b_start[1],
                "end_lat": b_end[0],
                "end_lon": b_end[1],
                "end_alt_m": 100.0,
            },
            # A holding member (zero speed) is excluded from timing and deviation.
            {
                "id": 1,
                "speed_mps": 0.0,
                "start_lat": 39.0,
                "start_lon": 116.0,
                "end_lat": 39.0,
                "end_lon": 116.0,
                "end_alt_m": 100.0,
            },
        ]

        def vehicles(a_east: float, b_east: float) -> list:
            a = destination(*a_start, 90.0, a_east)
            b = destination(*b_start, 90.0, b_east)
            return [
                {"id": 2, "lat": a[0], "lon": a[1], "alt_amsl_m": 100.0},
                {"id": 3, "lat": b[0], "lon": b[1], "alt_amsl_m": 100.0},
            ]

        # Vehicle 3 starts a full second later than vehicle 2, so start_spread_s must be non-zero.
        samples = [
            (0.0, vehicles(0.0, 0.0)),
            (1.0, vehicles(10.0, 0.0)),
            (2.0, vehicles(20.0, 5.0)),
            (10.0, vehicles(100.0, 95.0)),
        ]
        tail = [(11.0, vehicles(100.0, 100.0))]
        metrics = formation_sync_metrics(members, samples, tail, member_ids={1, 2, 3})
        self.assertEqual(metrics["moving_members"], 2)
        self.assertEqual(metrics["start_spread_s"], 1.0)
        self.assertEqual(metrics["missing_start"], [])
        self.assertEqual(metrics["arrival_spread_s"], 1.0)
        self.assertEqual(metrics["missing_arrival"], [])
        self.assertAlmostEqual(metrics["max_deviation_m"], 7.5, delta=0.05)
        self.assertAlmostEqual(metrics["min_separation_m"], 20.0, delta=0.05)
        self.assertEqual(metrics["closest_approach_violations"], [])

    def test_formation_sync_metrics_detects_closest_approach_violation(self):
        # Two members start 2.5 m apart and converge onto the same end point: the pair closes well
        # past min(3.0, initial) - 0.5 m, which must be reported even though neither ever drops below
        # the absolute 3 m floor check used for S1-S3/S5 (that check is scenario-level, not here).
        base = (39.9, 116.4)
        a_end = destination(*base, 0.0, 100.0)
        b_start = destination(*base, 90.0, 2.5)
        members = [
            {
                "id": 10,
                "speed_mps": 5.0,
                "start_lat": base[0],
                "start_lon": base[1],
                "end_lat": a_end[0],
                "end_lon": a_end[1],
                "end_alt_m": 100.0,
            },
            {
                "id": 11,
                "speed_mps": 5.0,
                "start_lat": b_start[0],
                "start_lon": b_start[1],
                "end_lat": a_end[0],
                "end_lon": a_end[1],
                "end_alt_m": 100.0,
            },
        ]

        def lerp(p, q, s):
            return (p[0] + (q[0] - p[0]) * s, p[1] + (q[1] - p[1]) * s)

        def sample(s):
            a = lerp(base, a_end, s)
            b = lerp(b_start, a_end, s)
            return [
                {"id": 10, "lat": a[0], "lon": a[1], "alt_amsl_m": 100.0},
                {"id": 11, "lat": b[0], "lon": b[1], "alt_amsl_m": 100.0},
            ]

        samples = [(0.0, sample(0.0)), (1.0, sample(0.5)), (2.0, sample(1.0))]
        metrics = formation_sync_metrics(members, samples, member_ids={10, 11})
        self.assertAlmostEqual(metrics["min_separation_m"], 0.0, delta=0.05)
        self.assertEqual(len(metrics["closest_approach_violations"]), 1)
        violation = metrics["closest_approach_violations"][0]
        self.assertEqual(violation["pair"], [10, 11])
        self.assertAlmostEqual(violation["min_separation_m"], 0.0, delta=0.05)
        self.assertAlmostEqual(violation["initial_separation_m"], 2.5, delta=0.05)

    def test_formation_sync_metrics_separation_only_considers_formation_members(self):
        # A and B fly a parallel 100 m leg 10 m apart (safe). A non-member vehicle (99, e.g. another
        # aircraft) passes within 0.3 m of A partway through -- without member_ids that would look like
        # a near-collision. B also drops out for one sample (lat/lon/alt_amsl_m reported as None), which
        # must be skipped rather than raise.
        base = (39.9, 116.4)
        a_end = destination(*base, 0.0, 100.0)
        b_start = destination(*base, 90.0, 10.0)
        b_end = destination(*a_end, 90.0, 10.0)
        members = [
            {
                "id": 10,
                "speed_mps": 5.0,
                "start_lat": base[0],
                "start_lon": base[1],
                "end_lat": a_end[0],
                "end_lon": a_end[1],
                "end_alt_m": 100.0,
            },
            {
                "id": 11,
                "speed_mps": 5.0,
                "start_lat": b_start[0],
                "start_lon": b_start[1],
                "end_lat": b_end[0],
                "end_lon": b_end[1],
                "end_alt_m": 100.0,
            },
        ]

        def lerp(p, q, s):
            return (p[0] + (q[0] - p[0]) * s, p[1] + (q[1] - p[1]) * s)

        def sample(s, intrude=False, missing_b=False):
            a = lerp(base, a_end, s)
            vehicles = [{"id": 10, "lat": a[0], "lon": a[1], "alt_amsl_m": 100.0}]
            if missing_b:
                vehicles.append({"id": 11, "lat": None, "lon": None, "alt_amsl_m": None})
            else:
                b = lerp(b_start, b_end, s)
                vehicles.append({"id": 11, "lat": b[0], "lon": b[1], "alt_amsl_m": 100.0})
            if intrude:
                near = destination(a[0], a[1], 0.0, 0.3)
                vehicles.append({"id": 99, "lat": near[0], "lon": near[1], "alt_amsl_m": 100.0})
            return vehicles

        samples = [
            (0.0, sample(0.0)),
            (1.0, sample(0.5, intrude=True, missing_b=True)),
            (2.0, sample(1.0)),
        ]
        filtered = formation_sync_metrics(members, samples, member_ids={10, 11})
        self.assertAlmostEqual(filtered["min_separation_m"], 10.0, delta=0.05)
        self.assertEqual(filtered["closest_approach_violations"], [])

        # Without member_ids, the non-member intruder corrupts the separation check.
        unfiltered = formation_sync_metrics(members, samples)
        self.assertAlmostEqual(unfiltered["min_separation_m"], 0.3, delta=0.05)

    def test_formation_sync_metrics_reports_missing_ids_when_member_never_appears(self):
        # Member 3 is commanded (moving) but never reports telemetry again (e.g. it dropped off comms);
        # the spreads must be None and the missing id must be named rather than silently ignored.
        base = (39.9, 116.4)
        a_end = destination(*base, 90.0, 50.0)
        members = [
            {
                "id": 2,
                "speed_mps": 5.0,
                "start_lat": base[0],
                "start_lon": base[1],
                "end_lat": a_end[0],
                "end_lon": a_end[1],
                "end_alt_m": 100.0,
            },
            {
                "id": 3,
                "speed_mps": 5.0,
                "start_lat": 39.0,
                "start_lon": 116.0,
                "end_lat": 39.001,
                "end_lon": 116.001,
                "end_alt_m": 100.0,
            },
        ]

        def sample(east):
            a = destination(*base, 90.0, east)
            return [{"id": 2, "lat": a[0], "lon": a[1], "alt_amsl_m": 100.0}]

        samples = [(0.0, sample(0.0)), (1.0, sample(10.0)), (5.0, sample(50.0))]
        metrics = formation_sync_metrics(members, samples, member_ids={2, 3})
        self.assertEqual(metrics["moving_members"], 2)
        self.assertIsNone(metrics["start_spread_s"])
        self.assertEqual(metrics["missing_start"], [3])
        self.assertIsNone(metrics["arrival_spread_s"])
        self.assertEqual(metrics["missing_arrival"], [3])
        self.assertIsNone(metrics["min_separation_m"])

    def test_formation_sync_metrics_tail_excludes_separation_and_deviation(self):
        # The pair stays a safe 20 m apart for the whole leg, then (only in the tail window, after the
        # leg already finished) drifts to 0.1 m apart. That near-collision must not surface anywhere.
        base = (39.9, 116.4)
        a_end = destination(*base, 90.0, 50.0)
        b_start = destination(*base, 0.0, 20.0)
        b_end = destination(*a_end, 0.0, 20.0)
        members = [
            {
                "id": 2,
                "speed_mps": 5.0,
                "start_lat": base[0],
                "start_lon": base[1],
                "end_lat": a_end[0],
                "end_lon": a_end[1],
                "end_alt_m": 100.0,
            },
            {
                "id": 3,
                "speed_mps": 5.0,
                "start_lat": b_start[0],
                "start_lon": b_start[1],
                "end_lat": b_end[0],
                "end_lon": b_end[1],
                "end_alt_m": 100.0,
            },
        ]

        def lerp(p, q, s):
            return (p[0] + (q[0] - p[0]) * s, p[1] + (q[1] - p[1]) * s)

        def sample(s):
            a = lerp(base, a_end, s)
            b = lerp(b_start, b_end, s)
            return [
                {"id": 2, "lat": a[0], "lon": a[1], "alt_amsl_m": 100.0},
                {"id": 3, "lat": b[0], "lon": b[1], "alt_amsl_m": 100.0},
            ]

        samples = [(0.0, sample(0.0)), (1.0, sample(0.5)), (2.0, sample(1.0))]
        close = destination(*a_end, 0.0, 0.1)
        tail = [
            (
                3.0,
                [
                    {"id": 2, "lat": a_end[0], "lon": a_end[1], "alt_amsl_m": 100.0},
                    {"id": 3, "lat": close[0], "lon": close[1], "alt_amsl_m": 100.0},
                ],
            )
        ]
        metrics = formation_sync_metrics(members, samples, tail, member_ids={2, 3})
        self.assertAlmostEqual(metrics["min_separation_m"], 20.0, delta=0.05)
        self.assertEqual(metrics["closest_approach_violations"], [])
        self.assertAlmostEqual(metrics["max_deviation_m"], 0.0, delta=0.05)

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

    def test_cop_truth_metrics_interpolates_active_truths(self):
        scenario = {
            "truths": [
                {"category": "vehicle", "waypoints": [[0, 0.0, 0.0], [10, 0.0, 0.001]]},
                {"category": "person", "waypoints": [[6, 1.0, 1.0], [10, 1.0, 1.001]]},
            ]
        }
        tracks = [{"categoryId": 2, "lat": 0.0, "lon": 0.0005}]

        metrics = cop_truth_metrics(scenario, tracks, at_time_sec=5.0)

        self.assertTrue(metrics["pass"])
        self.assertEqual(metrics["truth_count"], 1)
        self.assertEqual(metrics["errors_by_truth_m"], {"0": 0.0})

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


class AcceptanceSecretTest(unittest.TestCase):
    def _write_secret(self, directory: str, content: bytes, mode: int = 0o600) -> Path:
        path = Path(directory) / "token"
        path.write_bytes(content)
        path.chmod(mode)
        return path

    def test_reads_owner_only_single_line_secret(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self._write_secret(directory, b"0123456789abcdef\n")

            self.assertEqual(read_acceptance_token(path), "0123456789abcdef")

    def test_rejects_invalid_secret_files(self):
        cases = (
            (b"too-short", 0o600),
            (b"0123456789abcdef\nsecond-line", 0o600),
            (b"0123456789abcde\x00f", 0o600),
            (b"\xff" * 16, 0o600),
            (b"0123456789abcdef", 0o640),
        )
        for content, mode in cases:
            with self.subTest(content=content, mode=oct(mode)):  # noqa: SIM117 -- Python 3.8
                with tempfile.TemporaryDirectory() as directory:
                    path = self._write_secret(directory, content, mode)

                    with self.assertRaises(ValueError):
                        read_acceptance_token(path)

    @unittest.skipUnless(hasattr(os, "O_NOFOLLOW"), "platform does not support no-follow opens")
    def test_rejects_symbolic_link(self):
        with tempfile.TemporaryDirectory() as directory:
            target = self._write_secret(directory, b"0123456789abcdef")
            link = Path(directory) / "token-link"
            link.symlink_to(target)

            with self.assertRaises(ValueError):
                read_acceptance_token(link)
            with self.assertRaises(ValueError), acceptance_secret_file(link):
                pass

    def test_generated_secret_is_private_and_removed(self):
        generated_path = None
        with acceptance_secret_file() as path:
            generated_path = path
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertGreaterEqual(len(read_acceptance_token(path).encode("utf-8")), 16)

        self.assertIsNotNone(generated_path)
        self.assertFalse(generated_path.exists())


if __name__ == "__main__":
    unittest.main()
