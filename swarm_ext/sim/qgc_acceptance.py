#!/usr/bin/env python3
"""Client and metric helpers for QGC's loopback-only swarm acceptance interface."""

from __future__ import annotations

import contextlib
import json
import math
import os
import secrets
import socket
import stat
import tempfile
import time
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Callable


class QGCAcceptanceError(RuntimeError):
    """The QGC acceptance interface was unavailable or rejected a request."""


MINIMUM_TOKEN_BYTES = 16
MAXIMUM_TOKEN_BYTES = 512


def read_acceptance_token(path: str | Path) -> str:
    """Read an owner-only acceptance token without following symbolic links."""
    token_path = Path(path)
    flags = os.O_RDONLY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(token_path, flags)
    except OSError as exc:
        raise ValueError("QGC acceptance token file is not readable") from exc
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise ValueError("QGC acceptance token path must be a regular file")
        if metadata.st_mode & 0o077:
            raise ValueError("QGC acceptance token file must not grant group or other access")
        if not metadata.st_mode & stat.S_IRUSR:
            raise ValueError("QGC acceptance token file must be readable by its owner")
        encoded = os.read(descriptor, MAXIMUM_TOKEN_BYTES + 3)
    finally:
        os.close(descriptor)

    if encoded.endswith(b"\r\n"):
        encoded = encoded[:-2]
    elif encoded.endswith(b"\n"):
        encoded = encoded[:-1]
    if not MINIMUM_TOKEN_BYTES <= len(encoded) <= MAXIMUM_TOKEN_BYTES:
        raise ValueError(
            f"QGC acceptance token must contain {MINIMUM_TOKEN_BYTES} to "
            f"{MAXIMUM_TOKEN_BYTES} UTF-8 bytes"
        )
    if b"\x00" in encoded or b"\r" in encoded or b"\n" in encoded:
        raise ValueError("QGC acceptance token must contain exactly one line")
    try:
        return encoded.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ValueError("QGC acceptance token must be valid UTF-8") from exc


@contextlib.contextmanager
def acceptance_secret_file(existing_path: str = ""):
    """Yield a validated secret path, creating an ephemeral one when omitted."""
    if existing_path:
        # Keep the final path component unresolved so read_acceptance_token's
        # O_NOFOLLOW check cannot be bypassed by normalizing a symlink first.
        token_path = Path(existing_path).expanduser().absolute()
        read_acceptance_token(token_path)
        yield token_path
        return

    with tempfile.TemporaryDirectory(prefix="qgc-swarm-secret-") as directory:
        secret_dir = Path(directory)
        secret_dir.chmod(0o700)
        token_path = secret_dir / "acceptance-token"
        token_path.write_text(secrets.token_urlsafe(32), encoding="utf-8")
        token_path.chmod(0o600)
        yield token_path


class QGCAcceptanceClient:
    def __init__(self, host: str, port: int, token: str, timeout_sec: float = 5.0):
        token_bytes = token.encode("utf-8")
        if not MINIMUM_TOKEN_BYTES <= len(token_bytes) <= MAXIMUM_TOKEN_BYTES:
            raise ValueError(
                f"QGC acceptance token must contain {MINIMUM_TOKEN_BYTES} to "
                f"{MAXIMUM_TOKEN_BYTES} UTF-8 bytes"
            )
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


def _truth_position_at(truth: dict, at_time_sec: float | None) -> tuple[float, float] | None:
    waypoints = truth.get("waypoints", [])
    if not waypoints:
        return None
    if at_time_sec is None or at_time_sec >= float(waypoints[-1][0]):
        return float(waypoints[-1][1]), float(waypoints[-1][2])
    if at_time_sec < float(waypoints[0][0]):
        return None
    for index in range(len(waypoints) - 1):
        start = waypoints[index]
        end = waypoints[index + 1]
        start_time = float(start[0])
        end_time = float(end[0])
        if start_time <= at_time_sec <= end_time:
            span = end_time - start_time
            ratio = 0.0 if span <= 0.0 else (at_time_sec - start_time) / span
            return (
                float(start[1]) + (float(end[1]) - float(start[1])) * ratio,
                float(start[2]) + (float(end[2]) - float(start[2])) * ratio,
            )
    return float(waypoints[-1][1]), float(waypoints[-1][2])


def cop_truth_metrics(scenario: dict, tracks: list[dict], at_time_sec: float | None = None) -> dict:
    """Greedily match active truth points to same-category COP tracks and report geo error."""
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
    active_truths = []
    for truth_index, truth in enumerate(scenario.get("truths", [])):
        position = _truth_position_at(truth, at_time_sec)
        if position is None:
            continue
        active_truths.append(truth_index)
        truth_lat, truth_lon = position
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
    errors_by_truth = {}
    for error, truth_index, track_index in candidates:
        if truth_index in used_truths or track_index in used_tracks:
            continue
        used_truths.add(truth_index)
        used_tracks.add(track_index)
        rounded_error = round(error, 2)
        errors.append(rounded_error)
        errors_by_truth[str(truth_index)] = rounded_error
    truth_count = len(active_truths)
    return {
        "pass": len(errors) == truth_count and truth_count > 0,
        "matched_truths": len(errors),
        "truth_count": truth_count,
        "errors_m": errors,
        "errors_by_truth_m": errors_by_truth,
        "max_error_m": max(errors, default=None),
    }
