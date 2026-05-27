import sys
import math
import random
import numpy as np

from PyQt6.QtCore import QTimer, Qt
from PyQt6.QtGui import QPixmap
from PyQt6.QtWidgets import (QApplication, QMainWindow, QWidget, QHBoxLayout, 
                             QVBoxLayout, QPushButton, QLabel, QListWidget, 
                             QSplitter, QGroupBox, QSlider)

import pyqtgraph as pg
import pyqtgraph.opengl as gl
from orbit import (
    OrbitalElements,
    CartesianState,
    coe_to_rv,
    rv_to_coe,
    propagate_kepler_universal,
    eci_to_ecef,
    ecef_to_geodetic_spherical,
)
from orbit import is_visible_from_station_ecef
from network_model import NetworkGraph, build_topology, Link, LinkProperties, SPEED_OF_LIGHT_KM_S
from routing import DijkstraRouter, DistanceVectorRouter, Router
from traffic import TrafficGenerator, TrafficSimulator
from failures import FailureModel, LinkImpairments
from ground import GroundStation
from link_budget import inverse_square_budget
from logging_utils import CSVRunLogger
from metrics import MetricsCollector

# --- Realistic Physical Constants ---
EARTH_RADIUS_KM = 6378.137 
MU_EARTH = 398600.4418 

# --- Constellation Parameters ---
LEO_ALTITUDE_KM = 550.0
SAT_ALTITUDE_KM = EARTH_RADIUS_KM + LEO_ALTITUDE_KM 
MAX_LINK_RANGE_KM = 2500.0  

NUM_PLANES = 10
NUM_SATS_PER_PLANE = 10
NUM_TOTAL_SATS = NUM_PLANES * NUM_SATS_PER_PLANE
INCLINATION_RAD = math.radians(53.0) 

# --- Network/Traffic Defaults ---
ROUTING_MODE = "distance_vector"  # "distance_vector" | "dijkstra"
ROUTING_COST = "hops"  # "hops" | "latency"
TRAFFIC_PATTERN = "uniform"  # "uniform" | "hotspot" | "burst"
TRAFFIC_RATE_PPS = 3.0

# --- Ground Stations ---
GROUND_MIN_ELEV_DEG = 10.0
GROUND_SAT_MAX_RANGE_KM = 3000.0
GROUND_BW_Mbps = 300.0
GROUND_BASE_LOSS = 0.002

# A few example stations (extend as needed)
GROUND_STATIONS = [
    ("Sri Lanka", 7.8731, 80.7718),
    ("Singapore", 1.3521, 103.8198),
    ("London", 51.5074, -0.1278),
    ("New York", 40.7128, -74.0060),
    ("Tokyo", 35.6762, 139.6503),
]

# --- UI ---
EARTH_TEXTURE_PATH = "img/earth_texture.jpg"
TOPOLOGY_HISTORY_STEPS = 300

# --- Professional Dark Theme (QSS) ---
DARK_THEME = """
QMainWindow, QWidget { background-color: #0d1117; color: #c9d1d9; font-family: 'Segoe UI', Arial, sans-serif; }
QGroupBox { border: 1px solid #30363d; border-radius: 6px; margin-top: 14px; font-weight: bold; font-size: 13px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; color: #58a6ff; }
QPushButton { background-color: #21262d; border: 1px solid #30363d; border-radius: 5px; padding: 8px; font-weight: bold; color: #c9d1d9; }
QPushButton:hover { background-color: #30363d; border: 1px solid #8b949e; }
QListWidget { background-color: #010409; border: 1px solid #30363d; border-radius: 5px; padding: 5px; outline: none; }
QListWidget::item { padding: 4px; border-radius: 3px; }
QListWidget::item:selected { background-color: #1f6feb; color: white; font-weight: bold; }
QListWidget::item:hover { background-color: #21262d; }
QSlider::groove:horizontal { border: 1px solid #30363d; height: 6px; background: #010409; border-radius: 3px; }
QSlider::handle:horizontal { background: #58a6ff; width: 14px; margin: -4px 0; border-radius: 7px; }
QSlider::handle:horizontal:hover { background: #79c0ff; }
QLabel { font-size: 12px; }
"""

class SatelliteAgent:
    def __init__(self, node_id, orbit_plane_idx, phase_offset_rad):
        self.node_id = node_id
        self.name = f"SAT-{orbit_plane_idx:02d}-{node_id % NUM_SATS_PER_PLANE:02d}"
        self.plane_idx = orbit_plane_idx
        
        # --- Orbital State (ECI) ---
        # Keep v1 simple but physically meaningful: near-circular LEO shells, 2-body dynamics.
        self.RAAN_rad = (2 * math.pi / NUM_PLANES) * self.plane_idx
        self.inclination = INCLINATION_RAD
        self.argp_rad = 0.0
        # Small eccentricity helps avoid perfectly rigid "ring" behavior while staying simple.
        self.ecc = 0.001
        self.a_km = float(SAT_ALTITUDE_KM)
        self.nu_rad = float(phase_offset_rad)

        self.coe = OrbitalElements(
            a_km=self.a_km,
            e=self.ecc,
            i_rad=self.inclination,
            raan_rad=self.RAAN_rad,
            argp_rad=self.argp_rad,
            nu_rad=self.nu_rad,
        )
        self.state = coe_to_rv(MU_EARTH, self.coe)
        
        self.pos_3d = np.zeros(3)
        self.vel_eci_km_s = np.zeros(3)
        self.is_active = True
        self.neighbors = []  # populated by network layer each step

        self._sync_render_state()

    def _sync_render_state(self):
        # UI uses a 1/1000 scaling so Earth radius (~6378 km) becomes ~6.4 units.
        self.pos_3d = self.state.r_km / 1000.0
        self.vel_eci_km_s = self.state.v_km_s.copy()
        # Keep nu_rad updated for existing UI fields.
        try:
            self.coe = rv_to_coe(MU_EARTH, self.state.r_km, self.state.v_km_s)
            self.nu_rad = self.coe.nu_rad
        except Exception:
            # For numerical edge cases, don't crash the UI loop.
            pass

    def update_physics(self, dt_seconds):
        if not self.is_active: return
        r, v = propagate_kepler_universal(MU_EARTH, self.state.r_km, self.state.v_km_s, float(dt_seconds))
        self.state = CartesianState(r_km=r, v_km_s=v)
        self._sync_render_state()

class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("SpaceXAI - Multi-Agent Constellation Router")
        self.resize(1600, 950)
        
        # State Variables
        self.sats = []
        for plane in range(NUM_PLANES):
            for sat_idx in range(NUM_SATS_PER_PLANE):
                node_id = plane * NUM_SATS_PER_PLANE + sat_idx
                phase_offset = (2 * math.pi / NUM_SATS_PER_PLANE) * sat_idx + (plane * 0.1)
                self.sats.append(SatelliteAgent(node_id, plane, phase_offset))
                
        self.trace_src = 12
        self.trace_dst = 77
        self.step_count = 0
        self.selected_sat_id = None
        self.status_msg = "NOMINAL"
        self.last_sim_time_s = 0.0

        # --- Ground Stations ---
        self.ground_nodes = []  # list[GroundStation]
        self.ground_node_ids = []
        for i, (name, lat_deg, lon_deg) in enumerate(GROUND_STATIONS):
            gid = NUM_TOTAL_SATS + i
            gs = GroundStation(
                gid=gid,
                name=name,
                lat_rad=math.radians(float(lat_deg)),
                lon_rad=math.radians(float(lon_deg)),
                alt_km=0.0,
                min_elev_deg=GROUND_MIN_ELEV_DEG,
            )
            self.ground_nodes.append(gs)
            self.ground_node_ids.append(gid)
        self.best_sat_for_station = {gs.gid: None for gs in self.ground_nodes}
        self.run_logger = None

        # Topology / dashboard state
        self.prev_edge_set = set()
        self.failed_edges = []  # list of (u,v,ttl_steps)
        self.metric_hist = {
            "t": [],
            "delivery": [],
            "delay": [],
            "util": [],
        }
        self.occlusion_enabled = True
        
        # Dynamic Parameters
        self.sim_dt_seconds = 5.0
        self.strike_severity = 0.20

        # --- Network / Routing / Traffic ---
        self.graph = NetworkGraph()
        self.router: Router = self._make_router()
        self.traffic = TrafficSimulator(
            TrafficGenerator(pattern=TRAFFIC_PATTERN, rate_pps=TRAFFIC_RATE_PPS, hotspot_id=self.trace_dst)
        )
        self.last_traffic_stats = None
        self.metrics = MetricsCollector(sample_pairs=40)
        self.last_step_metrics = None

        # Failure/impairment knobs (expose in UI later if needed)
        self.failure_model = FailureModel(
            LinkImpairments(
                blackout_prob_per_s=0.0000,
                latency_spike_prob_per_s=0.0000,
                latency_spike_add_s=0.030,
                loss_multiplier=1.0,
            )
        )

        # Optional: CSV logging (toggle later; default off)
        self.log_to_csv = False
        if self.log_to_csv:
            self.run_logger = CSVRunLogger("runs/latest_run.csv")

        self.setup_ui()
        self.init_gl_objects()
        
        self.timer = QTimer()
        self.timer.timeout.connect(self.update_simulation)
        self.timer.start(16) 

    def _make_router(self) -> Router:
        if ROUTING_MODE == "dijkstra":
            return DijkstraRouter(weight=ROUTING_COST)
        return DistanceVectorRouter(cost=ROUTING_COST, rounds_per_step=3)

    def setup_ui(self):
        main_widget = QWidget()
        self.setCentralWidget(main_widget)
        main_layout = QHBoxLayout(main_widget)
        main_layout.setContentsMargins(10, 10, 10, 10)
        
        splitter = QSplitter(Qt.Orientation.Horizontal)
        main_layout.addWidget(splitter)
        
        # --- LEFT PANEL: 3D Render ---
        self.gl_view = gl.GLViewWidget()
        self.gl_view.setCameraPosition(distance=25, elevation=30, azimuth=40)
        self.gl_view.setBackgroundColor('#05070a') # Deep space background
        splitter.addWidget(self.gl_view)
        
        # --- RIGHT PANEL: Controls & Telemetry ---
        right_container = QWidget()
        right_layout = QVBoxLayout(right_container)
        right_container.setMaximumWidth(450)
        
        # 1. Telemetry HUD
        self.lbl_telemetry = QLabel("INITIALIZING...")
        self.lbl_telemetry.setStyleSheet("font-family: 'Consolas', monospace; font-size: 13px; color: #3fb950; background-color: #010409; padding: 15px; border: 1px solid #30363d; border-radius: 6px;")
        right_layout.addWidget(self.lbl_telemetry)

        # 1b. Earth texture reference (external image)
        earth_group = QGroupBox("EARTH REFERENCE")
        earth_layout = QVBoxLayout()
        self.lbl_earth_img = QLabel()
        self.lbl_earth_img.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.lbl_earth_img.setStyleSheet("background-color: #010409; border: 1px solid #30363d; border-radius: 6px; padding: 6px;")
        try:
            pm = QPixmap(EARTH_TEXTURE_PATH)
            if not pm.isNull():
                self.lbl_earth_img.setPixmap(pm.scaledToWidth(400, Qt.TransformationMode.SmoothTransformation))
            else:
                self.lbl_earth_img.setText(f"Missing image: {EARTH_TEXTURE_PATH}")
        except Exception:
            self.lbl_earth_img.setText(f"Failed to load: {EARTH_TEXTURE_PATH}")
        earth_layout.addWidget(self.lbl_earth_img)
        earth_group.setLayout(earth_layout)
        right_layout.addWidget(earth_group)
        
        # 2. Mission Control Panel
        control_group = QGroupBox("MISSION CONTROL")
        control_layout = QVBoxLayout()
        
        # Speed Slider
        self.lbl_speed = QLabel(f"Simulation Speed (ΔT): {self.sim_dt_seconds}x")
        self.slider_speed = QSlider(Qt.Orientation.Horizontal)
        self.slider_speed.setRange(1, 50)
        self.slider_speed.setValue(int(self.sim_dt_seconds))
        self.slider_speed.valueChanged.connect(self.update_speed)
        control_layout.addWidget(self.lbl_speed)
        control_layout.addWidget(self.slider_speed)
        
        # Strike Severity Slider
        self.lbl_severity = QLabel(f"Strike Severity: {int(self.strike_severity*100)}% Node Loss")
        self.slider_severity = QSlider(Qt.Orientation.Horizontal)
        self.slider_severity.setRange(5, 80)
        self.slider_severity.setValue(int(self.strike_severity * 100))
        self.slider_severity.valueChanged.connect(self.update_severity)
        control_layout.addWidget(self.lbl_severity)
        control_layout.addWidget(self.slider_severity)
        
        # Action Buttons
        btn_layout = QHBoxLayout()
        self.btn_strike = QPushButton("EXECUTE STRIKE")
        self.btn_strike.setStyleSheet("background-color: #da3633; color: white; border-color: #b3261e;")
        self.btn_strike.clicked.connect(self.trigger_kinetic_strike)
        
        self.btn_reset = QPushButton("SYSTEM REBOOT")
        self.btn_reset.setStyleSheet("background-color: #238636; color: white; border-color: #2ea043;")
        self.btn_reset.clicked.connect(self.reset_network)

        self.btn_pause = QPushButton("PAUSE")
        self.btn_pause.setCheckable(True)
        self.btn_pause.setStyleSheet("background-color: #21262d; color: white; border-color: #30363d;")
        self.btn_pause.toggled.connect(self.toggle_pause)

        self.btn_step = QPushButton("STEP")
        self.btn_step.setStyleSheet("background-color: #21262d; color: white; border-color: #30363d;")
        self.btn_step.clicked.connect(self.step_once)

        self.btn_occlusion = QPushButton("OCCLUSION: ON")
        self.btn_occlusion.setCheckable(True)
        self.btn_occlusion.setChecked(True)
        self.btn_occlusion.setStyleSheet("background-color: #21262d; color: white; border-color: #30363d;")
        self.btn_occlusion.toggled.connect(self.toggle_occlusion)
        
        btn_layout.addWidget(self.btn_strike)
        btn_layout.addWidget(self.btn_reset)
        btn_layout.addWidget(self.btn_pause)
        btn_layout.addWidget(self.btn_step)
        btn_layout.addWidget(self.btn_occlusion)
        control_layout.addLayout(btn_layout)
        
        control_group.setLayout(control_layout)
        right_layout.addWidget(control_group)
        
        # 3. Individual Node Inspector
        inspector_group = QGroupBox("NODE INSPECTOR")
        inspector_layout = QVBoxLayout()
        
        self.sat_list_widget = QListWidget()
        for sat in self.sats:
            self.sat_list_widget.addItem(f"{sat.name} (ID: {sat.node_id})")
        self.sat_list_widget.currentRowChanged.connect(self.sat_selected)
        inspector_layout.addWidget(self.sat_list_widget)
        
        self.lbl_spec = QLabel("Awaiting Node Selection...")
        self.lbl_spec.setStyleSheet("font-family: 'Consolas', monospace; font-size: 12px; background-color: #010409; padding: 12px; border: 1px solid #30363d; border-radius: 5px;")
        self.lbl_spec.setWordWrap(True)
        inspector_layout.addWidget(self.lbl_spec)
        
        inspector_group.setLayout(inspector_layout)
        right_layout.addWidget(inspector_group)

        # 4. Live topology panel (2D)
        topo_group = QGroupBox("LIVE TOPOLOGY (LAT/LON)")
        topo_layout = QVBoxLayout()
        self.topo_plot = pg.PlotWidget()
        self.topo_plot.setBackground('#010409')
        self.topo_plot.showGrid(x=True, y=True, alpha=0.2)
        self.topo_plot.setLabel('bottom', 'Longitude (deg)')
        self.topo_plot.setLabel('left', 'Latitude (deg)')
        self.topo_plot.setXRange(-180, 180)
        self.topo_plot.setYRange(-90, 90)
        self.topo_edges_item = pg.PlotDataItem(pen=pg.mkPen((80, 120, 200, 70), width=1))
        self.topo_failed_item = pg.PlotDataItem(pen=pg.mkPen((220, 60, 60, 180), width=2))
        self.topo_route_item = pg.PlotDataItem(pen=pg.mkPen((255, 200, 40, 220), width=3))
        self.topo_nodes_item = pg.ScatterPlotItem(size=6, brush=pg.mkBrush(60, 180, 80, 220), pen=None)
        self.topo_ground_item = pg.ScatterPlotItem(size=8, brush=pg.mkBrush(120, 160, 255, 230), pen=None)
        self.topo_plot.addItem(self.topo_edges_item)
        self.topo_plot.addItem(self.topo_failed_item)
        self.topo_plot.addItem(self.topo_route_item)
        self.topo_plot.addItem(self.topo_nodes_item)
        self.topo_plot.addItem(self.topo_ground_item)
        topo_layout.addWidget(self.topo_plot)
        topo_group.setLayout(topo_layout)
        right_layout.addWidget(topo_group)

        # 5. Metrics-over-time plots
        metrics_group = QGroupBox("METRICS OVER TIME")
        metrics_layout = QVBoxLayout()
        self.plot_delivery = pg.PlotWidget()
        self.plot_delivery.setBackground('#010409')
        self.plot_delivery.setMaximumHeight(110)
        self.plot_delivery.showGrid(x=True, y=True, alpha=0.2)
        self.plot_delivery.setLabel('left', 'Delivery %')
        self.curve_delivery = self.plot_delivery.plot(pen=pg.mkPen((80, 200, 120), width=2))
        metrics_layout.addWidget(self.plot_delivery)

        self.plot_delay = pg.PlotWidget()
        self.plot_delay.setBackground('#010409')
        self.plot_delay.setMaximumHeight(110)
        self.plot_delay.showGrid(x=True, y=True, alpha=0.2)
        self.plot_delay.setLabel('left', 'Delay (s)')
        self.curve_delay = self.plot_delay.plot(pen=pg.mkPen((120, 160, 255), width=2))
        metrics_layout.addWidget(self.plot_delay)

        self.plot_util = pg.PlotWidget()
        self.plot_util.setBackground('#010409')
        self.plot_util.setMaximumHeight(110)
        self.plot_util.showGrid(x=True, y=True, alpha=0.2)
        self.plot_util.setLabel('left', 'Util %')
        self.curve_util = self.plot_util.plot(pen=pg.mkPen((255, 200, 40), width=2))
        metrics_layout.addWidget(self.plot_util)

        metrics_group.setLayout(metrics_layout)
        right_layout.addWidget(metrics_group)
        
        splitter.addWidget(right_container)
        splitter.setSizes([1100, 500])

    def toggle_occlusion(self, enabled: bool):
        self.occlusion_enabled = bool(enabled)
        self.btn_occlusion.setText("OCCLUSION: ON" if self.occlusion_enabled else "OCCLUSION: OFF")

    def update_speed(self, value):
        self.sim_dt_seconds = float(value)
        self.lbl_speed.setText(f"Simulation Speed (ΔT): {self.sim_dt_seconds}x")

    def update_severity(self, value):
        self.strike_severity = value / 100.0
        self.lbl_severity.setText(f"Strike Severity: {value}% Node Loss")

    def toggle_pause(self, paused: bool):
        if paused:
            self.btn_pause.setText("RESUME")
            self.timer.stop()
        else:
            self.btn_pause.setText("PAUSE")
            self.timer.start(16)

    def step_once(self):
        # single simulation step when paused
        if self.timer.isActive():
            return
        self.update_simulation()

    def _camera_position_world(self) -> np.ndarray:
        """
        Best-effort camera position in world coordinates (same units as GL items).
        """
        try:
            p = self.gl_view.cameraPosition()
            return np.array([float(p.x()), float(p.y()), float(p.z())], dtype=float)
        except Exception:
            # Fallback using azimuth/elevation/distance
            try:
                d = float(self.gl_view.opts.get("distance", 25.0))
                az = math.radians(float(self.gl_view.opts.get("azimuth", 0.0)))
                el = math.radians(float(self.gl_view.opts.get("elevation", 0.0)))
                # pyqtgraph uses azimuth around z and elevation from xy plane
                x = d * math.cos(el) * math.cos(az)
                y = d * math.cos(el) * math.sin(az)
                z = d * math.sin(el)
                return np.array([x, y, z], dtype=float)
            except Exception:
                return np.array([0.0, 0.0, 25.0], dtype=float)

    def _is_occluded_by_earth(self, point_world: np.ndarray, earth_radius_world: float) -> bool:
        """
        True if the point is behind the Earth sphere relative to the camera.
        Uses ray-sphere intersection from camera to point.
        """
        cam = self._camera_position_world()
        p = np.asarray(point_world, dtype=float)
        d = p - cam
        dd = float(np.dot(d, d))
        if dd <= 1e-12:
            return False
        # Solve |cam + t d|^2 = R^2 for t in [0,1)
        a = dd
        b = 2.0 * float(np.dot(cam, d))
        c = float(np.dot(cam, cam)) - earth_radius_world * earth_radius_world
        disc = b * b - 4.0 * a * c
        if disc <= 0.0:
            return False
        sqrt_disc = math.sqrt(disc)
        t1 = (-b - sqrt_disc) / (2.0 * a)
        t2 = (-b + sqrt_disc) / (2.0 * a)
        # any intersection between camera and point means occluded
        return (0.0 < t1 < 1.0) or (0.0 < t2 < 1.0)

    def init_gl_objects(self):
        earth_rad_scaled = EARTH_RADIUS_KM / 1000.0
        md = gl.MeshData.sphere(rows=60, cols=60, radius=earth_rad_scaled)

        # Procedural "texture" for orientation (no external assets needed)
        verts = md.vertexes()
        colors = np.zeros((verts.shape[0], 4), dtype=float)
        for i, (x, y, z) in enumerate(verts):
            r = math.sqrt(x * x + y * y + z * z) + 1e-12
            lat = math.asin(z / r)
            lon = math.atan2(y, x)
            lat_d = math.degrees(lat)

            # Polar caps
            if abs(lat_d) > 70:
                col = (0.95, 0.97, 1.0, 0.95)
            else:
                # Deterministic pseudo-land mask from trig "noise"
                n = (
                    math.sin(3.0 * lon)
                    + 0.6 * math.sin(2.0 * lat)
                    + 0.4 * math.sin(5.0 * (lon + lat))
                    + 0.2 * math.sin(11.0 * lon + 3.0 * lat)
                )
                if n > 0.35:
                    # land
                    col = (0.15, 0.45, 0.22, 0.95)
                    # deserts band
                    if abs(lat_d) < 25 and n > 0.75:
                        col = (0.62, 0.52, 0.28, 0.95)
                else:
                    # ocean
                    col = (0.05, 0.12, 0.28, 0.92)
                    # shallow water near "coasts"
                    if 0.25 < n < 0.35:
                        col = (0.05, 0.22, 0.35, 0.92)

            colors[i] = col

        try:
            md.setVertexColors(colors)
            earth_color = (1, 1, 1, 1)
        except Exception:
            # Fallback if pyqtgraph build lacks vertex color support
            earth_color = (0.08, 0.12, 0.2, 0.75)

        self.earth_mesh = gl.GLMeshItem(meshdata=md, smooth=True, color=earth_color, shader='shaded', glOptions='translucent')
        self.gl_view.addItem(self.earth_mesh)

        # Orientation helpers: equator + prime meridian
        steps = 200
        eq = np.zeros((steps + 1, 3), dtype=float)
        pm = np.zeros((steps + 1, 3), dtype=float)
        for k in range(steps + 1):
            ang = (2 * math.pi / steps) * k
            eq[k] = [earth_rad_scaled * math.cos(ang), earth_rad_scaled * math.sin(ang), 0.0]
            pm[k] = [earth_rad_scaled * math.cos(0.0) * math.cos(ang), earth_rad_scaled * math.cos(0.0) * math.sin(0.0), earth_rad_scaled * math.sin(ang)]
        self.gl_equator = gl.GLLinePlotItem(pos=eq, color=(1.0, 1.0, 1.0, 0.25), width=1, antialias=True)
        self.gl_view.addItem(self.gl_equator)
        self.gl_meridian = gl.GLLinePlotItem(pos=pm, color=(1.0, 0.3, 0.3, 0.28), width=1, antialias=True)
        self.gl_view.addItem(self.gl_meridian)
        
        for plane in range(NUM_PLANES):
            steps = 100
            path_pts = np.zeros((steps + 1, 3))
            Om = (2 * math.pi / NUM_PLANES) * plane
            for s in range(steps + 1):
                nu = (2 * math.pi / steps) * s
                r = SAT_ALTITUDE_KM / 1000.0
                x = r * (math.cos(Om) * math.cos(nu) - math.sin(Om) * math.sin(nu) * math.cos(INCLINATION_RAD))
                y = r * (math.sin(Om) * math.cos(nu) + math.cos(Om) * math.sin(nu) * math.cos(INCLINATION_RAD))
                z = r * (math.sin(nu) * math.sin(INCLINATION_RAD))
                path_pts[s] = [x, y, z]
            orbit_line = gl.GLLinePlotItem(pos=path_pts, color=(0.15, 0.2, 0.3, 0.4), width=1, antialias=True)
            self.gl_view.addItem(orbit_line)

        self.gl_active_scatter = gl.GLScatterPlotItem()
        self.gl_view.addItem(self.gl_active_scatter)
        self.gl_dead_scatter = gl.GLScatterPlotItem()
        self.gl_view.addItem(self.gl_dead_scatter)
        self.gl_selected_scatter = gl.GLScatterPlotItem()
        self.gl_view.addItem(self.gl_selected_scatter)
        self.gl_ground_scatter = gl.GLScatterPlotItem()
        self.gl_view.addItem(self.gl_ground_scatter)
        
        self.gl_network_links = gl.GLLinePlotItem(mode='lines', color=(0.2, 0.3, 0.5, 0.3), width=1.5, antialias=True)
        self.gl_view.addItem(self.gl_network_links)
        
        self.gl_packet_path = gl.GLLinePlotItem(color=(1.0, 0.8, 0.1, 1.0), width=3, antialias=True)
        self.gl_view.addItem(self.gl_packet_path)

    def sat_selected(self, index):
        if index >= 0: self.selected_sat_id = index

    def trigger_kinetic_strike(self):
        self.status_msg = f"CRITICAL: {int(self.strike_severity*100)}% KINETIC LOSS DETECTED"
        # mark for convergence time tracking
        self.metrics.notify_failure(self.last_sim_time_s)
        active_ids = [s.node_id for s in self.sats if s.is_active]
        if not active_ids: return
        num_destroy = int(len(active_ids) * self.strike_severity)
        doomed_ids = random.sample(active_ids, num_destroy)
        
        if self.trace_src in doomed_ids: doomed_ids.remove(self.trace_src)
        if self.trace_dst in doomed_ids: doomed_ids.remove(self.trace_dst)
        
        for sat in self.sats:
            if sat.node_id in doomed_ids:
                sat.is_active = False
        # Routing/traffic will adapt automatically on next step.

    def reset_network(self):
        self.status_msg = "NOMINAL"
        for sat in self.sats:
            sat.is_active = True
        # Reset router + traffic state for a clean run
        self.router = self._make_router()
        self.traffic = TrafficSimulator(
            TrafficGenerator(pattern=TRAFFIC_PATTERN, rate_pps=TRAFFIC_RATE_PPS, hotspot_id=self.trace_dst)
        )

    def update_simulation(self):
        self.step_count += 1
        sim_time_s = self.step_count * self.sim_dt_seconds
        dt_s = float(self.sim_dt_seconds)
        self.last_sim_time_s = sim_time_s
        
        # 1) Physics propagate (ECI states)
        for sat in self.sats:
            sat.update_physics(dt_s)
            
        # 2) Build network graph (LOS + range) with link properties
        active_sats = [s for s in self.sats if s.is_active]
        active_ids = [s.node_id for s in active_sats]
        # Include ground nodes in the graph
        active_ids_all = list(active_ids) + list(self.ground_node_ids)
        r_eci_by_id = {s.node_id: s.state.r_km for s in active_sats}
        build_topology(
            self.graph,
            active_ids=active_ids,
            r_eci_by_id=r_eci_by_id,
            earth_radius_km=EARTH_RADIUS_KM,
            max_range_km=MAX_LINK_RANGE_KM,
            clearance_km=0.0,
            base_bandwidth_mbps=2000.0,
            base_loss_prob=0.0005,
            extra_latency_s=0.001,
            link_budget=lambda d_km, bw, loss: inverse_square_budget(d_km, bw, loss, ref_km=500.0),
        )

        # 2b) Add ground<->sat links using elevation mask + LOS
        # Compute sat ECEF once for this step.
        sat_ecef_by_id = {sid: eci_to_ecef(r_eci_by_id[sid], sim_time_s, theta0_rad=0.0) for sid in active_ids}
        ground_pts_scaled = []
        for gs in self.ground_nodes:
            gs_ecef = gs.ecef_position_km(EARTH_RADIUS_KM)
            ground_pts_scaled.append(gs_ecef / 1000.0)
            best = None
            best_el = -1e9
            for sid in active_ids:
                sat_ecef = sat_ecef_by_id[sid]
                if not is_visible_from_station_ecef(gs_ecef, sat_ecef, EARTH_RADIUS_KM, gs.min_elev_deg):
                    continue
                d_km = float(np.linalg.norm(sat_ecef - gs_ecef))
                if d_km > GROUND_SAT_MAX_RANGE_KM:
                    continue

                # Track best-elevation visible satellite (handover foundation)
                # Approx: maximize dot with up; reuse elevation function via visibility helper already checked.
                up = gs_ecef / float(np.linalg.norm(gs_ecef))
                rho = (sat_ecef - gs_ecef)
                rho_norm = float(np.linalg.norm(rho))
                el_sin = float(np.dot(rho / max(1e-9, rho_norm), up))
                if el_sin > best_el:
                    best_el = el_sin
                    best = sid

                # Build a link in the shared graph
                latency = d_km / SPEED_OF_LIGHT_KM_S + 0.003  # ground network adds extra processing
                lb = inverse_square_budget(d_km, GROUND_BW_Mbps, GROUND_BASE_LOSS, ref_km=800.0)
                props = LinkProperties(latency_s=float(latency), bandwidth_mbps=float(lb.bandwidth_mbps), loss_prob=float(lb.loss_prob))
                link = Link(a=gs.gid, b=sid, distance_km=float(d_km), props=props)
                self.graph.add_undirected(gs.gid, sid, link)

            self.best_sat_for_station[gs.gid] = best

        # 3) Apply failure/impairment models (blackouts, spikes, loss scaling)
        self.failure_model.apply(self.graph, dt_s=dt_s)

        # 4) Routing step (modular)
        self.router.step(self.graph, active_ids_all)

        # 5) Traffic step (packets + metrics)
        self.last_traffic_stats = self.traffic.step(
            graph=self.graph,
            router=self.router,
            active_ids=active_ids_all,
            now_t=sim_time_s,
            dt_s=dt_s,
        )
        self.last_step_metrics = self.metrics.step(
            graph=self.graph,
            router=self.router,
            active_ids=active_ids_all,
            traffic_stats=self.last_traffic_stats,
            now_s=sim_time_s,
        )

        # 6) Populate neighbor lists for inspector convenience
        for s in self.sats:
            s.neighbors = []
        for u in active_ids:
            self.sats[u].neighbors = [n for n in self.graph.neighbors(u).keys() if n < NUM_TOTAL_SATS]

        # 7) Render links from graph edges
        link_coords = []
        edge_set = set()
        for u, v, _link in self.graph.edges():
            edge_set.add((min(u, v), max(u, v)))
            if u < NUM_TOTAL_SATS:
                pu = self.sats[u].pos_3d
            else:
                pu = (self.ground_nodes[u - NUM_TOTAL_SATS].ecef_position_km(EARTH_RADIUS_KM) / 1000.0)
            if v < NUM_TOTAL_SATS:
                pv = self.sats[v].pos_3d
            else:
                pv = (self.ground_nodes[v - NUM_TOTAL_SATS].ecef_position_km(EARTH_RADIUS_KM) / 1000.0)
            if self.occlusion_enabled:
                mid = (np.asarray(pu) + np.asarray(pv)) * 0.5
                if self._is_occluded_by_earth(mid, earth_radius_world=EARTH_RADIUS_KM / 1000.0):
                    continue
            link_coords.append(pu)
            link_coords.append(pv)

        # Render ground stations
        if len(ground_pts_scaled) > 0:
            self.gl_ground_scatter.setData(pos=np.array(ground_pts_scaled), color=(0.58, 0.65, 1.0, 0.95), size=7, pxMode=True)
        else:
            self.gl_ground_scatter.setData(pos=np.empty((0, 3)))

        active_pts = np.array([s.pos_3d for s in self.sats if s.is_active])
        dead_pts = np.array([s.pos_3d for s in self.sats if not s.is_active])
        
        if len(active_pts) > 0: self.gl_active_scatter.setData(pos=active_pts, color=(0.24, 0.72, 0.31, 0.9), size=6, pxMode=True)
        else: self.gl_active_scatter.setData(pos=np.empty((0,3)))
            
        if len(dead_pts) > 0: self.gl_dead_scatter.setData(pos=dead_pts, color=(0.85, 0.21, 0.2, 0.8), size=8, pxMode=True)
        else: self.gl_dead_scatter.setData(pos=np.empty((0,3)))

        if len(link_coords) > 0: self.gl_network_links.setData(pos=np.array(link_coords))
        else: self.gl_network_links.setData(pos=np.empty((0,3)))

        path_trace = []
        if 0 <= self.trace_src < len(self.sats) and 0 <= self.trace_dst < len(self.sats):
            if self.sats[self.trace_src].is_active and self.sats[self.trace_dst].is_active:
                path_trace = self.router.path(self.trace_src, self.trace_dst, max_hops=256)
                
        if len(path_trace) > 1:
            pts = np.array([self.sats[pid].pos_3d for pid in path_trace], dtype=float)
            if self.occlusion_enabled:
                # keep only visible points (simple: drop whole path if midpoint occluded)
                mid = pts[len(pts) // 2]
                if self._is_occluded_by_earth(mid, earth_radius_world=EARTH_RADIUS_KM / 1000.0):
                    pts = np.empty((0, 3))
            self.gl_packet_path.setData(pos=pts)
        else: self.gl_packet_path.setData(pos=np.empty((0,3)))

        # --- Live topology dashboard (2D lat/lon) ---
        # Node positions
        sat_points = []
        for sid in active_ids:
            r_ecef = sat_ecef_by_id.get(sid)
            if r_ecef is None:
                continue
            lat_rad, lon_rad, _rmag = ecef_to_geodetic_spherical(r_ecef)
            sat_points.append({"pos": (math.degrees(lon_rad), math.degrees(lat_rad))})
        self.topo_nodes_item.setData(sat_points)

        ground_points = []
        for gs in self.ground_nodes:
            ground_points.append({"pos": (math.degrees(gs.lon_rad), math.degrees(gs.lat_rad))})
        self.topo_ground_item.setData(ground_points)

        # Edge segments
        # We draw edges using endpoint lat/lon of their nodes when possible.
        node_ll = {}
        for sid in active_ids:
            r_ecef = sat_ecef_by_id.get(sid)
            if r_ecef is None:
                continue
            lat_rad, lon_rad, _ = ecef_to_geodetic_spherical(r_ecef)
            node_ll[sid] = (math.degrees(lon_rad), math.degrees(lat_rad))
        for gs in self.ground_nodes:
            node_ll[gs.gid] = (math.degrees(gs.lon_rad), math.degrees(gs.lat_rad))

        ex, ey = [], []
        for (u, v) in edge_set:
            if u not in node_ll or v not in node_ll:
                continue
            (x1, y1), (x2, y2) = node_ll[u], node_ll[v]
            ex += [x1, x2]
            ey += [y1, y2]
        self.topo_edges_item.setData(ex, ey, connect="pairs")

        # Failed links (diff between previous and current)
        removed = self.prev_edge_set - edge_set
        for (u, v) in removed:
            self.failed_edges.append((u, v, 40))
        self.prev_edge_set = set(edge_set)

        fx, fy = [], []
        new_failed = []
        for (u, v, ttl) in self.failed_edges:
            if ttl <= 0:
                continue
            if u in node_ll and v in node_ll:
                (x1, y1), (x2, y2) = node_ll[u], node_ll[v]
                fx += [x1, x2]
                fy += [y1, y2]
            new_failed.append((u, v, ttl - 1))
        self.failed_edges = new_failed
        self.topo_failed_item.setData(fx, fy, connect="pairs")

        # Active route highlight (sat->sat tracer)
        rx, ry = [], []
        if len(path_trace) > 1:
            for a, b in zip(path_trace[:-1], path_trace[1:]):
                if a in node_ll and b in node_ll:
                    (x1, y1), (x2, y2) = node_ll[a], node_ll[b]
                    rx += [x1, x2]
                    ry += [y1, y2]
        self.topo_route_item.setData(rx, ry, connect="pairs")

        # --- Metrics plots ---
        if self.last_step_metrics is not None:
            m = self.last_step_metrics
            h = self.metric_hist
            h["t"].append(float(sim_time_s))
            h["delivery"].append(float(m.delivery_ratio) * 100.0)
            h["delay"].append(float(m.avg_delay_s))
            h["util"].append(float(m.avg_link_utilization) * 100.0)
            # trim
            if len(h["t"]) > TOPOLOGY_HISTORY_STEPS:
                for k in list(h.keys()):
                    h[k] = h[k][-TOPOLOGY_HISTORY_STEPS :]

            x = np.arange(len(h["t"]))
            self.curve_delivery.setData(x, h["delivery"])
            self.curve_delay.setData(x, h["delay"])
            self.curve_util.setData(x, h["util"])

        # Update Text
        color_hex = "#ff7b72" if "CRITICAL" in self.status_msg else "#3fb950"
        if self.last_traffic_stats is not None:
            t = self.last_traffic_stats
            m = self.last_step_metrics
            traffic_html = (
                f"<b>Traffic:</b> in-flight={t.in_flight}, delivered={t.delivered}, dropped={t.dropped}<br>"
                f"<b>Avg Delay:</b> {t.avg_delay_s:.3f} s &nbsp;&nbsp; <b>Avg Hops:</b> {t.avg_hops:.2f}<br>"
                f"<b>Delivery Ratio:</b> {m.delivery_ratio*100:.1f}% &nbsp;&nbsp; <b>Avg Path:</b> {m.avg_path_len_hops:.2f} hops<br>"
                f"<b>Link Util:</b> {m.avg_link_utilization*100:.1f}% &nbsp;&nbsp; <b>Route Updates:</b> {m.route_updates}<br>"
                f"<b>Routing:</b> {self.router.name} ({ROUTING_COST})<br><br>"
            )
        else:
            traffic_html = ""
        self.lbl_telemetry.setText(
            f"<b>GLOBAL TELEMETRY</b><br><br>"
            f"<b>Time Elapsed :</b> T+ {sim_time_s:.1f} s<br>"
            f"<b>Net Status   :</b> <span style='color:{color_hex}'>{self.status_msg}</span><br>"
            f"<b>Active Nodes :</b> {len(active_pts)} / {NUM_TOTAL_SATS}<br>"
            f"<b>Laser Links  :</b> {len(link_coords)//2}<br><br>"
            f"{traffic_html}"
            f"<b>Active Tracer:</b><br>Targeting node {self.trace_dst} from {self.trace_src}"
        )
        
        if self.selected_sat_id is not None:
            sat = self.sats[self.selected_sat_id]
            self.gl_selected_scatter.setData(pos=np.array([sat.pos_3d]), color=(1.0, 1.0, 1.0, 1.0), size=12, pxMode=True)
            
            s_color = "#3fb950" if sat.is_active else "#da3633"
            s_text = "ONLINE" if sat.is_active else "OFFLINE"
            try:
                coe = sat.coe
                a_km = coe.a_km
                e = coe.e
                inc_deg = math.degrees(coe.i_rad)
                raan_deg = math.degrees(coe.raan_rad)
            except Exception:
                a_km, e, inc_deg, raan_deg = float("nan"), float("nan"), float("nan"), float("nan")
            try:
                r_ecef = eci_to_ecef(sat.state.r_km, sim_time_s, theta0_rad=0.0)
                lat_rad, lon_rad, rmag_km = ecef_to_geodetic_spherical(r_ecef)
                lat_deg = math.degrees(lat_rad)
                lon_deg = (math.degrees(lon_rad) + 540.0) % 360.0 - 180.0
                alt_km = rmag_km - EARTH_RADIUS_KM
            except Exception:
                lat_deg, lon_deg, alt_km = float("nan"), float("nan"), float("nan")
            self.lbl_spec.setText(
                f"<span style='color:#58a6ff; font-weight:bold;'>{sat.name}</span><br><hr>"
                f"<b>Status:</b> <span style='color:{s_color};'>{s_text}</span><br>"
                f"<b>Plane ID:</b> {sat.plane_idx}<br>"
                f"<b>Orbit:</b> a={a_km:.1f} km, e={e:.4f}, i={inc_deg:.1f}°, Ω={raan_deg:.1f}°<br>"
                f"<b>True Anomaly:</b> {sat.nu_rad:.2f} rad<br>"
                f"<b>Subpoint (lat/lon/alt):</b> {lat_deg:.2f}°, {lon_deg:.2f}°, {alt_km:.1f} km<br>"
                f"<b>Neighbors:</b> {len(sat.neighbors)} connected<br>"
                f"<b>Routing:</b> {self.router.name} ({ROUTING_COST})<br><br>"
                f"<b>ECI Position (km):</b><br>"
                f"[{sat.pos_3d[0]*1000:.1f}, {sat.pos_3d[1]*1000:.1f}, {sat.pos_3d[2]*1000:.1f}]<br><br>"
                f"<b>ECI Velocity (km/s):</b><br>"
                f"[{sat.vel_eci_km_s[0]:.3f}, {sat.vel_eci_km_s[1]:.3f}, {sat.vel_eci_km_s[2]:.3f}]"
            )

        # CSV logging (one row per step)
        if self.run_logger is not None and self.last_traffic_stats is not None:
            t = self.last_traffic_stats
            m = self.last_step_metrics
            self.run_logger.log_row(
                {
                    "time_s": float(sim_time_s),
                    "active_sats": int(len(active_pts)),
                    "links_total": int(len(link_coords) // 2),
                    "router": str(self.router.name),
                    "routing_cost": str(ROUTING_COST),
                    "traffic_pattern": str(TRAFFIC_PATTERN),
                    "in_flight": int(t.in_flight),
                    "delivered_total": int(t.delivered),
                    "dropped_total": int(t.dropped),
                    "avg_delay_s": float(t.avg_delay_s),
                    "avg_hops": float(t.avg_hops),
                    "delivery_ratio": float(m.delivery_ratio) if m is not None else 0.0,
                    "avg_path_len_hops": float(m.avg_path_len_hops) if m is not None else 0.0,
                    "avg_link_utilization": float(m.avg_link_utilization) if m is not None else 0.0,
                    "route_updates": int(m.route_updates) if m is not None else 0,
                    "convergence_s": float(m.convergence_s) if (m is not None and m.convergence_s is not None) else "",
                }
            )

if __name__ == '__main__':
    app = QApplication(sys.argv)
    app.setStyleSheet(DARK_THEME) # Apply global dark theme
    window = MainWindow()
    window.show()
    sys.exit(app.exec())