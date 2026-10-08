#!/usr/bin/env python3
"""Drive formation_sync_check.run()/observe() with a fake acceptance client.

Guards the SITL driver's own judgement calls -- the things formation_sync_metrics cannot see by
itself: a command that claims ok but never actually started a transit, a transit that never finishes
before the timeout, and the "start" scenario's violations-only separation rule (2026-10-05
synchronized-legs design, section 4).
"""

import argparse
import unittest

import formation_sync_check as fsc
from qgc_acceptance import QGCAcceptanceError, destination


class FakeAcceptanceClient:
    """Replays canned 'status' responses in call order (repeating the last); fixed/callable responses
    for every other command. Mirrors the FakeArmClient/NullTelemetryClient style used elsewhere in this
    test suite, generalized to the handful of commands formation_sync_check issues. A 'status' entry
    that is a QGCAcceptanceError instance is raised instead of returned, so tests can simulate transient
    connection failures."""

    def __init__(self, statuses, responses=None):
        self._statuses = list(statuses)
        self._responses = dict(responses or {})
        self.calls = []

    def request(self, command, **kwargs):
        self.calls.append((command, kwargs))
        if command == "status":
            if not self._statuses:
                raise AssertionError("status sequence exhausted")
            entry = self._statuses.pop(0) if len(self._statuses) > 1 else self._statuses[0]
            if isinstance(entry, QGCAcceptanceError):
                raise entry
            return entry
        if command in self._responses:
            response = self._responses[command]
            return response(kwargs) if callable(response) else response
        raise AssertionError(f"unexpected command: {command}")


def _vehicle(vehicle_id, lat, lon, airborne=True, automation_allowed=True, **extra):
    vehicle = {
        "id": vehicle_id,
        "lat": lat,
        "lon": lon,
        "alt_amsl_m": 100.0,
        "alt_rel_m": 20.0 if airborne else 0.0,
        "armed": airborne,
        "automation_allowed": automation_allowed,
    }
    vehicle.update(extra)
    return vehicle


def _status(formation_active, vehicles, leader_id=1, slots=None, transit=None):
    slots = {} if slots is None else slots
    transit = {"active": False, "last_result": {}} if transit is None else transit
    return {
        "ok": True,
        "vehicles": vehicles,
        "formation": {
            "active": formation_active,
            "leader_id": leader_id,
            "type": fsc.FORMATION_TYPE_WEDGE,
            "spacing_m": 15.0,
            "vertical_step_m": 3.0,
            "heading_deg": 0.0,
            "slots": slots,
            "transit": transit,
        },
    }


def _args(scenario, leader=1, spacing=15.0, vertical_step=3.0, timeout=1.0):
    return argparse.Namespace(
        host="127.0.0.1",
        port=0,
        leader=leader,
        spacing=spacing,
        vertical_step=vertical_step,
        timeout=timeout,
        scenario=scenario,
    )


class FormationSyncCheckFakeClientTest(unittest.TestCase):
    def setUp(self):
        # Real wall-clock polling, but fast enough that these tests stay well under a second each.
        self._orig_poll_s = fsc.POLL_S
        self._orig_tail_s = fsc.ARRIVAL_TAIL_S
        fsc.POLL_S = 0.01
        fsc.ARRIVAL_TAIL_S = 0.05

    def tearDown(self):
        fsc.POLL_S = self._orig_poll_s
        fsc.ARRIVAL_TAIL_S = self._orig_tail_s

    def test_run_fails_when_command_does_not_start_a_transit(self):
        # goto comes back ok=true, but the embedded status shows no active transit and an unchanged
        # last_result: the command was a silent no-op, and that must not look like a pass.
        leader = _vehicle(1, 39.9, 116.4)
        follower = _vehicle(2, 39.9001, 116.4)
        status = _status(True, [leader, follower], slots={"2": 1})
        goto_response = {"ok": True, "message": "", "status": status}
        client = FakeAcceptanceClient([status], responses={"goto": goto_response})

        report = fsc.run(client, _args("goto-leader"))

        self.assertFalse(report["pass"])
        self.assertEqual(len(report["failures"]), 1)
        self.assertIn("did not start a formation transit", report["failures"][0])
        self.assertEqual([call for call, _ in client.calls], ["status", "goto"])

    def test_run_fails_on_timeout_while_transit_still_active(self):
        # The transit starts but never reports as finished; observe() must time out rather than hang,
        # and verdict() must fail on "did not complete" instead of silently accepting a partial leg.
        leader = _vehicle(1, 39.9, 116.4)
        follower = _vehicle(2, 39.9001, 116.4)
        members = [
            {
                "id": 2,
                "speed_mps": 5.0,
                "start_lat": 39.9001,
                "start_lon": 116.4,
                "end_lat": 39.9002,
                "end_lon": 116.4,
                "end_alt_m": 100.0,
            }
        ]
        active_transit = {"active": True, "leg_index": 1, "members": members, "last_result": {}}
        baseline_status = _status(True, [leader, follower], slots={"2": 1})
        active_status = _status(True, [leader, follower], slots={"2": 1}, transit=active_transit)
        goto_response = {"ok": True, "message": "", "status": active_status}
        # Only one entry after the initial snapshot: FakeAcceptanceClient repeats it forever, so the
        # transit never reports as finished and observe() must exit via its own timeout.
        client = FakeAcceptanceClient(
            [baseline_status, active_status], responses={"goto": goto_response}
        )

        report = fsc.run(client, _args("goto-leader", timeout=0.2))

        self.assertFalse(report["pass"])
        self.assertIn("did not complete before the timeout", " ".join(report["failures"]))

    def test_run_passes_start_scenario_when_initially_close_pair_separates(self):
        # Regression for S4: vehicle 2 starts 2.5 m from the (stationary) leader -- already closer than
        # the 3 m floor -- and moves away to its slot. The "start" scenario must judge this on
        # closest_approach_violations only (the pair never gets closer than its 2.5 m start), not the
        # absolute 3 m floor used by S1-S3/S5.
        base = (39.9, 116.4)
        follower_start = destination(*base, 90.0, 2.5)
        follower_end = destination(*base, 180.0, 15.0)

        def follower_at(s):
            lat = follower_start[0] + (follower_end[0] - follower_start[0]) * s
            lon = follower_start[1] + (follower_end[1] - follower_start[1]) * s
            return _vehicle(2, lat, lon)

        leader = _vehicle(1, base[0], base[1])
        members = [
            {
                "id": 2,
                "speed_mps": 5.0,
                "start_lat": follower_start[0],
                "start_lon": follower_start[1],
                "end_lat": follower_end[0],
                "end_lon": follower_end[1],
                "end_alt_m": 100.0,
            }
        ]
        pre_start_status = _status(
            False, [_vehicle(1, *base), _vehicle(2, *follower_start)], leader_id=1, slots={}
        )
        active_transit = {"active": True, "leg_index": 1, "members": members, "last_result": {}}
        finished_transit = {
            "active": False,
            "leg_index": 1,
            "members": members,
            "last_result": {"kind": "reshape", "success": True, "reason": "", "leg_count": 1},
        }
        poll_statuses = [
            _status(True, [leader, follower_at(0.0)], slots={"2": 1}, transit=active_transit),
            _status(True, [leader, follower_at(0.5)], slots={"2": 1}, transit=active_transit),
            _status(True, [leader, follower_at(1.0)], slots={"2": 1}, transit=active_transit),
            _status(True, [leader, follower_at(1.0)], slots={"2": 1}, transit=finished_transit),
        ]
        start_formation_response = {"ok": True, "status": poll_statuses[0]}
        client = FakeAcceptanceClient(
            [pre_start_status, *poll_statuses],
            responses={"stop_formation": {"ok": True}, "start_formation": start_formation_response},
        )

        report = fsc.run(client, _args("start", timeout=1.0))

        self.assertTrue(report["pass"], report["failures"])
        self.assertEqual(report["legs"][0]["closest_approach_violations"], [])

    def test_run_fails_start_scenario_when_pair_closes_further(self):
        # Same shape as above, but the follower's end point closes the 2.5 m gap instead of opening it:
        # this must be flagged even though it never crosses the absolute 3 m floor check.
        base = (39.9, 116.4)
        follower_start = destination(*base, 90.0, 2.5)
        follower_end = destination(*base, 90.0, 0.2)  # ends well inside the starting gap

        def follower_at(s):
            lat = follower_start[0] + (follower_end[0] - follower_start[0]) * s
            lon = follower_start[1] + (follower_end[1] - follower_start[1]) * s
            return _vehicle(2, lat, lon)

        leader = _vehicle(1, base[0], base[1])
        members = [
            {
                "id": 2,
                "speed_mps": 5.0,
                "start_lat": follower_start[0],
                "start_lon": follower_start[1],
                "end_lat": follower_end[0],
                "end_lon": follower_end[1],
                "end_alt_m": 100.0,
            }
        ]
        pre_start_status = _status(
            False, [_vehicle(1, *base), _vehicle(2, *follower_start)], leader_id=1, slots={}
        )
        active_transit = {"active": True, "leg_index": 1, "members": members, "last_result": {}}
        finished_transit = {
            "active": False,
            "leg_index": 1,
            "members": members,
            "last_result": {"kind": "reshape", "success": True, "reason": "", "leg_count": 1},
        }
        poll_statuses = [
            _status(True, [leader, follower_at(0.0)], slots={"2": 1}, transit=active_transit),
            _status(True, [leader, follower_at(1.0)], slots={"2": 1}, transit=active_transit),
            _status(True, [leader, follower_at(1.0)], slots={"2": 1}, transit=finished_transit),
        ]
        start_formation_response = {"ok": True, "status": poll_statuses[0]}
        client = FakeAcceptanceClient(
            [pre_start_status, *poll_statuses],
            responses={"stop_formation": {"ok": True}, "start_formation": start_formation_response},
        )

        report = fsc.run(client, _args("start", timeout=1.0))

        self.assertFalse(report["pass"])
        self.assertEqual(len(report["legs"][0]["closest_approach_violations"]), 1)

    def test_observe_excludes_tail_samples_from_separation_and_deviation(self):
        # Two members stay a safe 20 m apart for the whole leg, then (only after the leg ends, in the
        # tail window used solely to catch late arrivals) report positions 0.1 m apart. That must not
        # move min_separation_m or raise a closest-approach violation.
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

        def leg_status(s, transit):
            # Vehicle 2 is the leader here (it is moving too, as in a goto-leader leg); vehicle 3 is its
            # only follower, so member_ids == {2, 3} with no third vehicle to accidentally collide with.
            a = lerp(base, a_end, s)
            b = lerp(b_start, b_end, s)
            return _status(
                True,
                [_vehicle(2, *a), _vehicle(3, *b)],
                leader_id=2,
                slots={"3": 1},
                transit=transit,
            )

        active_transit = {"active": True, "leg_index": 1, "members": members, "last_result": {}}
        finished_transit = {
            "active": False,
            "leg_index": 1,
            "members": members,
            "last_result": {"kind": "goto", "success": True, "reason": "", "leg_count": 1},
        }
        close = destination(*a_end, 0.0, 0.1)
        tail_vehicles = [_vehicle(2, *a_end), _vehicle(3, *close)]
        statuses = [
            leg_status(0.0, active_transit),
            leg_status(0.5, active_transit),
            leg_status(1.0, active_transit),
            {**leg_status(1.0, finished_transit), "vehicles": tail_vehicles},
        ]
        client = FakeAcceptanceClient(statuses)

        observation = fsc.observe(client, timeout_s=0.3, baseline_result={})

        self.assertTrue(observation["observed"])
        self.assertTrue(observation["completed"])
        leg = observation["legs"][0]
        self.assertAlmostEqual(leg["min_separation_m"], 20.0, delta=0.5)
        self.assertEqual(leg["closest_approach_violations"], [])

    def test_run_reports_nonzero_start_spread_and_fails_goto_leader_limit(self):
        # The leader jumps to its target on the second sample; the follower is held at its start for
        # about 2 real seconds (repeating its unmoved status many times -- "started" is detected from
        # real elapsed time between polls, not from any time value embedded in a status). start_spread_s
        # must come back non-zero and the goto-leader scenario (limit 1.5 s) must fail on it by name.
        fsc.POLL_S = 0.1  # coarser polling so ~2 s of real delay needs only ~20 iterations
        base = (39.9, 116.4)
        leader_end = destination(*base, 90.0, 100.0)
        follower_start = destination(*base, 0.0, 15.0)
        follower_end = destination(*leader_end, 0.0, 15.0)
        leader_origin = _vehicle(1, *base)
        leader_arrived = _vehicle(1, *leader_end)
        follower_stuck = _vehicle(2, *follower_start)
        follower_arrived = _vehicle(2, *follower_end)

        members = [
            {
                "id": 1,
                "speed_mps": 5.0,
                "start_lat": base[0],
                "start_lon": base[1],
                "end_lat": leader_end[0],
                "end_lon": leader_end[1],
                "end_alt_m": 100.0,
            },
            {
                "id": 2,
                "speed_mps": 5.0,
                "start_lat": follower_start[0],
                "start_lon": follower_start[1],
                "end_lat": follower_end[0],
                "end_lon": follower_end[1],
                "end_alt_m": 100.0,
            },
        ]
        baseline_status = _status(
            True, [leader_origin, follower_stuck], leader_id=1, slots={"2": 1}
        )
        active_transit = {"active": True, "leg_index": 1, "members": members, "last_result": {}}
        finished_transit = {
            "active": False,
            "leg_index": 1,
            "members": members,
            "last_result": {"kind": "goto", "success": True, "reason": "", "leg_count": 1},
        }
        origin_status = _status(
            True,
            [leader_origin, follower_stuck],
            leader_id=1,
            slots={"2": 1},
            transit=active_transit,
        )
        leader_started_status = _status(
            True,
            [leader_arrived, follower_stuck],
            leader_id=1,
            slots={"2": 1},
            transit=active_transit,
        )
        follower_started_status = _status(
            True,
            [leader_arrived, follower_arrived],
            leader_id=1,
            slots={"2": 1},
            transit=active_transit,
        )
        finished_status = _status(
            True,
            [leader_arrived, follower_arrived],
            leader_id=1,
            slots={"2": 1},
            transit=finished_transit,
        )
        statuses = [
            baseline_status,
            origin_status,
            *([leader_started_status] * 20),  # ~2 s of real polling with only the follower unmoved
            follower_started_status,
            finished_status,
        ]
        goto_response = {"ok": True, "message": "", "status": origin_status}
        client = FakeAcceptanceClient(statuses, responses={"goto": goto_response})

        report = fsc.run(client, _args("goto-leader", timeout=5.0))

        start_spread_s = report["legs"][0]["start_spread_s"]
        self.assertIsNotNone(start_spread_s)
        self.assertGreater(start_spread_s, 1.5)
        self.assertFalse(report["pass"])
        self.assertTrue(
            any("start_spread_s" in failure for failure in report["failures"]), report["failures"]
        )

    def test_run_passes_when_reshape_completes_instantly_already_converged(self):
        # The fleet is already in formation; starting it finishes before transit ever reports active.
        # That must still count as a pass for the "start" scenario.
        base = (39.9, 116.4)
        vehicles = [_vehicle(1, *base), _vehicle(2, *destination(*base, 90.0, 15.0))]
        pre_start_status = _status(False, vehicles, leader_id=1, slots={})
        instant_result = {"kind": "reshape", "success": True, "reason": "", "leg_count": 0}
        instant_status = _status(
            True,
            vehicles,
            leader_id=1,
            slots={"2": 1},
            transit={"active": False, "last_result": instant_result},
        )
        client = FakeAcceptanceClient(
            [pre_start_status, instant_status],
            responses={
                "stop_formation": {"ok": True, "status": pre_start_status},
                "start_formation": {"ok": True, "status": instant_status},
            },
        )

        report = fsc.run(client, _args("start", timeout=1.0))

        self.assertTrue(report["pass"], report["failures"])
        self.assertEqual(report["legs"], [])

    def test_run_fails_when_non_start_scenario_completes_instantly(self):
        # Same instant-completion shape, but for a non-"start" scenario: that must always fail, even
        # with success=true and the expected kind, because no movement was ever actually observed.
        base = (39.9, 116.4)
        leader = _vehicle(1, *base)
        follower = _vehicle(2, *destination(*base, 90.0, 15.0))
        baseline_status = _status(True, [leader, follower], leader_id=1, slots={"2": 1})
        instant_result = {"kind": "goto", "success": True, "reason": "", "leg_count": 1}
        instant_status = _status(
            True,
            [leader, follower],
            leader_id=1,
            slots={"2": 1},
            transit={"active": False, "last_result": instant_result},
        )
        goto_response = {"ok": True, "message": "", "status": instant_status}
        client = FakeAcceptanceClient(
            [baseline_status, instant_status], responses={"goto": goto_response}
        )

        report = fsc.run(client, _args("reverse", timeout=1.0))

        self.assertFalse(report["pass"])
        self.assertEqual(len(report["failures"]), 1)
        self.assertIn("only expected for the start scenario", report["failures"][0])

    def test_run_reports_status_polling_failure_instead_of_timeout(self):
        # observe() must distinguish "the connection kept failing" from "it simply timed out while
        # active" -- the operator needs to know to check connectivity, not formation behavior.
        leader = _vehicle(1, 39.9, 116.4)
        follower = _vehicle(2, 39.9001, 116.4)
        members = [
            {
                "id": 2,
                "speed_mps": 5.0,
                "start_lat": 39.9001,
                "start_lon": 116.4,
                "end_lat": 39.9002,
                "end_lon": 116.4,
                "end_alt_m": 100.0,
            }
        ]
        active_transit = {"active": True, "leg_index": 1, "members": members, "last_result": {}}
        baseline_status = _status(True, [leader, follower], slots={"2": 1})
        active_status = _status(True, [leader, follower], slots={"2": 1}, transit=active_transit)
        goto_response = {"ok": True, "message": "", "status": active_status}
        # The last entry (a connection error) repeats forever, so polling keeps failing until give-up.
        client = FakeAcceptanceClient(
            [baseline_status, active_status, QGCAcceptanceError("connection refused")],
            responses={"goto": goto_response},
        )

        report = fsc.run(client, _args("goto-leader", timeout=5.0))

        self.assertFalse(report["pass"])
        self.assertEqual(len(report["failures"]), 1)
        self.assertIn("status polling failed", report["failures"][0])
        self.assertIn("connection refused", report["failures"][0])

    def test_run_stops_before_command_when_restart_reshape_times_out(self):
        # If the restart's reshape never finishes, run() must report that and stop -- not go on to
        # issue the scenario's actual goto/route command against a formation that may not be ready.
        base = (39.9, 116.4)
        leader = _vehicle(1, *base)
        follower = _vehicle(2, *destination(*base, 90.0, 2.5))
        pre_status = _status(False, [leader, follower], leader_id=1, slots={})
        members = [
            {
                "id": 2,
                "speed_mps": 5.0,
                "start_lat": follower["lat"],
                "start_lon": follower["lon"],
                "end_lat": follower["lat"],
                "end_lon": follower["lon"],
                "end_alt_m": 100.0,
            }
        ]
        active_transit = {"active": True, "leg_index": 1, "members": members, "last_result": {}}
        # This single entry repeats forever: the reshape never reports as finished.
        active_status = _status(
            True, [leader, follower], leader_id=1, slots={"2": 1}, transit=active_transit
        )
        client = FakeAcceptanceClient(
            [pre_status, active_status],
            responses={
                "stop_formation": {"ok": True, "status": pre_status},
                "start_formation": {"ok": True, "status": active_status},
            },
        )

        report = fsc.run(client, _args("goto-leader", timeout=0.2))

        self.assertFalse(report["pass"])
        self.assertEqual(report["scenario"], "goto-leader")
        self.assertTrue(any("did not complete before the timeout" in f for f in report["failures"]))
        self.assertNotIn("goto", [call for call, _ in client.calls])

    def test_run_restart_baseline_uses_post_stop_status_not_pre_stop(self):
        # Regression: if a transit was active and stop_formation's own abort changes last_result,
        # baselining on the pre-stop status would make the post-restart check wrongly believe
        # start_formation had already produced a new result, just because last_result already
        # differed from the stale pre-stop value.
        base = (39.9, 116.4)
        leader = _vehicle(1, *base)
        follower = _vehicle(2, *destination(*base, 90.0, 15.0))
        old_result = {"kind": "goto", "success": True, "reason": "", "leg_count": 1}
        aborted_result = {
            "kind": "goto",
            "success": False,
            "reason": "aborted by stop_formation",
            "leg_count": 1,
        }
        pre_stop_status = _status(
            True,
            [leader, follower],
            leader_id=1,
            slots={"2": 1},
            transit={"active": True, "leg_index": 2, "members": [], "last_result": old_result},
        )
        post_stop_status = _status(
            False,
            [leader, follower],
            leader_id=1,
            slots={},
            transit={"active": False, "last_result": aborted_result},
        )
        client = FakeAcceptanceClient(
            [pre_stop_status],
            responses={
                "stop_formation": {"ok": True, "status": post_stop_status},
                # start_formation's own response status still looks exactly like the post-stop
                # snapshot: nothing new has happened yet in this round-trip.
                "start_formation": {"ok": True, "status": post_stop_status},
            },
        )

        report = fsc.run(client, _args("start", timeout=1.0))

        self.assertFalse(report["pass"])
        self.assertEqual(report["failures"], ["command did not start a formation transit"])


if __name__ == "__main__":
    unittest.main()
