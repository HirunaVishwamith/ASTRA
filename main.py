import sys
import math
import random
import numpy as np

from PyQt6.QtCore import QTimer, Qt
from PyQt6.QtWidgets import (QApplication, QMainWindow, QWidget, QHBoxLayout, 
                             QVBoxLayout, QPushButton, QLabel, QListWidget, 
                             QSplitter, QGroupBox, QSlider)

import pyqtgraph.opengl as gl

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
        
        self.nu_rad = phase_offset_rad 
        self.omega = math.sqrt(MU_EARTH / SAT_ALTITUDE_KM**3) 
        self.RAAN_rad = (2 * math.pi / NUM_PLANES) * self.plane_idx 
        self.inclination = INCLINATION_RAD
        
        self.pos_3d = np.zeros(3)
        self.is_active = True
        self.neighbors = [] 
        self.routing_table = {self.node_id: (0, self.node_id)} 
        self.table_changed = True 

    def update_physics(self, dt_seconds):
        if not self.is_active: return
        self.nu_rad += self.omega * dt_seconds
        
        r = SAT_ALTITUDE_KM
        Om = self.RAAN_rad
        inc = self.inclination
        nu = self.nu_rad
        
        x = r * (math.cos(Om) * math.cos(nu) - math.sin(Om) * math.sin(nu) * math.cos(inc))
        y = r * (math.sin(Om) * math.cos(nu) + math.cos(Om) * math.sin(nu) * math.cos(inc))
        z = r * (math.sin(nu) * math.sin(inc))
        
        self.pos_3d = np.array([x, y, z]) / 1000.0

    def broadcast_table(self):
        if not self.is_active or not self.table_changed: return None
        self.table_changed = False
        return self.routing_table

    def process_neighbor_update(self, neighbor_id, neighbor_table):
        if not self.is_active: return
        updated = False
        link_cost = 1 
        
        to_delete = [dst for dst, (cost, n_hop) in self.routing_table.items() if n_hop == neighbor_id]
        for dst in to_delete:
            del self.routing_table[dst]
            updated = True
            
        self.routing_table[self.node_id] = (0, self.node_id)

        for dest_id, (neighbor_cost, _) in neighbor_table.items():
            current_path = self.routing_table.get(dest_id)
            if current_path and current_path[0] <= neighbor_cost + link_cost: continue
            self.routing_table[dest_id] = (neighbor_cost + link_cost, neighbor_id)
            updated = True
            
        if updated: self.table_changed = True

    def get_next_hop(self, dest_id):
        if not self.is_active: return None
        path = self.routing_table.get(dest_id)
        return path[1] if path else None

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
        
        # Dynamic Parameters
        self.sim_dt_seconds = 5.0
        self.strike_severity = 0.20

        self.setup_ui()
        self.init_gl_objects()
        
        self.timer = QTimer()
        self.timer.timeout.connect(self.update_simulation)
        self.timer.start(16) 

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
                sat.routing_table = {}
                
        for sat in self.sats: 
            if sat.is_active: sat.routing_table = {sat.node_id: (0, sat.node_id)}

    def reset_network(self):
        self.status_msg = "NOMINAL"
        for sat in self.sats:
            sat.is_active = True
            sat.routing_table = {sat.node_id: (0, sat.node_id)}
            sat.table_changed = True

    def update_simulation(self):
        self.step_count += 1
        sim_time_s = self.step_count * self.sim_dt_seconds
        
        for sat in self.sats: sat.update_physics(self.sim_dt_seconds)
            
        active_sats = [s for s in self.sats if s.is_active]
        for s in active_sats: s.neighbors = []
        
        link_coords = []
        for i in range(len(active_sats)):
            for j in range(i + 1, len(active_sats)):
                sat_a = active_sats[i]
                sat_b = active_sats[j]
                dist = np.linalg.norm(sat_a.pos_3d - sat_b.pos_3d) * 1000.0
                if dist <= MAX_LINK_RANGE_KM:
                    sat_a.neighbors.append(sat_b.node_id)
                    sat_b.neighbors.append(sat_a.node_id)
                    link_coords.append(sat_a.pos_3d)
                    link_coords.append(sat_b.pos_3d)

        for _ in range(3):
            all_broadcasts = {}
            for s in active_sats:
                table = s.broadcast_table()
                if table is not None:
                    all_broadcasts[s.node_id] = table

            for sat in active_sats:
                for n_id in sat.neighbors:
                    if n_id in all_broadcasts and all_broadcasts[n_id] is not None:
                        sat.process_neighbor_update(n_id, all_broadcasts[n_id])

        active_pts = np.array([s.pos_3d for s in self.sats if s.is_active])
        dead_pts = np.array([s.pos_3d for s in self.sats if not s.is_active])
        
        if len(active_pts) > 0: self.gl_active_scatter.setData(pos=active_pts, color=(0.24, 0.72, 0.31, 0.9), size=6, pxMode=True)
        else: self.gl_active_scatter.setData(pos=np.empty((0,3)))
            
        if len(dead_pts) > 0: self.gl_dead_scatter.setData(pos=dead_pts, color=(0.85, 0.21, 0.2, 0.8), size=8, pxMode=True)
        else: self.gl_dead_scatter.setData(pos=np.empty((0,3)))

        if len(link_coords) > 0: self.gl_network_links.setData(pos=np.array(link_coords))
        else: self.gl_network_links.setData(pos=np.empty((0,3)))

        path_trace = []
        if self.sats[self.trace_src].is_active:
            path_trace = [self.trace_src]
            curr = self.trace_src
            visited = {curr}
            while curr != self.trace_dst:
                nxt = self.sats[curr].get_next_hop(self.trace_dst)
                if nxt is None or nxt in visited: break
                path_trace.append(nxt)
                visited.add(nxt)
                curr = nxt
                
        if len(path_trace) > 1: self.gl_packet_path.setData(pos=np.array([self.sats[pid].pos_3d for pid in path_trace]))
        else: self.gl_packet_path.setData(pos=np.empty((0,3)))

        # Update Text
        color_hex = "#ff7b72" if "CRITICAL" in self.status_msg else "#3fb950"
        self.lbl_telemetry.setText(
            f"<b>GLOBAL TELEMETRY</b><br><br>"
            f"<b>Time Elapsed :</b> T+ {sim_time_s:.1f} s<br>"
            f"<b>Net Status   :</b> <span style='color:{color_hex}'>{self.status_msg}</span><br>"
            f"<b>Active Nodes :</b> {len(active_pts)} / {NUM_TOTAL_SATS}<br>"
            f"<b>Laser Links  :</b> {len(link_coords)//2}<br><br>"
            f"<b>Active Tracer:</b><br>Targeting node {self.trace_dst} from {self.trace_src}"
        )
        
        if self.selected_sat_id is not None:
            sat = self.sats[self.selected_sat_id]
            self.gl_selected_scatter.setData(pos=np.array([sat.pos_3d]), color=(1.0, 1.0, 1.0, 1.0), size=12, pxMode=True)
            
            s_color = "#3fb950" if sat.is_active else "#da3633"
            s_text = "ONLINE" if sat.is_active else "OFFLINE"
            self.lbl_spec.setText(
                f"<span style='color:#58a6ff; font-weight:bold;'>{sat.name}</span><br><hr>"
                f"<b>Status:</b> <span style='color:{s_color};'>{s_text}</span><br>"
                f"<b>Plane ID:</b> {sat.plane_idx}<br>"
                f"<b>True Anomaly:</b> {sat.nu_rad:.2f} rad<br>"
                f"<b>Neighbors:</b> {len(sat.neighbors)} connected<br>"
                f"<b>Routing Db:</b> {len(sat.routing_table)} known paths<br><br>"
                f"<b>Vector (ECI X/Y/Z):</b><br>"
                f"[{sat.pos_3d[0]*1000:.1f}, {sat.pos_3d[1]*1000:.1f}, {sat.pos_3d[2]*1000:.1f}] km"
            )

if __name__ == '__main__':
    app = QApplication(sys.argv)
    app.setStyleSheet(DARK_THEME) # Apply global dark theme
    window = MainWindow()
    window.show()
    sys.exit(app.exec())