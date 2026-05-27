"""
Traffic simulation: generate packets and forward them across the current graph.

This is a discrete-event layer running on top of the dynamic topology + router.
It tracks delivery ratio, delay, hops, and drops due to missing routes/links or loss.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import random
from typing import Dict, List, Optional, Tuple

from network_model import NetworkGraph
from routing import Router


@dataclass
class Packet:
    pid: int
    src: int
    dst: int
    created_t: float
    ttl_hops: int = 32
    hops: int = 0
    last_node: int = -1
    delivered_t: Optional[float] = None
    dropped_t: Optional[float] = None
    drop_reason: Optional[str] = None
    path: List[int] = field(default_factory=list)


@dataclass
class InTransit:
    pid: int
    u: int
    v: int
    arrival_t: float


@dataclass
class TrafficStats:
    generated: int = 0
    delivered: int = 0
    dropped: int = 0
    in_flight: int = 0
    avg_delay_s: float = 0.0
    avg_hops: float = 0.0


class TrafficGenerator:
    """
    Generates flows according to patterns.
    rate_pps: packets per second (approx; we sample each step).
    """

    def __init__(self, pattern: str = "uniform", rate_pps: float = 2.0, hotspot_id: int = 0):
        self.pattern = pattern  # uniform | hotspot | burst
        self.rate_pps = float(rate_pps)
        self.hotspot_id = int(hotspot_id)
        self._burst_left = 0

    def sample_packets(self, active_ids: List[int], dt_s: float) -> List[Tuple[int, int]]:
        if len(active_ids) < 2:
            return []
        n_expect = self.rate_pps * dt_s
        # Poisson-ish approximation: generate floor + Bernoulli remainder
        n = int(n_expect)
        if random.random() < (n_expect - n):
            n += 1

        pairs: List[Tuple[int, int]] = []
        if self.pattern == "burst":
            if self._burst_left <= 0 and random.random() < 0.03:
                self._burst_left = random.randint(20, 80)
            if self._burst_left > 0:
                n += min(self._burst_left, 50)
                self._burst_left -= n

        for _ in range(max(0, n)):
            if self.pattern == "hotspot":
                # Many-to-one / one-to-many around a hotspot node
                if random.random() < 0.5:
                    src = random.choice(active_ids)
                    dst = self.hotspot_id if self.hotspot_id in active_ids else random.choice(active_ids)
                else:
                    src = self.hotspot_id if self.hotspot_id in active_ids else random.choice(active_ids)
                    dst = random.choice(active_ids)
                if src == dst:
                    continue
            else:
                src, dst = random.sample(active_ids, 2)
            pairs.append((src, dst))
        return pairs


class TrafficSimulator:
    def __init__(self, generator: TrafficGenerator):
        self.generator = generator
        self._next_pid = 1
        self.packets: Dict[int, Packet] = {}
        self.in_transit: List[InTransit] = []

    def step(self, *, graph: NetworkGraph, router: Router, active_ids: List[int], now_t: float, dt_s: float) -> TrafficStats:
        active_set = set(active_ids)

        # 1) deliver anything that arrived
        arrived: List[InTransit] = []
        remaining: List[InTransit] = []
        for item in self.in_transit:
            if item.arrival_t <= now_t:
                arrived.append(item)
            else:
                remaining.append(item)
        self.in_transit = remaining

        for item in arrived:
            pkt = self.packets.get(item.pid)
            if pkt is None or pkt.delivered_t is not None or pkt.dropped_t is not None:
                continue
            pkt.last_node = item.v
            pkt.path.append(item.v)
            if item.v == pkt.dst:
                pkt.delivered_t = now_t

        # 2) generate new packets
        for src, dst in self.generator.sample_packets(active_ids, dt_s):
            pid = self._next_pid
            self._next_pid += 1
            pkt = Packet(pid=pid, src=src, dst=dst, created_t=now_t, last_node=src, path=[src])
            self.packets[pid] = pkt

        # 3) forward packets at nodes (simple model: one hop per step per packet if possible)
        # Only consider packets that are currently "at a node" (not in transit).
        in_transit_pids = {it.pid for it in self.in_transit}
        for pkt in list(self.packets.values()):
            if pkt.delivered_t is not None or pkt.dropped_t is not None:
                continue
            if pkt.pid in in_transit_pids:
                continue
            if pkt.last_node not in active_set:
                pkt.dropped_t = now_t
                pkt.drop_reason = "source_node_inactive"
                continue
            if pkt.ttl_hops <= 0:
                pkt.dropped_t = now_t
                pkt.drop_reason = "ttl_exceeded"
                continue
            if pkt.last_node == pkt.dst:
                pkt.delivered_t = now_t
                continue

            nh = router.next_hop(pkt.last_node, pkt.dst)
            if nh is None:
                pkt.dropped_t = now_t
                pkt.drop_reason = "no_route"
                continue
            link = graph.get_link(pkt.last_node, nh)
            if link is None:
                pkt.dropped_t = now_t
                pkt.drop_reason = "route_broken"
                continue
            if random.random() < link.props.loss_prob:
                pkt.dropped_t = now_t
                pkt.drop_reason = "link_loss"
                continue

            pkt.ttl_hops -= 1
            pkt.hops += 1
            self.in_transit.append(InTransit(pid=pkt.pid, u=pkt.last_node, v=nh, arrival_t=now_t + link.props.latency_s))

        # 4) compute stats snapshot
        stats = TrafficStats()
        stats.generated = sum(1 for p in self.packets.values() if p.created_t >= now_t - dt_s - 1e-9)
        delivered = [p for p in self.packets.values() if p.delivered_t is not None]
        dropped = [p for p in self.packets.values() if p.dropped_t is not None]
        stats.delivered = len(delivered)
        stats.dropped = len(dropped)
        stats.in_flight = sum(1 for p in self.packets.values() if p.delivered_t is None and p.dropped_t is None)

        if delivered:
            delays = [(p.delivered_t - p.created_t) for p in delivered if p.delivered_t is not None]
            stats.avg_delay_s = float(sum(delays) / max(1, len(delays)))
            stats.avg_hops = float(sum(p.hops for p in delivered) / max(1, len(delivered)))
        return stats

