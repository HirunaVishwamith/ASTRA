"""
Routing layer: pluggable routing strategies over the current NetworkGraph.

Implements:
- Dijkstra shortest path (centralized per step)
- Distance-vector (iterative, distributed-style)
"""

from __future__ import annotations

from dataclasses import dataclass
import heapq
from typing import Dict, List, Optional, Tuple

from network_model import NetworkGraph


@dataclass(frozen=True)
class RouteDecision:
    next_hop: Optional[int]
    cost: float


class Router:
    name: str = "base"

    def step(self, graph: NetworkGraph, active_ids: List[int]) -> None:
        """Advance internal routing state one simulation step."""
        raise NotImplementedError

    def next_hop(self, src: int, dst: int) -> Optional[int]:
        raise NotImplementedError

    def path(self, src: int, dst: int, max_hops: int = 256) -> List[int]:
        if src == dst:
            return [src]
        p = [src]
        cur = src
        seen = {src}
        for _ in range(max_hops):
            nh = self.next_hop(cur, dst)
            if nh is None or nh in seen:
                break
            p.append(nh)
            if nh == dst:
                return p
            seen.add(nh)
            cur = nh
        return []


class DijkstraRouter(Router):
    name = "dijkstra"

    def __init__(self, weight: str = "latency"):
        self.weight = weight  # "latency" or "hops"
        # table[src][dst] = next_hop
        self.table: Dict[int, Dict[int, RouteDecision]] = {}

    def _edge_weight(self, link) -> float:
        if self.weight == "hops":
            return 1.0
        return float(link.props.latency_s)

    def step(self, graph: NetworkGraph, active_ids: List[int]) -> None:
        self.table = {}
        active_set = set(active_ids)
        for src in active_ids:
            dist: Dict[int, float] = {src: 0.0}
            prev: Dict[int, int] = {}
            pq: List[Tuple[float, int]] = [(0.0, src)]
            while pq:
                d, u = heapq.heappop(pq)
                if d != dist.get(u, float("inf")):
                    continue
                for v, link in graph.neighbors(u).items():
                    if v not in active_set:
                        continue
                    w = self._edge_weight(link)
                    nd = d + w
                    if nd < dist.get(v, float("inf")):
                        dist[v] = nd
                        prev[v] = u
                        heapq.heappush(pq, (nd, v))

            # build next-hop decisions
            self.table[src] = {}
            for dst in active_ids:
                if dst == src:
                    self.table[src][dst] = RouteDecision(next_hop=src, cost=0.0)
                    continue
                if dst not in dist:
                    continue
                # walk back one step from dst to src to find next hop
                cur = dst
                while cur in prev and prev[cur] != src:
                    cur = prev[cur]
                nh = cur if cur in prev and prev[cur] == src else (cur if cur == dst and prev.get(cur) == src else None)
                if nh is None:
                    # direct neighbor case
                    if prev.get(dst) == src:
                        nh = dst
                self.table[src][dst] = RouteDecision(next_hop=nh, cost=float(dist[dst]))

    def next_hop(self, src: int, dst: int) -> Optional[int]:
        return self.table.get(src, {}).get(dst, RouteDecision(None, float("inf"))).next_hop


class DistanceVectorRouter(Router):
    """
    Distance-vector style routing. Each node maintains a (cost,next_hop) table.
    We run a few synchronous "rounds" each simulation step to converge partially.
    """

    name = "distance_vector"

    def __init__(self, cost: str = "hops", rounds_per_step: int = 3):
        self.cost = cost  # "hops" or "latency"
        self.rounds_per_step = rounds_per_step
        self.table: Dict[int, Dict[int, RouteDecision]] = {}

    def _link_cost(self, link) -> float:
        if self.cost == "latency":
            return float(link.props.latency_s)
        return 1.0

    def step(self, graph: NetworkGraph, active_ids: List[int]) -> None:
        active_set = set(active_ids)

        # initialize missing tables
        for u in active_ids:
            if u not in self.table:
                self.table[u] = {u: RouteDecision(next_hop=u, cost=0.0)}

        # purge inactive nodes
        for u in list(self.table.keys()):
            if u not in active_set:
                del self.table[u]

        # synchronous rounds
        for _ in range(self.rounds_per_step):
            new_tables: Dict[int, Dict[int, RouteDecision]] = {}
            for u in active_ids:
                # start with self-route
                cur = dict(self.table.get(u, {}))
                cur[u] = RouteDecision(next_hop=u, cost=0.0)

                # Remove destinations that go via now-missing next-hop or inactive
                for dst, rd in list(cur.items()):
                    if dst not in active_set:
                        del cur[dst]
                        continue
                    if rd.next_hop is not None and rd.next_hop != u and not graph.has_link(u, rd.next_hop):
                        del cur[dst]

                # Bellman-Ford update from neighbors
                for v, link in graph.neighbors(u).items():
                    if v not in active_set:
                        continue
                    lc = self._link_cost(link)
                    vtab = self.table.get(v, {})
                    for dst, vrd in vtab.items():
                        if dst not in active_set:
                            continue
                        nd = vrd.cost + lc
                        best = cur.get(dst)
                        if best is None or nd < best.cost:
                            cur[dst] = RouteDecision(next_hop=v, cost=float(nd))

                new_tables[u] = cur

            self.table = new_tables

    def next_hop(self, src: int, dst: int) -> Optional[int]:
        return self.table.get(src, {}).get(dst, RouteDecision(None, float("inf"))).next_hop

