"""
Lightweight orbital mechanics utilities (Earth-centered inertial).

Units:
- Distance: km
- Velocity: km/s
- Time: s

This module intentionally implements a simple, dependency-light two-body Kepler
propagator (universal variables) suitable for real-time visualization.
"""

from __future__ import annotations

from dataclasses import dataclass
import math
from typing import Tuple

import numpy as np


@dataclass(frozen=True)
class OrbitalElements:
    """Classical orbital elements (COE). Angles in radians."""

    a_km: float  # semi-major axis
    e: float  # eccentricity
    i_rad: float  # inclination
    raan_rad: float  # right ascension of ascending node
    argp_rad: float  # argument of perigee
    nu_rad: float  # true anomaly


@dataclass
class CartesianState:
    """Cartesian state in ECI."""

    r_km: np.ndarray  # shape (3,)
    v_km_s: np.ndarray  # shape (3,)

    def copy(self) -> "CartesianState":
        return CartesianState(self.r_km.copy(), self.v_km_s.copy())


def _norm(x: np.ndarray) -> float:
    return float(np.linalg.norm(x))


def _stumpff_C(z: float) -> float:
    if z > 0:
        sz = math.sqrt(z)
        return (1.0 - math.cos(sz)) / z
    if z < 0:
        sz = math.sqrt(-z)
        return (math.cosh(sz) - 1.0) / (-z)
    return 0.5


def _stumpff_S(z: float) -> float:
    if z > 0:
        sz = math.sqrt(z)
        return (sz - math.sin(sz)) / (sz**3)
    if z < 0:
        sz = math.sqrt(-z)
        return (math.sinh(sz) - sz) / (sz**3)
    return 1.0 / 6.0


def coe_to_rv(mu_km3_s2: float, coe: OrbitalElements) -> CartesianState:
    """Convert classical orbital elements to ECI position/velocity."""
    a, e, inc, raan, argp, nu = (
        coe.a_km,
        coe.e,
        coe.i_rad,
        coe.raan_rad,
        coe.argp_rad,
        coe.nu_rad,
    )

    if a <= 0:
        raise ValueError("Semi-major axis must be > 0")
    if e < 0:
        raise ValueError("Eccentricity must be >= 0")

    p = a * (1.0 - e * e)
    r_pf = np.array(
        [
            p * math.cos(nu) / (1.0 + e * math.cos(nu)),
            p * math.sin(nu) / (1.0 + e * math.cos(nu)),
            0.0,
        ],
        dtype=float,
    )

    v_pf = np.array(
        [
            -math.sqrt(mu_km3_s2 / p) * math.sin(nu),
            math.sqrt(mu_km3_s2 / p) * (e + math.cos(nu)),
            0.0,
        ],
        dtype=float,
    )

    cO, sO = math.cos(raan), math.sin(raan)
    ci, si = math.cos(inc), math.sin(inc)
    cw, sw = math.cos(argp), math.sin(argp)

    # Perifocal to ECI rotation: R3(raan) * R1(i) * R3(argp)
    R = np.array(
        [
            [cO * cw - sO * sw * ci, -cO * sw - sO * cw * ci, sO * si],
            [sO * cw + cO * sw * ci, -sO * sw + cO * cw * ci, -cO * si],
            [sw * si, cw * si, ci],
        ],
        dtype=float,
    )

    r = R @ r_pf
    v = R @ v_pf
    return CartesianState(r_km=r, v_km_s=v)


def rv_to_coe(mu_km3_s2: float, r_km: np.ndarray, v_km_s: np.ndarray) -> OrbitalElements:
    """Convert ECI state to classical orbital elements (best-effort for near-circular)."""
    r = np.asarray(r_km, dtype=float)
    v = np.asarray(v_km_s, dtype=float)
    R = _norm(r)
    V = _norm(v)
    if R == 0:
        raise ValueError("Position magnitude is zero")

    h = np.cross(r, v)
    hmag = _norm(h)
    if hmag == 0:
        raise ValueError("Angular momentum magnitude is zero")

    k = np.array([0.0, 0.0, 1.0])
    n = np.cross(k, h)
    nmag = _norm(n)

    e_vec = (np.cross(v, h) / mu_km3_s2) - (r / R)
    e = _norm(e_vec)

    energy = V * V / 2.0 - mu_km3_s2 / R
    a = -mu_km3_s2 / (2.0 * energy) if abs(energy) > 1e-12 else float("inf")

    i = math.acos(max(-1.0, min(1.0, h[2] / hmag)))

    if nmag > 1e-12:
        raan = math.atan2(n[1], n[0]) % (2.0 * math.pi)
    else:
        raan = 0.0

    if nmag > 1e-12 and e > 1e-10:
        argp = math.acos(max(-1.0, min(1.0, float(np.dot(n, e_vec) / (nmag * e)))))
        if e_vec[2] < 0:
            argp = (2.0 * math.pi - argp) % (2.0 * math.pi)
    else:
        argp = 0.0

    if e > 1e-10:
        nu = math.acos(max(-1.0, min(1.0, float(np.dot(e_vec, r) / (e * R)))))
        if float(np.dot(r, v)) < 0:
            nu = (2.0 * math.pi - nu) % (2.0 * math.pi)
    else:
        if nmag > 1e-12:
            u = math.acos(max(-1.0, min(1.0, float(np.dot(n, r) / (nmag * R)))))
            if r[2] < 0:
                u = (2.0 * math.pi - u) % (2.0 * math.pi)
            nu = u
        else:
            nu = math.atan2(r[1], r[0]) % (2.0 * math.pi)

    return OrbitalElements(a_km=float(a), e=float(e), i_rad=float(i), raan_rad=float(raan), argp_rad=float(argp), nu_rad=float(nu))


def propagate_kepler_universal(mu_km3_s2: float, r0_km: np.ndarray, v0_km_s: np.ndarray, dt_s: float) -> Tuple[np.ndarray, np.ndarray]:
    """
    Two-body propagation using universal variables (valid for elliptic/hyperbolic).
    Returns (r_km, v_km_s) after dt_s.
    """
    r0 = np.asarray(r0_km, dtype=float)
    v0 = np.asarray(v0_km_s, dtype=float)
    r0mag = _norm(r0)
    if r0mag == 0:
        raise ValueError("Initial position magnitude is zero")

    vr0 = float(np.dot(r0, v0)) / r0mag
    alpha = 2.0 / r0mag - (float(np.dot(v0, v0)) / mu_km3_s2)

    if abs(alpha) > 1e-12:
        chi = math.sqrt(mu_km3_s2) * abs(alpha) * dt_s
    else:
        chi = math.sqrt(mu_km3_s2) * dt_s / r0mag

    sqrt_mu = math.sqrt(mu_km3_s2)
    for _ in range(50):
        z = alpha * chi * chi
        C = _stumpff_C(z)
        S = _stumpff_S(z)
        F = (
            (r0mag * vr0 / sqrt_mu) * chi * chi * C
            + (1.0 - alpha * r0mag) * chi * chi * chi * S
            + r0mag * chi
            - sqrt_mu * dt_s
        )
        dF = (
            (r0mag * vr0 / sqrt_mu) * chi * (1.0 - z * S)
            + (1.0 - alpha * r0mag) * chi * chi * C
            + r0mag
        )
        if abs(dF) < 1e-14:
            break
        delta = F / dF
        chi -= delta
        if abs(delta) < 1e-10:
            break

    z = alpha * chi * chi
    C = _stumpff_C(z)
    S = _stumpff_S(z)

    f = 1.0 - (chi * chi / r0mag) * C
    g = dt_s - (chi * chi * chi / sqrt_mu) * S
    r = f * r0 + g * v0
    rmag = _norm(r)

    fdot = (sqrt_mu / (rmag * r0mag)) * (z * S - 1.0) * chi
    gdot = 1.0 - (chi * chi / rmag) * C
    v = fdot * r0 + gdot * v0
    return r, v


def earth_rotation_rate_rad_s() -> float:
    """WGS-84 Earth rotation rate (rad/s)."""
    return 7.2921150e-5


def eci_to_ecef(r_eci_km: np.ndarray, t_since_epoch_s: float, theta0_rad: float = 0.0) -> np.ndarray:
    r = np.asarray(r_eci_km, dtype=float)
    theta = theta0_rad + earth_rotation_rate_rad_s() * float(t_since_epoch_s)
    c, s = math.cos(theta), math.sin(theta)
    R3 = np.array([[c, s, 0.0], [-s, c, 0.0], [0.0, 0.0, 1.0]], dtype=float)
    return R3 @ r


def ecef_to_geodetic_spherical(r_ecef_km: np.ndarray) -> Tuple[float, float, float]:
    r = np.asarray(r_ecef_km, dtype=float)
    x, y, z = float(r[0]), float(r[1]), float(r[2])
    lon = math.atan2(y, x)
    rho = math.hypot(x, y)
    lat = math.atan2(z, rho)
    rmag = math.sqrt(x * x + y * y + z * z)
    alt = rmag
    return lat, lon, alt


def has_line_of_sight(r1_km: np.ndarray, r2_km: np.ndarray, body_radius_km: float, clearance_km: float = 0.0) -> bool:
    a = np.asarray(r1_km, dtype=float)
    b = np.asarray(r2_km, dtype=float)
    d = b - a
    dd = float(np.dot(d, d))
    if dd == 0.0:
        return _norm(a) > (body_radius_km + clearance_km)
    t = -float(np.dot(a, d)) / dd
    t = max(0.0, min(1.0, t))
    closest = a + t * d
    return _norm(closest) > (body_radius_km + clearance_km)


def is_visible_from_station_ecef(station_ecef_km: np.ndarray, sat_ecef_km: np.ndarray, earth_radius_km: float, min_elev_deg: float) -> bool:
    from ground import elevation_angle_rad  # local import to avoid cycles

    el = elevation_angle_rad(station_ecef_km, sat_ecef_km)
    if el < math.radians(float(min_elev_deg)):
        return False
    return has_line_of_sight(station_ecef_km, sat_ecef_km, body_radius_km=float(earth_radius_km), clearance_km=0.0)

