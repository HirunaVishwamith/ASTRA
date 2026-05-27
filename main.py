import sys
import math
import random
import numpy as np

from PyQt6.QtCore import QTimer, Qt
from PyQt6.QtWidgets import (QApplication, QMainWindow, QWidget, QHBoxLayout, 
                             QVBoxLayout, QPushButton, QLabel, QListWidget, 
                             QSplitter, QGroupBox, QSlider)

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
from network_model import NetworkGraph, build_topology
from routing import DijkstraRouter, DistanceVectorRouter, Router
from traffic import TrafficGenerator, TrafficSimulator
from failures import FailureModel, LinkImpairments

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

        # Failure/impairment knobs (expose in UI later if needed)
        self.failure_model = FailureModel(
            LinkImpairments(
                blackout_prob_per_s=0.0000,
                latency_spike_prob_per_s=0.0000,
                latency_spike_add_s=0.030,
                loss_multiplier=1.0,
            )
        )

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
        
        btn_layout.addWidget(self.btn_strike)
        btn_layout.addWidget(self.btn_reset)
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
        
        splitter.addWidget(right_container)
        splitter.setSizes([1100, 500])

    def update_speed(self, value):
        self.sim_dt_seconds = float(value)
        self.lbl_speed.setText(f"Simulation Speed (ΔT): {self.sim_dt_seconds}x")

    def update_severity(self, value):
        self.strike_severity = value / 100.0
        self.lbl_severity.setText(f"Strike Severity: {value}% Node Loss")

    def init_gl_objects(self):
        earth_rad_scaled = EARTH_RADIUS_KM / 1000.0
        md = gl.MeshData.sphere(rows=30, cols=30, radius=earth_rad_scaled)
        self.earth_mesh = gl.GLMeshItem(meshdata=md, smooth=True, color=(0.08, 0.12, 0.2, 0.7), shader='shaded', glOptions='translucent')
        self.gl_view.addItem(self.earth_mesh)
        
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
        
        self.gl_network_links = gl.GLLinePlotItem(mode='lines', color=(0.2, 0.3, 0.5, 0.3), width=1.5, antialias=True)
        self.gl_view.addItem(self.gl_network_links)
        
        self.gl_packet_path = gl.GLLinePlotItem(color=(1.0, 0.8, 0.1, 1.0), width=3, antialias=True)
        self.gl_view.addItem(self.gl_packet_path)

    def sat_selected(self, index):
        if index >= 0: self.selected_sat_id = index

    def trigger_kinetic_strike(self):
        self.status_msg = f"CRITICAL: {int(self.strike_severity*100)}% KINETIC LOSS DETECTED"
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
        )

        # 3) Apply failure/impairment models (blackouts, spikes, loss scaling)
        self.failure_model.apply(self.graph, dt_s=dt_s)

        # 4) Routing step (modular)
        self.router.step(self.graph, active_ids)

        # 5) Traffic step (packets + metrics)
        self.last_traffic_stats = self.traffic.step(
            graph=self.graph,
            router=self.router,
            active_ids=active_ids,
            now_t=sim_time_s,
            dt_s=dt_s,
        )

        # 6) Populate neighbor lists for inspector convenience
        for s in self.sats:
            s.neighbors = []
        for u in active_ids:
            self.sats[u].neighbors = list(self.graph.neighbors(u).keys())

        # 7) Render links from graph edges
        link_coords = []
        for u, v, _link in self.graph.edges():
            link_coords.append(self.sats[u].pos_3d)
            link_coords.append(self.sats[v].pos_3d)

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
                
        if len(path_trace) > 1: self.gl_packet_path.setData(pos=np.array([self.sats[pid].pos_3d for pid in path_trace]))
        else: self.gl_packet_path.setData(pos=np.empty((0,3)))

        # Update Text
        color_hex = "#ff7b72" if "CRITICAL" in self.status_msg else "#3fb950"
        if self.last_traffic_stats is not None:
            t = self.last_traffic_stats
            traffic_html = (
                f"<b>Traffic:</b> in-flight={t.in_flight}, delivered={t.delivered}, dropped={t.dropped}<br>"
                f"<b>Avg Delay:</b> {t.avg_delay_s:.3f} s &nbsp;&nbsp; <b>Avg Hops:</b> {t.avg_hops:.2f}<br>"
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

if __name__ == '__main__':
    app = QApplication(sys.argv)
    app.setStyleSheet(DARK_THEME) # Apply global dark theme
    window = MainWindow()
    window.show()
    sys.exit(app.exec())