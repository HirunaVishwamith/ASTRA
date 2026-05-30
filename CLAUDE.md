# CLAUDE.md — ASTRA Project Guide

## What This Is

ASTRA (Autonomous Satellite Traffic & Routing Architecture) is a real-time LEO satellite constellation simulator built in Python with PyQt6 + pyqtgraph OpenGL. It is a PhD research project targeting open-source publication, aiming for SpaceX/Starlink-grade physical realism.

The simulator runs a 100-satellite constellation (10 planes × 10 sats) at 550 km LEO, 53° inclination (Starlink-like). Everything runs in a 16 ms GUI loop.

---

## Architecture

### Module Map

```
main.py               — PyQt6 MainWindow + simulation orchestration loop (~940 lines)
physics/orbit.py      — All orbital mechanics (canonical source)
orbit.py              — Re-export shim: `from physics.orbit import *`
network/graph.py      — NetworkGraph, Link, LinkProperties, build_topology()
network_model.py      — Re-export shim: `from network.graph import *`
network/failures.py   — FailureModel, LinkImpairments
failures.py           — Re-export shim
routing/algorithms.py — DijkstraRouter, DistanceVectorRouter, Router base class
routing.py            — Re-export shim
traffic/sim.py        — Packet, TrafficGenerator, TrafficSimulator, TrafficStats
traffic.py            — Re-export shim
metrics/collector.py  — MetricsCollector, StepMetrics
metrics.py            — Re-export shim
ground.py             — GroundStation dataclass, elevation_angle_rad(), ecef_to_eci()
link_budget.py        — inverse_square_budget() (heuristic ISL/ground link model)
logging_utils.py      — CSVRunLogger (optional CSV export, default off)
experiments.py        — (stub for batch experiment runs)
run.py                — Launcher shim
img/                  — earth_texture.jpg, interface screenshots
```

### The Shim Pattern

Many top-level files (`orbit.py`, `network_model.py`, etc.) are one-line re-export shims pointing to the real code in subdirectories (`physics/`, `network/`, `routing/`, `traffic/`, `metrics/`). **Always edit the real file in the subdirectory**, not the shim.

### Simulation Loop (`main.py:update_simulation`)

Each 16 ms timer tick executes in strict order:
1. **Physics** — `propagate_kepler_universal()` advances all satellite ECI states by `sim_dt_seconds`
2. **Topology** — `build_topology()` recomputes ISL graph (LOS + range check, O(N²))
3. **Ground links** — elevation mask + LOS check per ground station per active sat
4. **Failure model** — probabilistic blackouts/latency spikes applied to graph edges
5. **Routing** — `router.step()` recomputes routing tables over new graph
6. **Traffic** — packet generation, forwarding, queue drain, delivery/drop accounting
7. **Metrics** — delivery ratio, avg path length, link utilization, convergence time
8. **Render** — GL scatter/line items updated; telemetry HUD refreshed

---

## Key Physics Details

### Orbital Propagation
- **Two-body Kepler** using universal variables (Stumpff C/S functions) — valid for near-circular LEO
- ECI (Earth-Centered Inertial) Cartesian states `(r, v)` propagated each step
- RAAN spacing: `2π/NUM_PLANES` per plane (Walker-Delta constellation geometry)
- Phase offset: `2π/NUM_SATS_PER_PLANE * sat_idx + plane * 0.1 rad`

### Coordinate Transforms
- **ECI → ECEF**: simple R3 rotation by `θ₀ + ω_earth * t` (WGS-84 ω = 7.2921150e-5 rad/s)
- **ECEF → geodetic**: spherical Earth approximation (good to ~20 km for visualization)
- Render scale: `ECI km / 1000` → world units (Earth radius ≈ 6.378 world units)

### Link Model
- ISL latency: `distance_km / 299792.458 + 1 ms` (propagation + processing)
- ISL bandwidth: inverse-square scaling from 2000 Mbps at 500 km reference (`link_budget.py`)
- Ground link: same inverse-square from 300 Mbps base at 800 km reference
- Ground link extra latency: +3 ms (ground network processing)
- LOS: ray-sphere intersection against Earth body (no atmosphere margin yet)

### Ground Stations (default 5)
Sri Lanka, Singapore, London, New York, Tokyo — defined in `main.py:GROUND_STATIONS`.
Min elevation mask: 10°. Max ground-sat range: 3000 km.

---

## Current State (what works)

- [x] Real-time 3D OpenGL render with textured Earth sphere
- [x] 100 satellites, physics-valid orbits, ISL topology recomputed each step
- [x] Occlusion culling (back-face links hidden from camera)
- [x] Ground stations with elevation-masked links (bug fixed: all GS build links regardless of camera side)
- [x] Dijkstra + Distance-Vector routing (switchable via `ROUTING_MODE` constant)
- [x] Traffic simulation: uniform / hotspot / burst patterns
- [x] Kinetic strike (node failure) + System Reboot (recovery)
- [x] Node inspector (orbital elements, subpoint lat/lon, neighbors)
- [x] Telemetry HUD (delivery ratio, avg delay, hops, link utilization, route updates)
- [x] CSV logging (off by default, toggle `log_to_csv = True`)
- [x] Pause / Step / Speed slider / Strike severity slider
- [x] Tab-based right panel: MISSION | INSPECT | GROUND | 2D MAP
- [x] Ground station panel with live link table (best sat, elevation °, BW Mbps, latency ms)
- [x] 2D lat/lon topology map (re-enabled in MAP tab, throttled to 6 Hz)
- [x] Metrics plots at bottom (collapsible bar, 3 side-by-side, throttled to 12 Hz)
- [x] One-line metrics summary always visible even when plots collapsed

## UI Layout

```
┌──────────────────────────────┬──────────────────────────┐
│                              │ [MISSION|INSPECT|GND|MAP] │
│   3D OpenGL Globe            ├──────────────────────────┤
│   (dominant, resizable)      │ tab content              │
│                              │                          │
├──────────────────────────────┴──────────────────────────┤
│ [▼ METRICS] Delivery: X% | Delay: Xs | Util: X%         │
│ [delivery% plot]  [delay s plot]  [link util% plot]      │
└─────────────────────────────────────────────────────────┘
```

## Render Throttling

| Layer | Rate | Condition |
|---|---|---|
| 3D GL (satellites, links, path) | 60 Hz (every tick) | Always |
| Telemetry HUD text | 60 Hz | Always |
| Metrics summary label | 60 Hz | Always |
| Ground station table | ~12 Hz (every 5 ticks) | Always |
| Metrics plots | ~12 Hz | Only when bar expanded |
| 2D topology map | ~6 Hz (every 10 ticks) | Only when MAP tab active |

---

## Planned Improvements (Priority Order)

### Phase 1 — UI Overhaul (immediate)
- Re-enable and fix the 2D lat/lon topology map (currently items exist but panel removed)
- Re-enable metrics-over-time plots (delivery ratio, delay, utilization curves)
- Add a ground station panel: show which sat each GS is locked to, current link MBps, elevation angle
- Add a "link inspector": click any ISL or ground link to show distance, latency, bandwidth, loss
- Tab-based right panel (Mission Control / Node Inspector / Ground Stations / Metrics)
- Status bar at bottom with sim time, FPS, active nodes, total links count

### Phase 2 — Ground-to-Ground File Transfer Feature
The headline new feature: send a payload (e.g., an image) from Ground Station A to Ground Station B through the constellation and compute real-time throughput (MBps).

Implementation plan:
1. Add `FileTransferSession` dataclass: `src_gs_id`, `dst_gs_id`, `total_bytes`, `bytes_sent`, `start_t`, `end_t`
2. Use the existing routing layer to find path: `src_gs → sat → ... → sat → dst_gs`
3. Throttle bytes through each hop using actual `link.props.bandwidth_mbps` (already computed per link)
4. Compute instantaneous MBps = `bytes_transmitted_this_step / dt_s / 1e6`
5. UI: dropdown to select source GS and destination GS, "Send Image" button, progress bar + live MBps display
6. Show the active transfer path highlighted in a distinct color on the 3D view
7. Show bottleneck link highlighted (the minimum-bandwidth link on the path)

Realistic constraints to model:
- Half-duplex ISL (Starlink uses optical ISLs ~100 Gbps; our sim uses bandwidth from link_budget)
- Ground uplink/downlink asymmetry (uplink typically lower power)
- Handover interruption: if the serving satellite leaves elevation mask during transfer, simulate brief outage
- TCP-like congestion: optional, can simplify to min-bandwidth bottleneck model

### Phase 3 — Physics Realism Upgrades
- **J2 perturbation**: add `J2 = 1.08263e-3` nodal precession to RAAN and argument of perigee drift
- **Atmospheric drag**: simple exponential density model for accurate LEO decay at 550 km
- **Proper WGS-84 geodetic**: replace spherical with ellipsoidal lat/lon (affects ground station accuracy)
- **GMST (Greenwich Mean Sidereal Time)**: replace `theta0_rad=0` with actual GMST from epoch for real-sky coordinates
- **Multiple shells**: add a second constellation shell (e.g., 1200 km, 53.8°) to simulate Starlink Gen2

### Phase 4 — RF Link Budget
Replace `inverse_square_budget()` with a proper Friis/link-margin model:
- Transmit power (dBm), antenna gain (dBi), system noise temperature
- Free-space path loss: `FSPL = 20 log10(d) + 20 log10(f) + 92.45` (GHz, km)
- Rain fade margin for ground links (ITU-R P.618)
- Optical ISL model: aperture, pointing loss, atmospheric window
- Output: actual achievable data rate from Shannon capacity or modulation table

### Phase 5 — Advanced Routing
- **OSPF-like**: link-state flooding with sequence numbers
- **Geographic routing**: use sat lat/lon to route toward destination GS
- **QoS-aware**: priority queues, delay-bounded routing for real-time vs bulk traffic
- **Multi-path**: ECMP or spray-and-wait for resilience

### Phase 6 — Experiment Framework
Flesh out `experiments.py`:
- Headless batch runs (no GUI) for parameter sweeps
- Output: CSV + JSON metrics per run
- Compare routing algorithms under varying failure rates
- Plot: convergence time vs failure severity, throughput vs constellation density

---

## Development Rules

### Where to Edit
- **Physics changes**: `physics/orbit.py` (not `orbit.py`)
- **Network/topology changes**: `network/graph.py` (not `network_model.py`)
- **Routing changes**: `routing/algorithms.py` (not `routing.py`)
- **Traffic changes**: `traffic/sim.py` (not `traffic.py`)
- **Failure model**: `network/failures.py` (not `failures.py`)
- **Metrics**: `metrics/collector.py` (not `metrics.py`)
- **UI + orchestration**: `main.py` directly
- **Ground stations**: `ground.py`
- **Link budget model**: `link_budget.py`

### Constants in `main.py`
All top-level simulation constants live at the top of `main.py`:
- `NUM_PLANES`, `NUM_SATS_PER_PLANE`, `LEO_ALTITUDE_KM`, `INCLINATION_RAD`
- `MAX_LINK_RANGE_KM`, `GROUND_SAT_MAX_RANGE_KM`, `GROUND_BW_Mbps`
- `ROUTING_MODE`, `ROUTING_COST`, `TRAFFIC_PATTERN`, `TRAFFIC_RATE_PPS`
- `GROUND_STATIONS` list of `(name, lat_deg, lon_deg)` tuples

### Ground Station IDs
Satellites: IDs `0..NUM_TOTAL_SATS-1` (0..99 for default 10×10).
Ground stations: IDs `NUM_TOTAL_SATS + i` (100..104 for 5 stations).
This convention is used throughout routing, traffic, and graph code.

### Performance Notes
- `build_topology` is O(N²) — 100 sats = 4950 pairs, fast enough at 16 ms tick
- Earth mesh: 900×900 rows/cols — rendering heavy; reduce if FPS drops
- `propagate_kepler_universal` iterates up to 50 Newton steps per satellite
- Traffic `step()` iterates all live packets — purge old packets periodically to avoid memory growth

### Running
```powershell
python -m venv venv
venv\Scripts\activate
pip install numpy PyQt6 pyqtgraph
python main.py
```

---

## Open-Source Readiness Checklist

- [ ] Add proper RF link budget (replace heuristic inverse-square model)
- [ ] Add J2 perturbation for long-run accuracy
- [ ] Re-enable and polish 2D topology map and metrics plots
- [ ] Add ground-to-ground file transfer with live MBps display
- [ ] Add unit tests for `physics/orbit.py` (propagation round-trip, COE conversion)
- [ ] Add unit tests for routing correctness (shortest path, distance-vector convergence)
- [ ] Clean up shim pattern or document it clearly in README
- [ ] Add `pyproject.toml` / `requirements.txt`
- [ ] Record demo video for README
