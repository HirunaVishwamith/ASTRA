"""
Network model: builds a time-varying graph from satellite states.

Separates:
- "where satellites are" (physics / ECI states) from
- "who can talk to whom" (links with properties).

Units:
- Distance: km
- Latency: s
"""

from __future__ import annotations

from dataclasses import dataclass
import math
from typing import Callable, Dict, Iterable, List, Tuple

import numpy as np

from orbit import has_line_of_sight


SPEED_OF_LIGHT_KM_S = 299_792.458  # vacuum, good enough for ISL propagation delay


@dataclass(frozen=True)
class LinkProperties:
    latency_s: float
    bandwidth_mbps: float
    loss_prob: float  # 0..1


@dataclass(frozen=True)
class Link:
    a: int
    b: int
    distance_km: float
    props: LinkProperties


class NetworkGraph:
    """
    Undirected graph.
    adjacency[u][v] = Link
    """

    def __init__(self):
        self.adjacency: Dict[int, Dict[int, Link]] = {}

    def clear(self) -> None:
        self.adjacency.clear()

    def add_undirected(self, u: int, v: int, link: Link) -> None:
        self.adjacency.setdefault(u, {})[v] = link
        self.adjacency.setdefault(v, {})[u] = Link(a=link.b, b=link.a, distance_km=link.distance_km, props=link.props)

    def neighbors(self, u: int) -> Dict[int, Link]:
        return self.adjacency.get(u, {})

    def has_link(self, u: int, v: int) -> bool:
        return v in self.adjacency.get(u, {})

    def get_link(self, u: int, v: int) -> Link | None:
        return self.adjacency.get(u, {}).get(v)

    def edges(self) -> Iterable[Tuple[int, int, Link]]:
        seen = set()
        for u, nbrs in self.adjacency.items():
            for v, link in nbrs.items():
                key = (min(u, v), max(u, v))
                if key in seen:
                    continue
                seen.add(key)
                yield u, v, link


def build_topology(
    graph: NetworkGraph,
    *,
    active_ids: List[int],
    r_eci_by_id: Dict[int, np.ndarray],
    earth_radius_km: float,
    max_range_km: float,
    clearance_km: float = 0.0,
    base_bandwidth_mbps: float = 2000.0,
    base_loss_prob: float = 0.0005,
    extra_latency_s: float = 0.001,
    link_budget: Callable[[float, float, float], LinkProperties] | None = None,
) -> None:
    """
    Recompute topology.
    - LOS + range constrained
    - latency from distance / c + extra_latency_s
    - bandwidth & loss can be constant knobs or computed by a link_budget(distance_km, base_bw, base_loss)
    """
    graph.clear()
    for idx_i in range(len(active_ids)):
        a = active_ids[idx_i]
        ra = r_eci_by_id[a]
        for idx_j in range(idx_i + 1, len(active_ids)):
            b = active_ids[idx_j]
            rb = r_eci_by_id[b]
            d_km = float(np.linalg.norm(ra - rb))
            if d_km > max_range_km:
                continue
            if not has_line_of_sight(ra, rb, body_radius_km=earth_radius_km, clearance_km=clearance_km):
                continue

            latency = d_km / SPEED_OF_LIGHT_KM_S + extra_latency_s
            if link_budget is None:
                props = LinkProperties(
                    latency_s=float(latency), bandwidth_mbps=float(base_bandwidth_mbps), loss_prob=float(base_loss_prob)
                )
            else:
                lb = link_budget(float(d_km), float(base_bandwidth_mbps), float(base_loss_prob))
                props = LinkProperties(latency_s=float(latency), bandwidth_mbps=float(lb.bandwidth_mbps), loss_prob=float(lb.loss_prob))
            link = Link(a=a, b=b, distance_km=d_km, props=props)
            graph.add_undirected(a, b, link)

