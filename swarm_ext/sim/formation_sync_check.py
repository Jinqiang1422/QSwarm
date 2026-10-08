#!/usr/bin/env python3
"""SITL acceptance for synchronized formation legs (scenarios S1-S5).

Design: swarm_ext/docs/specs/2026-10-05-synchronized-formation-legs-design.md, section 4.
Drives a running QGC through its loopback acceptance interface and prints per-leg metrics as JSON.
The fleet must already be airborne. The operator may be using the same GUI: confirm before running.

Example:
  python3 formation_sync_check.py --token-file /tmp/qgc-swarm5-token/token --scenario goto-leader
"""

from __future__ import annotations

import argparse
import json
import sys
import time

from qgc_acceptance import (
    QGCAcceptanceClient,
    QGCAcceptanceError,
    destination,
    formation_sync_metrics,
    read_acceptance_token,
)

POLL_S = 0.25
ARRIVAL_TAIL_S = 3.0
MIN_SEPARATION_M = 3.0
ROUTE_UPLOAD_TIMEOUT_S = 30.0
MAX_CONSECUTIVE_STATUS_ERRORS = 3
AIRBORNE_ALT_REL_M = 5.0
# FormationController::FormationType (src/Swarm/FormationController.h); this script always requests wedge.
FORMATION_TYPE_WEDGE = 0

# One entry per --scenario choice. "expected_kind" must match transit.last_result.kind (goto/route/reshape).
# "separation_check" selects how a leg's proximity is judged: "absolute" is the S1-S3/S5 floor (no pair may
# be closer than MIN_SEPARATION_M); "violations" is the S4 rule (a pair may already be closer than that at
# rest, but may not close further -- see formation_sync_metrics' closest_approach_violations).
SCENARIO_LIMITS = {
    "start": {
        "expected_kind": "reshape",
        "limits": {"arrival_spread_s": 3.0},
        "separation_check": "violations",
    },
    "goto-leader": {
        "expected_kind": "goto",
        "limits": {"start_spread_s": 1.5, "arrival_spread_s": 3.0, "max_deviation_m": 3.0},
        "separation_check": "absolute",
    },
    "goto-follower": {
        "expected_kind": "goto",
        "limits": {"start_spread_s": 1.5, "arrival_spread_s": 3.0, "max_deviation_m": 3.0},
        "separation_check": "absolute",
    },
    "route": {
        "expected_kind": "route",
        "limits": {"start_spread_s": 1.5, "arrival_spread_s": 3.0, "max_deviation_m": 3.0},
        "separation_check": "absolute",
    },
    "reverse": {
        "expected_kind": "goto",
        "limits": {},
        "separation_check": "absolute",
    },
}


def _formation(status: dict) -> dict:
    return status.get("formation", {})


def _vehicle(status: dict, vehicle_id: int) -> dict | None:
    return next((v for v in status.get("vehicles", []) if int(v["id"]) == vehicle_id), None)


def _refusal_reason(response: dict) -> dict:
    """``response`` with the bulky nested 'status' snapshot dropped, so 'message'/'summary' stay visible."""
    return {key: value for key, value in response.items() if key != "status"}


def _airborne(vehicle: dict) -> bool:
    alt_rel_m = vehicle.get("alt_rel_m")
    return (
        bool(vehicle.get("armed"))
        and isinstance(alt_rel_m, (int, float))
        and alt_rel_m >= AIRBORNE_ALT_REL_M
    )


def _began_transit(baseline_result: dict, response: dict) -> bool:
    """False when ``response`` claims ok but the command never actually started a transit.

    Guards against a false pass from a stale ``last_result``: a command can be accepted (ok=true)
    without moving anything (e.g. a no-op), in which case transit.active is already false again by
    the time the response's status snapshot was taken, and last_result still reads the same as it did
    before the command was sent.
    """
    transit = _formation(response.get("status", {})).get("transit", {})
    if transit.get("active"):
        return True
    return transit.get("last_result", {}) != baseline_result


def _needs_restart(args: argparse.Namespace, formation: dict) -> bool:
    if args.scenario == "start" or not formation.get("active"):
        return True
    return (
        int(formation.get("leader_id", -1)) != args.leader
        or int(formation.get("type", -1)) != FORMATION_TYPE_WEDGE
        or abs(float(formation.get("spacing_m", -1.0)) - args.spacing) > 1e-6
        or abs(float(formation.get("vertical_step_m", -1.0)) - args.vertical_step) > 1e-6
    )


def _check_preconditions(status: dict, will_restart: bool, leader_id: int) -> str | None:
    vehicles_by_id = {int(v["id"]): v for v in status.get("vehicles", [])}
    if will_restart:
        if leader_id not in vehicles_by_id or not _airborne(vehicles_by_id[leader_id]):
            return (
                f"leader vehicle {leader_id} is not armed and airborne "
                f"(alt_rel_m >= {AIRBORNE_ALT_REL_M} m)"
            )
        airborne = sum(1 for vehicle in vehicles_by_id.values() if _airborne(vehicle))
        if airborne < 2:
            return f"fewer than 2 vehicles are armed and airborne (alt_rel_m >= {AIRBORNE_ALT_REL_M} m): {airborne}"
        return None
    formation = _formation(status)
    formation_leader_id = formation.get("leader_id")
    if formation_leader_id is None or int(formation_leader_id) not in vehicles_by_id:
        return "formation leader vehicle is missing from status"
    slots = formation.get("slots", {})
    if not slots:
        return "formation has no followers (empty slots)"
    member_ids = {int(formation_leader_id)} | {int(vehicle_id) for vehicle_id in slots}
    not_ready = sorted(
        vehicle_id
        for vehicle_id in member_ids
        if vehicle_id not in vehicles_by_id or not _airborne(vehicles_by_id[vehicle_id])
    )
    if not_ready:
        return f"formation member(s) not armed and airborne (alt_rel_m >= {AIRBORNE_ALT_REL_M} m): {not_ready}"
    return None


def _pick_goto_follower(status: dict, formation: dict) -> tuple[int | None, str | None]:
    vehicles_by_id = {int(v["id"]): v for v in status.get("vehicles", [])}
    candidates = sorted(
        vehicle_id
        for vehicle_id in (int(key) for key in formation.get("slots", {}))
        if vehicle_id in vehicles_by_id
        and vehicles_by_id[vehicle_id].get("automation_allowed")
        and _airborne(vehicles_by_id[vehicle_id])
    )
    if not candidates:
        return None, "no follower is automation_allowed and airborne"
    return candidates[0], None


def _run_route(
    client: QGCAcceptanceClient,
    args: argparse.Namespace,
    leader: dict,
    heading: float,
    baseline: dict,
) -> tuple[str | None, dict]:
    """Upload a 3-waypoint route for the leader, wait for it to land, then start the formation route once."""
    alt_rel = float(leader["alt_rel_m"])
    first = destination(float(leader["lat"]), float(leader["lon"]), heading, 120.0)
    second = destination(*first, heading + 90.0, 120.0)
    third = destination(*second, heading + 180.0, 80.0)
    waypoints = [
        {"lat": first[0], "lon": first[1], "alt_rel_m": alt_rel},
        {"lat": second[0], "lon": second[1], "alt_rel_m": alt_rel + 5.0},
        {"lat": third[0], "lon": third[1], "alt_rel_m": alt_rel + 5.0},
    ]
    uploaded = client.request("upload_route", vehicle_id=args.leader, waypoints=waypoints)
    if not uploaded.get("ok"):
        return f"upload_route refused: {_refusal_reason(uploaded)}", uploaded

    state = None
    deadline = time.monotonic() + ROUTE_UPLOAD_TIMEOUT_S
    while time.monotonic() < deadline:
        state = client.request("status").get("route_uploads", {}).get(str(args.leader))
        if state in ("ok", "failed"):
            break
        time.sleep(POLL_S)
    if state != "ok":
        return f"route upload did not complete: route_uploads[{args.leader}]={state!r}", uploaded

    response = client.request(
        "formation_route"
    )  # confirmation summary, then start -- a single attempt
    if not response.get("ok"):
        return f"formation_route refused: {_refusal_reason(response)}", response
    summary = response.get("summary", "")
    if "3 个航点" not in summary:
        return f"formation_route summary did not mention 3 waypoints: {summary!r}", response
    if not _began_transit(baseline, response):
        return "command did not start a formation transit", response
    return None, response


def observe(client: QGCAcceptanceClient, timeout_s: float, baseline_result: dict) -> dict:
    """Sample the transit that was just started until it ends.

    ``baseline_result`` is transit.last_result as it read right before the command that (should have)
    started this transit was sent; it is used to notice a transit that both started and finished between
    that command and our first few polls, with no window where it showed as active.

    Returns ``{"legs": [...], "result": {...}, "observed": bool, "completed": bool, "error": str|None}``.
    "observed" is true once any leg was ever seen active. "completed" is true once the last leg observed
    was seen to end (not merely that the timeout elapsed while it was still active), or once an instant
    completion (observed=False) was detected. "error" is set when polling gave up after repeated
    QGCAcceptanceError failures, instead of silently reporting a timeout.
    """
    origin = time.monotonic()
    legs: list[dict] = []
    ended_at = None
    status: dict = {}
    consecutive_errors = 0
    error = None
    instant_result = None
    while time.monotonic() - origin < timeout_s:
        try:
            status = client.request("status")
            consecutive_errors = 0
        except QGCAcceptanceError as exc:
            consecutive_errors += 1
            if consecutive_errors > MAX_CONSECUTIVE_STATUS_ERRORS:
                error = f"status polling failed: {exc}"
                break
            time.sleep(POLL_S)
            continue
        now = time.monotonic() - origin
        formation = _formation(status)
        transit = formation.get("transit", {})
        vehicles = status.get("vehicles", [])
        if not legs and not transit.get("active"):
            last_result = transit.get("last_result", {})
            if last_result != baseline_result:
                # The whole transit started and finished between the command and this poll, with no
                # window where it ever showed as active: stop waiting instead of spinning to the timeout.
                instant_result = last_result
                break
        if transit.get("active"):
            if not legs or legs[-1]["index"] != transit.get("leg_index"):
                if legs:
                    legs[-1]["ended_at"] = now
                leader_id = formation.get("leader_id")
                member_ids = {int(leader_id)} if leader_id is not None else set()
                member_ids |= {int(vehicle_id) for vehicle_id in formation.get("slots", {})}
                legs.append(
                    {
                        "index": transit.get("leg_index"),
                        "members": transit.get("members", []),
                        "member_ids": member_ids,
                        "samples": [],
                        "tail": [],
                        "ended_at": None,
                    }
                )
        elif legs and legs[-1]["ended_at"] is None:
            legs[-1]["ended_at"] = now
            ended_at = now
        for leg in legs:
            if leg["ended_at"] is None:
                leg["samples"].append((now, vehicles))
            elif now - leg["ended_at"] <= ARRIVAL_TAIL_S:
                leg["tail"].append((now, vehicles))
        if ended_at is not None and now - ended_at > ARRIVAL_TAIL_S:
            break
        time.sleep(POLL_S)

    if instant_result is not None:
        return {
            "legs": [],
            "result": instant_result,
            "observed": False,
            "completed": True,
            "error": None,
        }
    completed = error is None and bool(legs) and legs[-1]["ended_at"] is not None
    metrics = [
        {
            "leg": leg["index"],
            **formation_sync_metrics(
                leg["members"], leg["samples"], leg["tail"], member_ids=leg["member_ids"]
            ),
        }
        for leg in legs
    ]
    result = _formation(status).get("transit", {}).get("last_result", {})
    return {
        "legs": metrics,
        "result": result,
        "observed": bool(legs),
        "completed": completed,
        "error": error,
    }


def verdict(scenario: str, observation: dict) -> dict:
    config = SCENARIO_LIMITS[scenario]
    metrics = observation["legs"]
    result = observation["result"]
    failures = []
    if observation.get("error"):
        failures.append(observation["error"])
    elif not observation["observed"]:
        if observation["completed"]:
            # The transit started and finished before we ever saw it active (e.g. the fleet was
            # already converged). That is only an expected, passing outcome for the "start" scenario.
            if not (
                scenario == "start"
                and result.get("success")
                and result.get("kind") == config["expected_kind"]
            ):
                failures.append(
                    "transit completed before it was ever observed active "
                    f"(kind={result.get('kind')!r}, success={result.get('success')!r}); that is only "
                    "expected for the start scenario when the formation was already converged"
                )
        else:
            failures.append("no formation transit was observed")
    elif not observation["completed"]:
        failures.append("formation transit did not complete before the timeout")
    else:
        if not result.get("success"):
            failures.append(f"transit did not complete: {result.get('reason', 'unknown reason')}")
        if result.get("kind") != config["expected_kind"]:
            failures.append(
                f"transit kind {result.get('kind')!r} did not match expected {config['expected_kind']!r}"
            )
    for leg in metrics:
        if config["separation_check"] == "absolute":
            separation = leg["min_separation_m"]
            if separation is not None and separation < MIN_SEPARATION_M:
                failures.append(
                    f"leg {leg['leg']}: separation {separation} m < {MIN_SEPARATION_M} m"
                )
        else:
            for violation in leg["closest_approach_violations"]:
                failures.append(
                    f"leg {leg['leg']}: pair {violation['pair']} closed from "
                    f"{violation['initial_separation_m']} m to {violation['min_separation_m']} m"
                )
        if leg["moving_members"] < 2:
            continue
        for key, limit in config["limits"].items():
            value = leg[key]
            if value is None or value > limit:
                failures.append(f"leg {leg['leg']}: {key}={value} (limit {limit})")
    return {
        "scenario": scenario,
        "pass": not failures,
        "failures": failures,
        "legs": metrics,
        "result": result,
    }


def run(client: QGCAcceptanceClient, args: argparse.Namespace) -> dict:
    status = client.request("status")
    formation = _formation(status)
    will_restart = _needs_restart(args, formation)
    failure = _check_preconditions(status, will_restart, args.leader)
    if failure:
        return {"scenario": args.scenario, "pass": False, "failures": [failure]}

    if will_restart:
        # The baseline must come from *after* stop_formation: if a transit was active, stopping it can
        # itself change last_result (e.g. mark it aborted). Baselining on the pre-stop value would make
        # _began_transit() see that stop-induced change and wrongly credit it to start_formation below.
        stop_response = client.request("stop_formation")
        baseline = (
            _formation(stop_response.get("status", {})).get("transit", {}).get("last_result", {})
        )
        response = client.request(
            "start_formation",
            leader_id=args.leader,
            type="wedge",
            spacing_m=args.spacing,
            vertical_step_m=args.vertical_step,
        )
        if not response.get("ok"):
            return {
                "scenario": args.scenario,
                "pass": False,
                "failures": [f"start_formation refused: {_refusal_reason(response)}"],
            }
        if not _began_transit(baseline, response):
            return {
                "scenario": args.scenario,
                "pass": False,
                "failures": ["command did not start a formation transit"],
            }
        observation = observe(client, args.timeout, baseline)
        if args.scenario == "start":
            return verdict("start", observation)
        reshape_report = verdict("start", observation)
        if not reshape_report["pass"]:
            # The restart's reshape itself did not finish cleanly; stop here instead of issuing the
            # scenario's actual command against a formation that may not be in the expected state.
            reshape_report["scenario"] = args.scenario
            return reshape_report
        status = client.request("status")
        formation = _formation(status)
        failure = _check_preconditions(status, False, args.leader)
        if failure:
            return {"scenario": args.scenario, "pass": False, "failures": [failure]}

    leader = _vehicle(status, args.leader)
    if leader is None:
        return {
            "scenario": args.scenario,
            "pass": False,
            "failures": [f"leader vehicle {args.leader} is missing from status"],
        }
    heading = float(formation.get("heading_deg", 0.0))
    baseline = formation.get("transit", {}).get("last_result", {})

    if args.scenario in ("goto-leader", "goto-follower", "reverse"):
        bearing, distance = (
            (heading + 180.0, 30.0) if args.scenario == "reverse" else (heading + 90.0, 150.0)
        )
        lat, lon = destination(float(leader["lat"]), float(leader["lon"]), bearing, distance)
        target_id = args.leader
        if args.scenario == "goto-follower":
            target_id, failure = _pick_goto_follower(status, formation)
            if failure:
                return {"scenario": args.scenario, "pass": False, "failures": [failure]}
        response = client.request("goto", vehicle_id=target_id, center_lat=lat, center_lon=lon)
        if not response.get("ok"):
            failure = f"goto refused: {_refusal_reason(response)}"
        elif not _began_transit(baseline, response):
            failure = "command did not start a formation transit"
        else:
            failure = None
    else:  # route: three waypoints with two 90 deg turns and a 5 m climb
        failure, _response = _run_route(client, args, leader, heading, baseline)
    if failure:
        return {"scenario": args.scenario, "pass": False, "failures": [failure]}
    observation = observe(client, args.timeout, baseline)
    return verdict(args.scenario, observation)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=15615)
    parser.add_argument("--token-file", required=True)
    parser.add_argument("--leader", type=int, default=1)
    parser.add_argument("--spacing", type=float, default=15.0)
    parser.add_argument("--vertical-step", type=float, default=3.0)
    parser.add_argument("--timeout", type=float, default=240.0)
    parser.add_argument(
        "--scenario",
        required=True,
        choices=["start", "goto-leader", "goto-follower", "route", "reverse"],
    )
    args = parser.parse_args()
    try:
        client = QGCAcceptanceClient(args.host, args.port, read_acceptance_token(args.token_file))
        report = run(client, args)
    except (QGCAcceptanceError, ValueError, KeyError) as exc:
        print(
            json.dumps(
                {"scenario": args.scenario, "pass": False, "error": str(exc)},
                ensure_ascii=False,
                indent=2,
            )
        )
        return 2
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
