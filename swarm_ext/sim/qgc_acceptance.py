#!/usr/bin/env python3
"""Client and metric helpers for QGC's loopback-only swarm acceptance interface."""

from __future__ import annotations

import json
import math
import socket
import time
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Callable


class QGCAcceptanceError(RuntimeError):
    """The QGC acceptance interface was unavailable or rejected a request."""


class QGCAcceptanceClient:
    def __init__(self, host: str, port: int, token: str, timeout_sec: float = 5.0):
        if len(token) < 16:
            raise ValueError("QGC acceptance token must contain at least 16 characters")
        self.host = host
        self.port = port
        self.token = token
        self.timeout_sec = timeout_sec

    def request(self, command: str, **params) -> dict:
        payload = {"token": self.token, "command": command, **params}
        encoded = (json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
        try:
            with socket.create_connection((self.host, self.port), timeout=self.timeout_sec) as sock:
                sock.settimeout(self.timeout_sec)
                sock.sendall(encoded)
                response = bytearray()
                while b"\n" not in response:
                    chunk = sock.recv(65536)
                    if not chunk:
                        break
                    response.extend(chunk)
                    if len(response) > 4 * 1024 * 1024:
                        raise QGCAcceptanceError("QGC acceptance response exceeded 4 MiB")
        except OSError as exc:
            raise QGCAcceptanceError(f"QGC acceptance connection failed: {exc}") from exc
        if b"\n" not in response:
            raise QGCAcceptanceError("QGC acceptance response was incomplete")
        try:
            result = json.loads(response.split(b"\n", 1)[0])
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise QGCAcceptanceError("QGC acceptance response was not valid JSON") from exc
        if not isinstance(result, dict):
            raise QGCAcceptanceError("QGC acceptance response was not an object")
        return result

    def wait_status(
        self, predicate: Callable[[dict], bool], timeout_sec: float, interval_sec: float = 0.5
    ) -> dict:
        deadline = time.monotonic() + timeout_sec
        last_status = {}
        last_error = None
        while time.monotonic() < deadline:
            try:
                last_status = self.request("status")
                if last_status.get("ok") and predicate(last_status):
                    return last_status
            except QGCAcceptanceError as exc:
                last_error = exc
            time.sleep(interval_sec)
        if last_error and not last_status:
            raise QGCAcceptanceError(str(last_error))
        return last_status


def distance_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Short-distance equirectangular distance in metres."""
    earth_radius_m = 6371000.0
    mean_lat = math.radians((lat1 + lat2) / 2.0)
    east = math.radians(lon2 - lon1) * math.cos(mean_lat) * earth_radius_m
    north = math.radians(lat2 - lat1) * earth_radius_m
    return math.hypot(east, north)


def destination(lat: float, lon: float, azimuth_deg: float, distance: float) -> tuple[float, float]:
    earth_radius_m = 6371000.0
    angular = distance / earth_radius_m
    bearing = math.radians(azimuth_deg)
    lat1 = math.radians(lat)
    lon1 = math.radians(lon)
    lat2 = math.asin(
        math.sin(lat1) * math.cos(angular) + math.cos(lat1) * math.sin(angular) * math.cos(bearing)
    )
    lon2 = lon1 + math.atan2(
        math.sin(bearing) * math.sin(angular) * math.cos(lat1),
        math.cos(angular) - math.sin(lat1) * math.sin(lat2),
    )
    return math.degrees(lat2), math.degrees(lon2)


def formation_geometry_metrics(status: dict) -> dict:
    formation = status.get("formation", {})
    leader_id = int(formation.get("leader_id", 0))
    spacing = float(formation.get("spacing_m", 0.0))
    vertical_step = float(formation.get("vertical_step_m", 0.0))
    vehicles = {int(vehicle["id"]): vehicle for vehicle in status.get("vehicles", [])}
    leader = vehicles.get(leader_id)
    if not leader or not formation.get("active"):
        return {"pass": False, "error": "formation is not active or leader is missing"}

    followers = [vehicles[vehicle_id] for vehicle_id in sorted(vehicles) if vehicle_id != leader_id]
    horizontal_errors = {}
    vertical_errors = {}
    for index, follower in enumerate(followers, start=1):
        rank = (index + 1) // 2
        side = -1 if index % 2 == 1 else 1
        expected_lat, expected_lon = destination(
            float(leader["lat"]),
            float(leader["lon"]),
            float(leader["heading_deg"]) + 180.0 + side * 30.0,
            spacing * rank,
        )
        horizontal_errors[int(follower["id"])] = round(
            distance_m(
                expected_lat,
                expected_lon,
                float(follower["lat"]),
                float(follower["lon"]),
            ),
            2,
        )
        expected_altitude = float(leader["alt_amsl_m"]) + vertical_step * rank
        vertical_errors[int(follower["id"])] = round(
            abs(float(follower["alt_amsl_m"]) - expected_altitude), 2
        )
    return {
        "pass": bool(followers),
        "follower_count": len(followers),
        "horizontal_error_m": horizontal_errors,
        "vertical_error_m": vertical_errors,
        "max_horizontal_error_m": max(horizontal_errors.values(), default=None),
        "max_vertical_error_m": max(vertical_errors.values(), default=None),
    }


def cop_truth_metrics(scenario: dict, tracks: list[dict]) -> dict:
    """Greedily match final truth points to same-category COP tracks and report geo error."""
    category_ids = {
        "unknown": 0,
        "person": 1,
        "vehicle": 2,
        "vessel": 3,
        "animal": 4,
        "fire": 5,
        "smoke": 6,
    }
    candidates = []
    for truth_index, truth in enumerate(scenario.get("truths", [])):
        waypoints = truth.get("waypoints", [])
        if not waypoints:
            continue
        truth_lat, truth_lon = float(waypoints[-1][1]), float(waypoints[-1][2])
        category = category_ids.get(str(truth.get("category", "unknown")), 0)
        for track_index, track in enumerate(tracks):
            if int(track.get("categoryId", track.get("category", -1))) != category:
                continue
            candidates.append(
                (
                    distance_m(
                        truth_lat,
                        truth_lon,
                        float(track.get("lat", 0.0)),
                        float(track.get("lon", 0.0)),
                    ),
                    truth_index,
                    track_index,
                )
            )
    candidates.sort()
    used_truths = set()
    used_tracks = set()
    errors = []
    for error, truth_index, track_index in candidates:
        if truth_index in used_truths or track_index in used_tracks:
            continue
        used_truths.add(truth_index)
        used_tracks.add(track_index)
        errors.append(round(error, 2))
    truth_count = sum(bool(truth.get("waypoints")) for truth in scenario.get("truths", []))
    return {
        "pass": len(errors) == truth_count and truth_count > 0,
        "matched_truths": len(errors),
        "truth_count": truth_count,
        "errors_m": errors,
        "max_error_m": max(errors, default=None),
    }
