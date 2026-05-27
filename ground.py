"""
Ground station utilities.

We use a simple spherical Earth model consistent with the rest of the simulator.
This is sufficient for elevation-mask visibility + handover logic.
"""

from __future__ import annotations

from dataclasses import dataclass
import math
from typing import Tuple

import numpy as np

from orbit import earth_rotation_rate_rad_s


@dataclass(frozen=True)
class GroundStation:
    gid: int
    name: str
    lat_rad: float
    lon_rad: float
    alt_km: float = 0.0
    min_elev_deg: float = 10.0

    def ecef_position_km(self, earth_radius_km: float) -> np.ndarray:
        r = earth_radius_km + float(self.alt_km)
        clat = math.cos(self.lat_rad)
        slat = math.sin(self.lat_rad)
        clon = math.cos(self.lon_rad)
        slon = math.sin(self.lon_rad)
        return np.array([r * clat * clon, r * clat * slon, r * slat], dtype=float)


def ecef_to_eci(r_ecef_km: np.ndarray, t_since_epoch_s: float, theta0_rad: float = 0.0) -> np.ndarray:
    """
    Inverse of the simple ECI->ECEF rotation in orbit.py.
    ECI = R3(-theta) * ECEF
    """
    theta = theta0_rad + earth_rotation_rate_rad_s() * float(t_since_epoch_s)
    c, s = math.cos(theta), math.sin(theta)
    R3m = np.array([[c, -s, 0.0], [s, c, 0.0], [0.0, 0.0, 1.0]], dtype=float)
    return R3m @ np.asarray(r_ecef_km, dtype=float)


def elevation_angle_rad(station_ecef_km: np.ndarray, sat_ecef_km: np.ndarray) -> float:
    """
    Compute elevation angle above local horizon at station.
    Spherical Earth: "up" is along station position vector.
    """
    rs = np.asarray(station_ecef_km, dtype=float)
    r_sat = np.asarray(sat_ecef_km, dtype=float)
    rho = r_sat - rs
    rho_norm = float(np.linalg.norm(rho))
    if rho_norm == 0.0:
        return -math.pi / 2.0
    up = rs / float(np.linalg.norm(rs))
    sin_el = float(np.dot(rho / rho_norm, up))
    sin_el = max(-1.0, min(1.0, sin_el))
    return math.asin(sin_el)

