# ASTRA - Autonomous Satellite Traffic and Routing Architecture


ASTRA (Autonomous Satellite Traffic and Routing Architecture) is a research-oriented simulation framework for evaluating routing algorithms and distributed decision-making in Low Earth Orbit (LEO) satellite constellations.

The platform models satellites as autonomous agents operating in a dynamic orbital environment, enabling the study of:

- Inter-satellite communication networks
- Distributed routing protocols
- Constellation-scale traffic engineering
- Autonomous network recovery
- Multi-agent coordination and consensus
- Resilient space-based communication architectures

ASTRA is designed from first principles with a strong focus on transparency, extensibility, and performance

You may only use this repository for high-performance computing (HPC) research and autonomous system validation. Use is subject to the standard open-source [terms of use](LICENSE).

<div align="center">
  <img src="https://images.unsplash.com/photo-1446776811953-b23d57bd21aa?q=80&w=1200&auto=format&fit=crop" alt="Constellation Banner" width="100%" style="border-radius: 10px;">
</div>

<br>


## Key Capabilities

Unlike standard network simulators, this environment is built entirely from scratch utilizing pure Python and core mathematical libraries to ensure zero framework bloat and maximum deterministic control:

* **Real-Time 3D ECI Physics Engine:** Transforms true anomaly, inclination, and Right Ascension of the Ascending Node (RAAN) into high-fidelity Cartesian 3D coordinates.
* **Decentralized Agent Consensus:** Each satellite acts as an independent agent running a dynamic, distributed distance-vector routing algorithm, sharing state only with local Line-of-Sight (LoS) neighbors.
* **Catastrophic Failure Recovery:** Agents autonomously detect dead links and re-converge routing tables within milliseconds of a simulated kinetic strike, without relying on a centralized ground map.
* **Walker Delta Topology:** Accurately models a multi-plane, phase-shifted orbital geometry identical to modern mega-constellations.

##  Installation & Usage

Ensure you have Python 3.10+ installed. To maintain performance, the only required external dependency is `numpy` for matrix operations and `matplotlib` for the 3D telemetry visualization.



```bash
# Clone the repository
git clone [https://github.com/yourusername/constellation-router.git](https://github.com/yourusername/constellation-router.git)
cd constellation-router

# Create a virtual environment and install requirements
python -m venv venv
source venv/bin/activate  # On Windows use `venv\\Scripts\\activate`
pip install -r requirements.txt

```

### Running the 3D Telemetry Dashboard

To launch the real-time Earth-Centered Inertial (ECI) simulation with the interactive telemetry dashboard:

```bash
python run_simulation.py --planes 10 --sats_per_plane 10 --interactive

```

### Simulating a Kinetic Strike

During the interactive simulation, you can manually trigger a massive topology shift to observe the network's autonomous self-healing capabilities:

1. Click the **"Trigger Kinetic Strike"** button in the UI.
2. Watch as 20% of the active nodes are destroyed.
3. Observe the `[TELEMETRY]` readouts as agents locally invalidate dead paths and re-establish the packet trace route `Sat A -> Sat B` around the debris field.

## System Architecture

The repository is structured to separate the physical constraints from the agent logic:

* `core/physics.py`: Contains the `MU_EARTH`, `G_CONST`, and the Keplerian-to-Cartesian $O(N^2)$ transformation models. *(Note: Targeted for future CUDA acceleration)*.
* `agents/router.py`: Contains the `SatelliteAgent` class and the autonomous `process_neighbor_update()` consensus logic.
* `viz/dashboard_3d.py`: The `matplotlib` real-time visualization interface featuring bypassing of generic auto-scaling for guaranteed stability.

## Contributing

If you wish to contribute, specifically in porting the $O(N^2)$ Line-of-Sight distance calculations to a **custom CUDA kernel** via Numba/C++, please submit a pull request.

## ⚖️ License

Copyright © 2024. Licensed under the MIT License. See `LICENSE` for details.
