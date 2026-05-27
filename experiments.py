"""
Headless experiment runner.

Run:
  python experiments.py

This runs the simulation loop without the UI and writes CSV metrics.
"""

from __future__ import annotations

import math
import numpy as np

from orbit import OrbitalElements, coe_to_rv, propagate_kepler_universal
from network_model import NetworkGraph, build_topology, Link, LinkProperties, SPEED_OF_LIGHT_KM_S
from routing import DijkstraRouter, DistanceVectorRouter
from traffic import TrafficGenerator, TrafficSimulator
from failures import FailureModel, LinkImpairments
from ground import GroundStation
from orbit import eci_to_ecef, is_visible_from_station_ecef
from link_budget import inverse_square_budget
from logging_utils import CSVRunLogger


EARTH_RADIUS_KM = 6378.137
MU_EARTH = 398600.4418


def run(
    *,
    steps: int = 600,
    dt_s: float = 5.0,
    num_planes: int = 10,
    sats_per_plane: int = 10,
    inclination_deg: float = 53.0,
    altitude_km: float = 550.0,
    max_isl_range_km: float = 2500.0,
    router_mode: str = "distance_vector",
    routing_cost: str = "hops",
    traffic_pattern: str = "uniform",
    traffic_rate_pps: float = 3.0,
    out_csv: str = "runs/experiment.csv",
) -> None:
    n_sats = num_planes * sats_per_plane
    a_km = EARTH_RADIUS_KM + altitude_km
    inc = math.radians(inclination_deg)

    # init sat states
    states = {}
    coes = {}
    for plane in range(num_planes):
        raan = (2 * math.pi / num_planes) * plane
        for sidx in range(sats_per_plane):
            sid = plane * sats_per_plane + sidx
            nu = (2 * math.pi / sats_per_plane) * sidx + (plane * 0.1)
            coe = OrbitalElements(a_km=a_km, e=0.001, i_rad=inc, raan_rad=raan, argp_rad=0.0, nu_rad=nu)
            st = coe_to_rv(MU_EARTH, coe)
            states[sid] = st
            coes[sid] = coe

    # ground stations (same defaults)
    ground = [
        GroundStation(gid=n_sats + 0, name="Sri Lanka", lat_rad=math.radians(7.8731), lon_rad=math.radians(80.7718)),
        GroundStation(gid=n_sats + 1, name="Singapore", lat_rad=math.radians(1.3521), lon_rad=math.radians(103.8198)),
        GroundStation(gid=n_sats + 2, name="London", lat_rad=math.radians(51.5074), lon_rad=math.radians(-0.1278)),
    ]
    ground_ids = [g.gid for g in ground]

    graph = NetworkGraph()
    router = DijkstraRouter(weight=routing_cost) if router_mode == "dijkstra" else DistanceVectorRouter(cost=routing_cost)
    traffic = TrafficSimulator(TrafficGenerator(pattern=traffic_pattern, rate_pps=traffic_rate_pps, hotspot_id=ground_ids[0]))
    failure_model = FailureModel(LinkImpairments())
    logger = CSVRunLogger(out_csv)

    for k in range(steps):
        t = (k + 1) * dt_s

        # physics
        for sid in range(n_sats):
            st = states[sid]
            r, v = propagate_kepler_universal(MU_EARTH, st.r_km, st.v_km_s, dt_s)
            st.r_km = r
            st.v_km_s = v

        active_sat_ids = list(range(n_sats))
        active_all = active_sat_ids + ground_ids
        r_eci_by_id = {sid: states[sid].r_km for sid in active_sat_ids}

        build_topology(
            graph,
            active_ids=active_sat_ids,
            r_eci_by_id=r_eci_by_id,
            earth_radius_km=EARTH_RADIUS_KM,
            max_range_km=max_isl_range_km,
            base_bandwidth_mbps=2000.0,
            base_loss_prob=0.0005,
            extra_latency_s=0.001,
            link_budget=lambda d_km, bw, loss: inverse_square_budget(d_km, bw, loss, ref_km=500.0),
        )

        # ground links
        sat_ecef = {sid: eci_to_ecef(states[sid].r_km, t, 0.0) for sid in active_sat_ids}
        for gs in ground:
            gs_ecef = gs.ecef_position_km(EARTH_RADIUS_KM)
            for sid in active_sat_ids:
                if not is_visible_from_station_ecef(gs_ecef, sat_ecef[sid], EARTH_RADIUS_KM, gs.min_elev_deg):
                    continue
                d_km = float(np.linalg.norm(sat_ecef[sid] - gs_ecef))
                if d_km > 3000.0:
                    continue
                latency = d_km / SPEED_OF_LIGHT_KM_S + 0.003
                lb = inverse_square_budget(d_km, 300.0, 0.002, ref_km=800.0)
                props = LinkProperties(latency_s=float(latency), bandwidth_mbps=float(lb.bandwidth_mbps), loss_prob=float(lb.loss_prob))
                graph.add_undirected(gs.gid, sid, Link(a=gs.gid, b=sid, distance_km=d_km, props=props))

        failure_model.apply(graph, dt_s=dt_s)
        router.step(graph, active_all)
        stats = traffic.step(graph=graph, router=router, active_ids=active_all, now_t=t, dt_s=dt_s)

        logger.log_row(
            {
                "time_s": float(t),
                "nodes_total": int(len(active_all)),
                "links_total": int(sum(1 for _ in graph.edges())),
                "router": str(router.name),
                "delivered_total": int(stats.delivered),
                "dropped_total": int(stats.dropped),
                "in_flight": int(stats.in_flight),
                "avg_delay_s": float(stats.avg_delay_s),
                "avg_hops": float(stats.avg_hops),
            }
        )

    logger.close()


if __name__ == "__main__":
    run()

