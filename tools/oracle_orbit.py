"""Dump orbit reference vectors from the Python implementation.

Builds the exact default constellation (main.py geometry), then for each
satellite emits its COE, the coe_to_rv state, and the state after propagating
100 steps of 5 s. tests/test_orbit.c reproduces this in C and compares.
"""
import math
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from physics.orbit import coe_to_rv, OrbitalElements, propagate_kepler_universal

MU = 398600.4418
EARTH_R = 6378.137
SMA = EARTH_R + 550.0
ECC = 0.001
INC = math.radians(53.0)
NUM_PLANES = 10
NUM_SATS = 10
DT = 5.0
STEPS = 100


def main():
    out = []
    for plane in range(NUM_PLANES):
        for sat_idx in range(NUM_SATS):
            raan = (2 * math.pi / NUM_PLANES) * plane
            nu = (2 * math.pi / NUM_SATS) * sat_idx + plane * 0.1
            coe = OrbitalElements(a_km=SMA, e=ECC, i_rad=INC,
                                  raan_rad=raan, argp_rad=0.0, nu_rad=nu)
            st = coe_to_rv(MU, coe)
            r, v = st.r_km.copy(), st.v_km_s.copy()
            out.append("COE %.17g %.17g %.17g %.17g %.17g %.17g" %
                       (SMA, ECC, INC, raan, 0.0, nu))
            out.append("RV %.17g %.17g %.17g %.17g %.17g %.17g" %
                       (r[0], r[1], r[2], v[0], v[1], v[2]))
            for _ in range(STEPS):
                r, v = propagate_kepler_universal(MU, r, v, DT)
            out.append("PROP %.17g %.17g %.17g %.17g %.17g %.17g" %
                       (r[0], r[1], r[2], v[0], v[1], v[2]))
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "orbit_vectors.txt")
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")
    print("wrote %d satellites -> %s" % (NUM_PLANES * NUM_SATS, path))


if __name__ == "__main__":
    main()
