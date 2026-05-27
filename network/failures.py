"""
Failure and impairment models.
"""

from __future__ import annotations

from dataclasses import dataclass
import random

from network_model import NetworkGraph, Link, LinkProperties


@dataclass
class LinkImpairments:
    blackout_prob_per_s: float = 0.0  # probability a given link is down each second (Bernoulli per step)
    latency_spike_prob_per_s: float = 0.0
    latency_spike_add_s: float = 0.05
    loss_multiplier: float = 1.0


class FailureModel:
    """
    Stateless step-wise impairment application.
    For research-grade work later, you can make these stateful (Markov).
    """

    def __init__(self, impair: LinkImpairments):
        self.impair = impair

    def apply(self, graph: NetworkGraph, dt_s: float) -> None:
        p_black = 1.0 - (1.0 - self.impair.blackout_prob_per_s) ** float(max(0.0, dt_s))
        p_spike = 1.0 - (1.0 - self.impair.latency_spike_prob_per_s) ** float(max(0.0, dt_s))

        to_remove = []
        for u, v, link in list(graph.edges()):
            if self.impair.blackout_prob_per_s > 0 and random.random() < p_black:
                to_remove.append((u, v))
                continue

            add_lat = self.impair.latency_spike_add_s if (self.impair.latency_spike_prob_per_s > 0 and random.random() < p_spike) else 0.0
            loss = min(1.0, max(0.0, link.props.loss_prob * self.impair.loss_multiplier))
            props = LinkProperties(
                latency_s=float(link.props.latency_s + add_lat),
                bandwidth_mbps=float(link.props.bandwidth_mbps),
                loss_prob=float(loss),
            )
            graph.adjacency[u][v] = Link(a=u, b=v, distance_km=link.distance_km, props=props)
            graph.adjacency[v][u] = Link(a=v, b=u, distance_km=link.distance_km, props=props)

        for u, v in to_remove:
            if v in graph.adjacency.get(u, {}):
                del graph.adjacency[u][v]
            if u in graph.adjacency.get(v, {}):
                del graph.adjacency[v][u]

