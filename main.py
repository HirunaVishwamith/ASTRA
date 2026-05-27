import sys
import math
import random
import numpy as np

from PyQt6.QtCore import QTimer, Qt
from PyQt6.QtWidgets import (QApplication, QMainWindow, QWidget, QHBoxLayout, 
                             QVBoxLayout, QPushButton, QLabel, QListWidget, QSplitter)

import pyqtgraph.opengl as gl

# --- Realistic Physical Constants (WGS84 Standards) ---
EARTH_RADIUS_KM = 6378.137 
MU_EARTH = 398600.4418 

# --- Constellation Parameters (Starlink-Style Walker Delta) ---
LEO_ALTITUDE_KM = 550.0
SAT_ALTITUDE_KM = EARTH_RADIUS_KM + LEO_ALTITUDE_KM 
MAX_LINK_RANGE_KM = 2500.0  

NUM_PLANES = 10
NUM_SATS_PER_PLANE = 10
NUM_TOTAL_SATS = NUM_PLANES * NUM_SATS_PER_PLANE
INCLINATION_RAD = math.radians(53.0) 

SIM_DT_SECONDS = 5.0 
FAILURE_RATE = 0.20 

class SatelliteAgent:
    def __init__(self, node_id, orbit_plane_idx, phase_offset_rad):
        self.node_id = node_id
        self.name = f"SAT-{orbit_plane_idx:02d}-{sat_idx_name(node_id):02d}"
        self.plane_idx = orbit_plane_idx
        
        # Keplerian Elements
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
        
        # Normalize coordinates down slightly for pyqtgraph viewport scaling (~1 unit = 1000km)
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
            if current_path and current_path[0] <= neighbor_cost + link_cost:
                continue
                
            self.routing_table[dest_id] = (neighbor_cost + link_cost, neighbor_id)
            updated = True
            
        if updated: self.table_changed = True

    def get_next_hop(self, dest_id):
        if not self.is_active: return None
        path = self.routing_table.get(dest_id)
        return path[1] if path else None

def sat_idx_name(node_id):
    return node_id % NUM_SATS_PER_PLANE


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("SpaceXAI - Ultra Fast OpenGL Constellation Router")
        self.resize(1400, 900)
        
        # Initialize Core Data
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

        # --- UI LAYOUT ---
        main_widget = QWidget()
        self.setCentralWidget(main_widget)
        main_layout = QHBoxLayout(main_widget)
        
        splitter = QSplitter(Qt.Orientation.Horizontal)
        main_layout.addWidget(splitter)
        
        # Left Panel: 3D OpenGL View
        left_container = QWidget()
        left_layout = QVBoxLayout(left_container)
        
        self.gl_view = gl.GLViewWidget()
        self.gl_view.setCameraPosition(distance=25, elevation=30, azimuth=40)
        left_layout.addWidget(self.gl_view)
        
        # Controls Group
        ctrl_layout = QHBoxLayout()
        self.btn_strike = QPushButton("Trigger Kinetic Strike")
        self.btn_strike.setStyleSheet("background-color: #5e0000; color: white; font-weight: bold; padding: 10px;")
        self.btn_strike.clicked.connect(self.trigger_kinetic_strike)
        
        self.btn_reset = QPushButton("Heal / Reset Network")
        self.btn_reset.setStyleSheet("background-color: #004d1a; color: white; font-weight: bold; padding: 10px;")
        self.btn_reset.clicked.connect(self.reset_network)
        
        ctrl_layout.addWidget(self.btn_strike)
        ctrl_layout.addWidget(self.btn_reset)
        left_layout.addLayout(ctrl_layout)
        
        splitter.addWidget(left_container)
        
        # Right Panel: Real-Time Telemetry and Interactive List
        right_container = QWidget()
        right_layout = QVBoxLayout(right_container)
        right_container.setMaximumWidth(400)
        
        self.lbl_telemetry = QLabel("System Status: INIT\nTime: 0s")
        self.lbl_telemetry.setStyleSheet("font-family: monospace; font-size: 12px; color: #00ff41; background-color: black; padding: 10px; border-radius: 5px;")
        right_layout.addWidget(self.lbl_telemetry)
        
        right_layout.addWidget(QLabel("<b>Select Satellite for Individual Specs:</b>"))
        self.sat_list_widget = QListWidget()
        self.sat_list_widget.currentRowChanged.connect(self.sat_selected)
        right_layout.addWidget(self.sat_list_widget)
        
        self.lbl_spec = QLabel("Select a satellite to read telemetry...")
        self.lbl_spec.setStyleSheet("font-family: monospace; font-size: 11px; background-color: #111; padding: 10px; border: 1px solid #333;")
        self.lbl_spec.setWordWrap(True)
        right_layout.addWidget(self.lbl_spec)
        
        splitter.addWidget(right_container)
        
        self.populate_satellite_list()
        self.init_gl_objects()
        
        # High Frequency Refresh Timer (Runs effortlessly at 60Hz due to hardware acceleration)
        self.timer = QTimer()
        self.timer.timeout.connect(self.update_simulation)
        self.timer.start(16) # ~60 Frames Per Second

    def populate_satellite_list(self):
        for sat in self.sats:
            self.sat_list_widget.addItem(f"{sat.name} (ID: {sat.node_id})")

    def init_gl_objects(self):
        # 1. Earth Representation Sphere
        earth_rad_scaled = EARTH_RADIUS_KM / 1000.0
        md = gl.MeshData.sphere(rows=20, cols=20, radius=earth_rad_scaled)
        self.earth_mesh = gl.GLMeshItem(meshdata=md, smooth=True, color=(0.1, 0.15, 0.3, 0.6), shader='shaded', glOptions='translucent')
        self.gl_view.addItem(self.earth_mesh)
        
        # 2. Add Permanent Orbital Paths Line Traces
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
            orbit_line = gl.GLLinePlotItem(pos=path_pts, color=(0.2, 0.2, 0.3, 0.3), width=1, antialias=True)
            self.gl_view.addItem(orbit_line)

        # 3. Dynamic Node Scattered Points
        self.gl_active_scatter = gl.GLScatterPlotItem()
        self.gl_view.addItem(self.gl_active_scatter)
        
        self.gl_dead_scatter = gl.GLScatterPlotItem()
        self.gl_view.addItem(self.gl_dead_scatter)
        
        self.gl_selected_scatter = gl.GLScatterPlotItem()
        self.gl_view.addItem(self.gl_selected_scatter)
        
        # 4. Mesh Network Inter-satellite Laser Links
        self.gl_network_links = gl.GLLinePlotItem(mode='lines', color=(0.2, 0.2, 0.4, 0.2), width=1, antialias=True)
        self.gl_view.addItem(self.gl_network_links)
        
        # 5. Routing Packet Highlighter Path
        self.gl_packet_path = gl.GLLinePlotItem(color=(1.0, 0.86, 0.0, 0.9), width=3, antialias=True)
        self.gl_view.addItem(self.gl_packet_path)

    def sat_selected(self, index):
        if index >= 0:
            self.selected_sat_id = index

    def trigger_kinetic_strike(self):
        self.status_msg = "RECOVERING (CRITICAL 20% LOSS)"
        active_ids = [s.node_id for s in self.sats if s.is_active]
        if not active_ids: return
        num_destroy = int(len(active_ids) * FAILURE_RATE)
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
        sim_time_s = self.step_count * SIM_DT_SECONDS
        
        # 1. Physics Frame Update
        for sat in self.sats:
            sat.update_physics(SIM_DT_SECONDS)
            
        # 2. Topology Scan (Laser Line of Sight Paths)
        active_sats = [s for s in self.sats if s.is_active]
        for s in active_sats: s.neighbors = []
        
        link_coords = []
        for i in range(len(active_sats)):
            for j in range(i + 1, len(active_sats)):
                sat_a = active_sats[i]
                sat_b = active_sats[j]
                # Multiply back to raw KM scale for precise mathematical check
                dist = np.linalg.norm(sat_a.pos_3d - sat_b.pos_3d) * 1000.0
                if dist <= MAX_LINK_RANGE_KM:
                    sat_a.neighbors.append(sat_b.node_id)
                    sat_b.neighbors.append(sat_a.node_id)
                    link_coords.append(sat_a.pos_3d)
                    link_coords.append(sat_b.pos_3d)

        # 3. Dynamic Multi-Agent Consensus Phases
        # 3. Dynamic Multi-Agent Consensus Phases
        for _ in range(3):
            # Safe population loop to avoid double-evaluation side effects
            all_broadcasts = {}
            for s in active_sats:
                table = s.broadcast_table()
                if table is not None:
                    all_broadcasts[s.node_id] = table

            for sat in active_sats:
                for n_id in sat.neighbors:
                    # Added a defensive check to ensure neighbor table is valid
                    if n_id in all_broadcasts and all_broadcasts[n_id] is not None:
                        sat.process_neighbor_update(n_id, all_broadcasts[n_id])

        # 4. Process Graphics Buffers via Hardware Instancing
        active_pts = np.array([s.pos_3d for s in self.sats if s.is_active])
        dead_pts = np.array([s.pos_3d for s in self.sats if not s.is_active])
        
        if len(active_pts) > 0:
            self.gl_active_scatter.setData(pos=active_pts, color=(0.0, 1.0, 0.25, 0.8), size=7, pxMode=True)
        else:
            self.gl_active_scatter.setData(pos=np.empty((0,3)))
            
        if len(dead_pts) > 0:
            self.gl_dead_scatter.setData(pos=dead_pts, color=(1.0, 0.0, 0.2, 0.7), size=9, pxMode=True)
        else:
            self.gl_dead_scatter.setData(pos=np.empty((0,3)))

        if len(link_coords) > 0:
            self.gl_network_links.setData(pos=np.array(link_coords))
        else:
            self.gl_network_links.setData(pos=np.empty((0,3)))

        # 5. Packet Tracer Updates
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
                
        if len(path_trace) > 1:
            trace_line_pts = np.array([self.sats[pid].pos_3d for pid in path_trace])
            self.gl_packet_path.setData(pos=trace_line_pts)
        else:
            self.gl_packet_path.setData(pos=np.empty((0,3)))

        # 6. Update GUI Text Panels
        num_act = len(active_pts)
        num_dead = len(dead_pts)
        self.lbl_telemetry.setText(
            f"--- MAIN CONSTELLATION TELEMETRY ---\n"
            f"Sim Steps  : {self.step_count}\n"
            f"Mission T+ : {sim_time_s:.1f} s\n"
            f"Net Status : {self.status_msg}\n"
            f"Sat Active : {num_act} / {NUM_TOTAL_SATS}\n"
            f"Sat Offline: {num_dead}\n"
            f"Active Links: {len(link_coords)//2}\n"
            f"Tracing Log: Sat {self.trace_src} ➔ Sat {self.trace_dst}"
        )
        
        # 7. Update Highlight Target Specs
        if self.selected_sat_id is not None:
            sat = self.sats[self.selected_sat_id]
            self.gl_selected_scatter.setData(pos=np.array([sat.pos_3d]), color=(1.0, 1.0, 1.0, 1.0), size=15, pxMode=True)
            
            status_str = "<font color='#00ff00'>ONLINE</font>" if sat.is_active else "<font color='#ff0000'>OFFLINE (DESTROYED)</font>"
            self.lbl_spec.setText(
                f"<b>SPECIFICATION SHEET - {sat.name}</b><br><br>"
                f"<b>Node Identifier:</b> {sat.node_id}<br>"
                f"<b>Orbital Plane:</b> Vector Ring #{sat.plane_idx}<br>"
                f"<b>Status Flag:</b> {status_str}<br>"
                f"<b>Mean Motion V:</b> {sat.omega*1000:.4f} mrad/s<br>"
                f"<b>True Anomaly Rad:</b> {sat.nu_rad:.4f} rad<br>"
                f"<b>Current Vector (ECI):</b><br>"
                f"&nbsp;&nbsp;X: {sat.pos_3d[0]*1000:.1f} km<br>"
                f"&nbsp;&nbsp;Y: {sat.pos_3d[1]*1000:.1f} km<br>"
                f"&nbsp;&nbsp;Z: {sat.pos_3d[2]*1000:.1f} km<br><br>"
                f"<b>Local Neighbors:</b> {len(sat.neighbors)} connected<br>"
                f"<b>Routing Knowledge:</b> {len(sat.routing_table)} known nodes"
            )
        else:
            self.gl_selected_scatter.setData(pos=np.empty((0,3)))


if __name__ == '__main__':
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec())