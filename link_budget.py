"""
Simple distance-based link budget models.

These are not RF/optical-accurate yet; they give realistic *behavior*:
- Links at longer range have higher loss and lower throughput.
- Close links are fast and reliable.
"""

from __future__ import annotations

from network_model import LinkProperties


def inverse_square_budget(distance_km: float, base_bw_mbps: float, base_loss: float, ref_km: float = 500.0) -> LinkProperties:
    """
    Heuristic budget:
    - bandwidth scales ~ (ref/d)^2, clipped to [0.05, 1.0] of base
    - loss increases ~ (d/ref)^2, clipped
    """
    d = max(1.0, float(distance_km))
    scale = (float(ref_km) / d) ** 2
    bw = float(base_bw_mbps) * max(0.05, min(1.0, scale))
    loss = min(0.2, max(0.0, float(base_loss) * max(1.0, 1.0 / max(1e-9, scale))))
    return LinkProperties(latency_s=0.0, bandwidth_mbps=bw, loss_prob=loss)

