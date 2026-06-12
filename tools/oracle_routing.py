"""Dump Dijkstra all-pairs routing reference from the Python implementation.

Per epoch: propagate, build ISL topology, run DijkstraRouter(weight=latency),
then emit positions + every (src,dst) next-hop and path cost. test_routing.c
rebuilds and compares next-hop tables (bit-exact) and costs.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from physics.orbit import coe_to_rv, OrbitalElements, propagate_kepler_universal
from network.graph import NetworkGraph, build_topology
from link_budget import inverse_square_budget
from routing.algorithms import DijkstraRouter

MU = 398600.4418
EARTH_R = 6378.137
SMA = EARTH_R + 550.0
ECC = 0.001
INC = math.radians(53.0)
NP_, NS_ = 10, 10
MAXR = 2500.0
DT = 5.0
EPOCH_STEPS = [0, 50, 200]


def main():
    states = []
    for plane in range(NP_):
        for s in range(NS_):
            raan = (2 * math.pi / NP_) * plane
            nu = (2 * math.pi / NS_) * s + plane * 0.1
            st = coe_to_rv(MU, OrbitalElements(SMA, ECC, INC, raan, 0.0, nu))
            states.append([st.r_km.copy(), st.v_km_s.copy()])
    ids = list(range(NP_ * NS_))

    lines = []
    step = 0
    for target in EPOCH_STEPS:
        while step < target:
            for s in states:
                s[0], s[1] = propagate_kepler_universal(MU, s[0], s[1], DT)
            step += 1
        lines.append("EPOCH %d" % step)
        for i in ids:
            r = states[i][0]
            lines.append("POS %d %.17g %.17g %.17g" % (i, r[0], r[1], r[2]))

        g = NetworkGraph()
        build_topology(g, active_ids=ids, r_eci_by_id={i: states[i][0] for i in ids},
                       earth_radius_km=EARTH_R, max_range_km=MAXR, clearance_km=0.0,
                       base_bandwidth_mbps=2000.0, base_loss_prob=0.0005, extra_latency_s=0.001,
                       link_budget=lambda d, bw, loss: inverse_square_budget(d, bw, loss, ref_km=500.0))
        router = DijkstraRouter(weight="latency")
        router.step(g, ids)

        lines.append("ROUTES %d" % (len(ids) * len(ids)))
        for src in ids:
            row = router.table.get(src, {})
            for dst in ids:
                rd = row.get(dst)
                if rd is None or rd.next_hop is None:
                    lines.append("R %d %d -1 inf" % (src, dst))
                else:
                    lines.append("R %d %d %d %.17g" % (src, dst, rd.next_hop, rd.cost))

    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "routing_vectors.txt")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote %d epochs -> %s" % (len(EPOCH_STEPS), path))


if __name__ == "__main__":
    main()
