"""
Metrics collection for experiments and dashboard.
"""

from __future__ import annotations

from dataclasses import dataclass
import random
from typing import Dict, List, Optional

from network_model import NetworkGraph
from routing import Router
from traffic import TrafficStats


@dataclass
class StepMetrics:
    time_s: float
    avg_path_len_hops: float
    delivery_ratio: float
    avg_delay_s: float
    avg_hops: float
    avg_link_utilization: float
    route_updates: int
    convergence_s: Optional[float]


class MetricsCollector:
    def __init__(self, sample_pairs: int = 40):
        self.sample_pairs = int(sample_pairs)
        self._last_router_snapshot: Dict[int, Dict[int, int]] | None = None
        self._failure_time_s: Optional[float] = None
        self._converged_at_s: Optional[float] = None

    def notify_failure(self, now_s: float) -> None:
        self._failure_time_s = float(now_s)
        self._converged_at_s = None

    def _router_signature(self, router: Router, active_ids: List[int], dst_samples: int = 10) -> Dict[int, Dict[int, int]]:
        sig: Dict[int, Dict[int, int]] = {}
        if not active_ids:
            return sig
        for src in active_ids:
            dsts = random.sample(active_ids, min(dst_samples, len(active_ids)))
            row: Dict[int, int] = {}
            for dst in dsts:
                nh = router.next_hop(src, dst)
                row[dst] = -1 if nh is None else int(nh)
            sig[int(src)] = row
        return sig

    def route_update_count(self, router: Router, active_ids: List[int]) -> int:
        sig = self._router_signature(router, active_ids)
        if self._last_router_snapshot is None:
            self._last_router_snapshot = sig
            return 0
        updates = 0
        prev = self._last_router_snapshot
        for src, row in sig.items():
            prow = prev.get(src, {})
            for dst, nh in row.items():
                if prow.get(dst) != nh:
                    updates += 1
        self._last_router_snapshot = sig
        return int(updates)

    def avg_path_length(self, router: Router, active_ids: List[int]) -> float:
        if len(active_ids) < 2:
            return 0.0
        pairs = min(self.sample_pairs, len(active_ids) * (len(active_ids) - 1) // 2)
        if pairs <= 0:
            return 0.0
        total = 0
        count = 0
        for _ in range(pairs):
            a, b = random.sample(active_ids, 2)
            p = router.path(a, b, max_hops=256)
            if p:
                total += (len(p) - 1)
                count += 1
        return float(total / max(1, count))

    def convergence_time(self, now_s: float, route_updates: int) -> Optional[float]:
        if self._failure_time_s is None:
            return None
        if self._converged_at_s is None and route_updates == 0:
            self._converged_at_s = float(now_s)
        if self._converged_at_s is None:
            return None
        return float(self._converged_at_s - self._failure_time_s)

    def step(
        self,
        *,
        graph: NetworkGraph,
        router: Router,
        active_ids: List[int],
        traffic_stats: TrafficStats,
        now_s: float,
    ) -> StepMetrics:
        route_updates = self.route_update_count(router, active_ids)
        avg_path = self.avg_path_length(router, active_ids)
        delivered = max(0, traffic_stats.delivered)
        dropped = max(0, traffic_stats.dropped)
        denom = delivered + dropped
        delivery_ratio = float(delivered / denom) if denom > 0 else 0.0
        conv = self.convergence_time(now_s, route_updates)
        return StepMetrics(
            time_s=float(now_s),
            avg_path_len_hops=float(avg_path),
            delivery_ratio=float(delivery_ratio),
            avg_delay_s=float(traffic_stats.avg_delay_s),
            avg_hops=float(traffic_stats.avg_hops),
            avg_link_utilization=float(getattr(traffic_stats, "avg_link_utilization", 0.0)),
            route_updates=int(route_updates),
            convergence_s=conv,
        )

