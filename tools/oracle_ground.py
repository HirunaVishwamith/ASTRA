"""Dump ground<->satellite link reference from the Python implementation.

Per epoch: propagate, compute satellite ECEF at sim_time=step*dt, then run the
exact main.py ground-link construction (elevation mask + range + LOS, budget
ref=800, +3 ms latency). Emit sat ECI positions, the ground links, and the
best (highest-elevation) sat per station. test_ground.c reproduces + compares.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from physics.orbit import (coe_to_rv, OrbitalElements, propagate_kepler_universal,
                           eci_to_ecef, is_visible_from_station_ecef)
from ground import GroundStation
from link_budget import inverse_square_budget

MU = 398600.4418
EARTH_R = 6378.137
SMA = EARTH_R + 550.0
ECC = 0.001
INC = math.radians(53.0)
NP_, NS_ = 10, 10
DT = 5.0
# Epochs chosen to have ground passes (constellation coverage of these 5
# stations is sparse; many epochs have no visible satellite).
EPOCH_STEPS = [117, 226, 345]
NUM_SATS = NP_ * NS_

GROUND = [("Sri Lanka", 7.8731, 80.7718), ("Singapore", 1.3521, 103.8198),
          ("London", 51.5074, -0.1278), ("New York", 40.7128, -74.0060),
          ("Tokyo", 35.6762, 139.6503)]
GS_MAX_RANGE = 3000.0
GS_BW = 300.0
GS_LOSS = 0.002
MIN_ELEV = 10.0
SPEED_C = 299792.458


def main():
    states = []
    for plane in range(NP_):
        for s in range(NS_):
            raan = (2 * math.pi / NP_) * plane
            nu = (2 * math.pi / NS_) * s + plane * 0.1
            st = coe_to_rv(MU, OrbitalElements(SMA, ECC, INC, raan, 0.0, nu))
            states.append([st.r_km.copy(), st.v_km_s.copy()])
    ids = list(range(NUM_SATS))
    stations = [GroundStation(gid=NUM_SATS + i, name=nm,
                              lat_rad=math.radians(la), lon_rad=math.radians(lo),
                              alt_km=0.0, min_elev_deg=MIN_ELEV)
                for i, (nm, la, lo) in enumerate(GROUND)]

    lines = []
    step = 0
    for target in EPOCH_STEPS:
        while step < target:
            for s in states:
                s[0], s[1] = propagate_kepler_universal(MU, s[0], s[1], DT)
            step += 1
        sim_time = step * DT
        lines.append("EPOCH %d" % step)
        for i in ids:
            r = states[i][0]
            lines.append("POS %d %.17g %.17g %.17g" % (i, r[0], r[1], r[2]))

        sat_ecef = {i: eci_to_ecef(states[i][0], sim_time, theta0_rad=0.0) for i in ids}
        glinks = []
        best_lines = []
        for gs in stations:
            gse = gs.ecef_position_km(EARTH_R)
            up = gse / float(np.linalg.norm(gse))
            best = None
            best_el = -1e9
            for sid in ids:
                se = sat_ecef[sid]
                if not is_visible_from_station_ecef(gse, se, EARTH_R, gs.min_elev_deg):
                    continue
                d = float(np.linalg.norm(se - gse))
                if d > GS_MAX_RANGE:
                    continue
                rho = se - gse
                el_sin = float(np.dot(rho / max(1e-9, float(np.linalg.norm(rho))), up))
                if el_sin > best_el:
                    best_el = el_sin
                    best = sid
                lb = inverse_square_budget(d, GS_BW, GS_LOSS, ref_km=800.0)
                lat = d / SPEED_C + 0.003
                glinks.append((gs.gid, sid, d, lat, lb.bandwidth_mbps, lb.loss_prob))
            best_lines.append("BEST %d %d" % (gs.gid, -1 if best is None else best))
        glinks.sort()
        lines.append("GLINKS %d" % len(glinks))
        for gid, sid, d, lat, bw, loss in glinks:
            lines.append("GL %d %d %.17g %.17g %.17g %.17g" % (gid, sid, d, lat, bw, loss))
        lines.extend(best_lines)

    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ground_vectors.txt")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote %d epochs -> %s" % (len(EPOCH_STEPS), path))


if __name__ == "__main__":
    main()
