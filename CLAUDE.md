# CLAUDE.md — ASTRA Project Guide (C)

## What This Is

ASTRA (Autonomous Satellite Traffic & Routing Architecture) is a real-time LEO
satellite constellation simulator written in **pure C (C11)** with a hand-rolled
**OpenGL 3.3 + GLX/EGL** visualization (no GLFW, no engine). It is a PhD research
project targeting open-source publication and SpaceX/Starlink-grade physical
realism.

The default constellation is 100 satellites (10 planes × 10 sats) at 550 km LEO,
53° inclination (Starlink-like Walker-Delta). The simulation core is headless and
runs a fixed-step tick; the renderer is a separate thread reading immutable
snapshots.

> **History:** ASTRA began as Python + PyQt6 + pyqtgraph. It was fully ported to
> C and verified numerically against the Python (see *Verification*), then the
> Python was removed (commit `e09f97e`). The Python is recoverable via git
> (`git checkout 9dfbf3a -- <path>`); the frozen parity vectors it produced live
> in `tools/*_vectors.txt` and are what `make test` checks against today.

---

## Layout

```
include/astra/      — public headers (config, vec3, orbit, graph, routing,
                      ground, failures, traffic, metrics, rng, sim, sim_thread)
src/                — core simulation library (one .c per header)
tests/              — C verification tests (one binary each); read tools/*_vectors.txt
tools/*_vectors.txt — FROZEN Python↔C parity vectors (the verification reference)
apps/               — headless tools (profilers / dataset generators)
viz/                — renderer library (GL context, loader, shaders, scene)
gui/                — viz executables (headless render, interactive viewer, tests)
img/                — textures + sample renders
Makefile            — build everything (no cmake)
```

### Module map (`src/` + `include/astra/`)

| Module | Responsibility |
|---|---|
| `orbit` | Two-body Kepler (universal variables / Stumpff), COE↔RV, ECI/ECEF, geodetic, LOS, elevation |
| `graph` | `NetworkGraph`, links, inverse-square link budget, ISL topology (O(N²)), CSR build, O(1) adjacency |
| `routing` | `Router`: Dijkstra (lazy (dist,node) heap) + Distance-Vector (sync Bellman-Ford); all-pairs next-hop table |
| `ground` | Ground stations, elevation-masked ground↔sat links |
| `failures` | Probabilistic link blackout / latency spike / loss multiplier (PCG32 RNG) |
| `traffic` | Zero-alloc packet pool, uniform/hotspot/burst generators, per-directed-link bandwidth budget |
| `metrics` | Delivery ratio, delay, hops, link util, route churn (full next-hop diff), convergence |
| `sim` | Orchestrator: fixed-step `astra_sim_tick`, lock-free triple-buffer snapshot, SPSC command ring |
| `sim_thread` | pthread driver (realtime ~60 Hz or free-run) |
| `rng`/`vec3`/`config` | PCG32 PRNG; double-precision vec3; all compile-time constants |

## The Tick (`src/sim.c:advance`)

Strict order each step: **propagate → build active sets → ISL topology → ground
links → failures → CSR → routing → traffic → metrics**. Then `astra_sim_tick`
drains commands, advances (unless paused), and publishes a `RenderSnapshot`.

## Sim ↔ Render Boundary (lock-free, race-free)

- The sim thread is the **sole writer** of all truth.
- It publishes immutable `RenderSnapshot`s through a **triple buffer**
  (`astra_snapshot_acquire` returns the newest complete frame; never blocks,
  never tears).
- Commands (strike / reboot / route+cost mode / speed / pause / rate) flow
  render→sim through an **SPSC ring** (`astra_cmd_push`), applied at a defined
  point in the tick — so **no snapshot can reference a half-dead node** (strike
  safety by design). Verified TSan-clean (`test_threading`).

---

## Verification (the spine)

C is checked against a **frozen Python parity snapshot**. `tools/*_vectors.txt`
hold reference vectors; `tests/test_*.c` recompute in C and diff against
tolerance. **`make test` needs no Python.** Results at parity time:

- orbit: pos err 3e-12 km · graph/topology: 5e-13 km · Dijkstra next-hop:
  bit-exact over 30k pairs · DV→Dijkstra cost: 4e-17 · ground links: exact.
- failures/traffic/metrics: invariant-verified (conservation, reproducibility).
- Routing weights/latency are **double** (not float32) so paths match Python
  float64 bit-exactly.

`make oracles` would regenerate the vectors from a Python reference (provenance
only; Python is gone).

---

## Visualization (`viz/` + `gui/`)

Hand-rolled OpenGL 3.3 core. **GLFW is not used** (unavailable/unvendorable in
the dev env); on Linux GLFW just wraps GLX/EGL, which we use directly:

- `viz/glctx*` — context abstraction with a vtable; two backends: **EGL**
  (offscreen, headless golden images) and **GLX/X11** (interactive window).
- `viz/glfn*` — GLEW-free runtime loader (X-macro list) for GL 2.0+ entry points
  via `eglGetProcAddress` / `glXGetProcAddressARB`. GL 1.1 calls use `-lGL`.
- `viz/shader`, `viz/mat4`, `viz/image` (libpng + libjpeg) — helpers.
- `viz/render.c` — the 3D scene: **photoreal Earth** (day-map texture from
  `img/earth_texture.jpg`, per-vertex equirectangular UVs, day/night terminator
  with faked city lights, ocean specular, additive atmosphere limb, starfield),
  ISL/ground links coloured by utilisation (idle=cool, loaded=hot, down=dim red,
  depth-occluded by the globe), satellites + ground stations as glowing points.
  Consumes a `RenderSnapshot` only. `render_view_proj()` exposes the frame MVP
  for screen projection (picking / callouts).
- `viz/ui.{c,h}` — batched immediate-mode 2D overlay: rects, outlines, thick
  lines, arcs, triangles in one shape draw; **FreeType** text via per-font R8
  glyph atlases (`ui_font_load(ttf, px)`). Pixel coords, RGBA, depth off.
- `viz/hud.{c,h}` — the **mission-control dashboard** drawn over the 3D scene
  from a `RenderSnapshot`: top status bar, GLOBAL ASSET LIST, SELECTED ASSET,
  LIVE NETWORK PERFORMANCE (avg-delay gauge + bar meters), SIMULATION CONTROL,
  bottom history plots, and a 3D selection callout. Pure presentation.
- `gui/astra_render` — headless: run N steps, render snapshot → PNG.
- `gui/astra_gui` — interactive viewer (sim on its own realtime thread + HUD).
  Controls: drag = orbit, wheel = zoom, **left-click = select satellite**,
  Up/Down (or `[` `]`) = walk the asset list, **S** = strike selected, **R** =
  reboot, **P** = pause, **M** = toggle routing, **Q/Esc** = quit.
  `--selftest N --shot out.png` renders N frames headlessly for validation.
- `gui/viz_golden` — visual-correctness test (render determinism + golden).
- `gui/viz_uitest` — UI-framework smoke test (panels/gauge/plot/text → PNG).

> Gotcha: point size is `gl_PointSize = uScale/p.w` where `p.w` ≈ camera distance
> (~18), so `uScale` must be ~hundreds, not single digits.
> Fonts live at `/usr/share/fonts/truetype/dejavu/`. Run the viewer from the repo
> root so `img/earth_texture.jpg` and the font paths resolve.

---

## Profiles (the four requested) — all in `apps/` + `gui/`

| Profile | Tool | Result |
|---|---|---|
| Performance | `apps/astra_profile` (`--sweep`) | 100 sats: ~840 steps/s, ~4200× real-time, 7.4% of a 16 ms tick. Hot: traffic 58%, routing 25%. |
| Network-behavior | `apps/astra_dataset` | per-step CSV + JSON; scripted `--strike`/`--reboot`/`--blackout`. Strike → route-churn spike (~10k vs ~250). |
| Physical-accuracy | `apps/astra_physcheck` | two-body invariants (energy/h/LRL ~1e-14, period closure 6e-11 km) → propagator analytically exact. |
| Visual-correctness | `gui/viz_golden` (`make viz-test`) | render is pixel-deterministic (mad 0.0000) + golden regression. |

**Deferred:** SGP4-vs-real-Starlink-TLE divergence study — needs outbound
network (celestrak) + the `sgp4` package; the dev sandbox has neither. The
harness is structured to accept TLEs in a networked environment. (Current model
is pure two-body, no J2/drag, so that study would quantify exactly that gap.)

**Known finding:** the default `MAX_LINK_RANGE_KM=2500` under-connects the 10×10
shell (in-plane spacing ≈4281 km > range ⇒ partition ⇒ ~0.16 delivery). At
5000 km the 100-sat shell reaches ~0.91 delivery; denser shells (144 sats)
self-connect even at 2500 km. At ≥1024 sats / 5000 km link count hits
`ASTRA_MAX_LINKS` (raise the degree cap for big shells).

---

## Building & Running

No cmake. Requires: gcc/clang (C11), `-pthread`, `-lm`; for viz also
`-lEGL -lGL -lX11 -lpng` (+ a GL-capable environment, e.g. Mesa/EGL).

```
make            # core library + headless apps
make test       # build & run the 9 C verification tests (Python-free)
make apps       # headless profilers/dataset tools
make gui        # viz executables (needs GL/EGL/X11/libpng)
make viz-test   # visual-correctness golden-image test (needs GL)
make clean
```

Quick looks:
```
./build/astra_profile --steps 2000              # perf + per-stage breakdown
./build/astra_profile --sweep                   # 100..1024-sat scaling
./build/astra_dataset --strike 200:42 --reboot 400 --csv out.csv --json out.json
./build/astra_render --range 5000 --out frame.png
./build/astra_gui --range 5000                  # interactive (needs a display)
```

---

## Development Rules

- **Edit the real file** under `src/` or `viz/`; headers in `include/astra/` or
  `viz/`. There are no more re-export shims.
- All compile-time constants live in `include/astra/config.h` (mirrors the old
  `main.py` constants). Capacities (`ASTRA_MAX_SATS`, `ASTRA_MAX_LINKS`, …) drive
  every static buffer — bump them for larger shells.
- **Node IDs:** sats `0..num_sats-1`; ground stations `num_sats + i`. Used
  throughout routing/traffic/graph.
- The renderer must stay a **pure function of `RenderSnapshot`** — never reach
  into sim state or recompute physics (e.g. GS world positions are precomputed
  into the snapshot in `publish`).
- Zero per-step allocation in the hot path (packet pool, CSR scratch, fixed
  buffers). Keep it that way.
- After changing core numerics, run `make test`; after changing the renderer,
  run `make viz-test`. Threading changes: re-run under ThreadSanitizer
  (`setarch $(uname -m) -R ./build/test_threading_tsan` to dodge the ASLR mmap
  clash).

## Roadmap (next)

- SGP4/TLE physical-accuracy study (when networked): quantify two-body vs real.
- Physics realism: J2 nodal precession, drag, WGS-84 geodetic, GMST, 2nd shell.
- RF link budget (Friis/Shannon) replacing the inverse-square heuristic.
- Ground-to-ground file transfer with live throughput + bottleneck highlight.
- Renderer polish: Earth texture (img/earth_texture.jpg), orbital ribbons, HUD,
  path highlight; vendor GLFW only if a portability need arises.
