import numpy as np
import math
import random
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from matplotlib.widgets import Button
from mpl_toolkits.mplot3d.art3d import Line3DCollection

# --- Realistic Physical Constants (WGS84 Standards) ---
EARTH_RADIUS_KM = 6378.137 # Equatorial radius
EARTH_MASS_KG = 5.9722e24
G_CONST = 6.67430e-20 # km^3 / (kg * s^2)
MU_EARTH = 398600.4418 # km^3/s^2 (Standard gravitational parameter)

# --- Constellation Parameters (Starlink-Style Walker Delta) ---
LEO_ALTITUDE_KM = 550.0
SAT_ALTITUDE_KM = EARTH_RADIUS_KM + LEO_ALTITUDE_KM 
MAX_LINK_RANGE_KM = 2500.0  # Laser link constraint (prevents Earth clipping)

NUM_PLANES = 10
NUM_SATS_PER_PLANE = 10
NUM_TOTAL_SATS = NUM_PLANES * NUM_SATS_PER_PLANE
INCLINATION_RAD = math.radians(53.0) # Typical Phase 1 LEO inclination

SIM_DT_SECONDS = 10.0 # Time step
FAILURE_RATE = 0.20 

class SatelliteAgent:
    def __init__(self, node_id, orbit_plane_idx, phase_offset_rad):
        self.node_id = node_id
        self.plane_idx = orbit_plane_idx
        
        # Keplerian Orbital Elements
        self.nu_rad = phase_offset_rad # True Anomaly (phase along orbit)
        self.omega = math.sqrt(MU_EARTH / SAT_ALTITUDE_KM**3) # Mean motion (rad/s)
        self.RAAN_rad = (2 * math.pi / NUM_PLANES) * self.plane_idx # Right Ascension
        self.inclination = INCLINATION_RAD
        
        self.pos_3d = np.zeros(3)
        self.is_active = True
        self.neighbors = [] 
        
        self.routing_table = {self.node_id: (0, self.node_id)} 
        self.table_changed = True 

    def update_physics(self, dt_seconds):
        """First-principles conversion from Keplerian to Cartesian ECI coordinates."""
        if not self.is_active: return
        
        # Advance the true anomaly based on orbital velocity
        self.nu_rad += self.omega * dt_seconds
        
        # Compute ECI (Earth-Centered Inertial) 3D coordinates
        r = SAT_ALTITUDE_KM
        Om = self.RAAN_rad
        inc = self.inclination
        nu = self.nu_rad
        
        x = r * (math.cos(Om) * math.cos(nu) - math.sin(Om) * math.sin(nu) * math.cos(inc))
        y = r * (math.sin(Om) * math.cos(nu) + math.cos(Om) * math.sin(nu) * math.cos(inc))
        z = r * (math.sin(nu) * math.sin(inc))
        
        self.pos_3d = np.array([x, y, z])

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

# --- 3D Constellation Environment & Visualization ---

class ConstellationViz3D:
    def __init__(self):
        self.sats = []
        for plane in range(NUM_PLANES):
            for sat_idx in range(NUM_SATS_PER_PLANE):
                node_id = plane * NUM_SATS_PER_PLANE + sat_idx
                # Apply phase shift between planes to create a Walker Delta mesh
                phase_offset = (2 * math.pi / NUM_SATS_PER_PLANE) * sat_idx + (plane * 0.1)
                self.sats.append(SatelliteAgent(node_id, plane, phase_offset))

        self.fig = plt.figure(figsize=(12, 10))
        self.fig.canvas.manager.set_window_title('SpaceXAI 3D Constellation Router')
        
        # Setup 3D Axis
        self.ax = self.fig.add_subplot(111, projection='3d')
        plt.subplots_adjust(bottom=0.15, left=0.0, right=0.75) # Room for UI and stats
        self.ax.set_facecolor('#05050f') 
        self.fig.patch.set_facecolor('#05050f')
        
        # Remove grid and axes for deep space look
        self.ax.grid(False)
        self.ax.set_axis_off()
        
        limit = SAT_ALTITUDE_KM * 1.1
        self.ax.set_xlim([-limit, limit])
        self.ax.set_ylim([-limit, limit])
        self.ax.set_zlim([-limit, limit])

        # Draw 3D Earth
        u = np.linspace(0, 2 * np.pi, 40)
        v = np.linspace(0, np.pi, 40)
        x_e = EARTH_RADIUS_KM * np.outer(np.cos(u), np.sin(v))
        y_e = EARTH_RADIUS_KM * np.outer(np.sin(u), np.sin(v))
        z_e = EARTH_RADIUS_KM * np.outer(np.ones(np.size(u)), np.cos(v))
        self.ax.plot_surface(x_e, y_e, z_e, color='#1e2a4a', alpha=0.6, edgecolor='#2f4a7a', linewidth=0.2)

        # Initialize 3D Plot Objects
        self.p_active = self.ax.scatter([], [], [], c='#00ff41', marker='o', s=10, label='Active Sat')
        self.p_dead = self.ax.scatter([], [], [], c='#ff073a', marker='x', s=15, label='Offline Node')
        
        # For tracing packets in 3D
        self.p_packet = self.ax.scatter([], [], [], c='#ffdd00', marker='o', s=40, label='Packet Trace')
        self.l_packet, = self.ax.plot([], [], [], '-', color='#ffdd00', linewidth=2.5, alpha=0.9)
        
        # Provide a 0-length dummy segment to bypass Matplotlib's empty array auto-scaling bug
        dummy_segment = [[(0.0, 0.0, 0.0), (0.0, 0.0, 0.0)]]
        self.line_collection = Line3DCollection(dummy_segment, colors='#3a3a5a', linewidths=0.4, alpha=0.4)

        # Disable auto-scaling for this collection to ensure complete stability
        self.ax.add_collection3d(self.line_collection)
        
        # Status Text (Placed on 2D figure coordinate frame)
        self.stats_text = self.fig.text(0.80, 0.90, '', color='white', fontsize=11, 
                                        verticalalignment='top', family='monospace')
        self.strike_text = self.fig.text(0.5, 0.90, '', color='#ff073a',
                                        fontsize=20, fontweight='bold', ha='center')

        self.trace_src = 12
        self.trace_dst = 77
        self.step_count = 0
        self.status_msg = "NOMINAL"

        # --- UI CONTROL INTERFACE ---
        ax_strike = plt.axes([0.2, 0.05, 0.25, 0.05])
        ax_reset = plt.axes([0.55, 0.05, 0.25, 0.05])
        
        self.btn_strike = Button(ax_strike, 'Trigger Kinetic Strike', color='#5e0000', hovercolor='#ff073a')
        self.btn_reset = Button(ax_reset, 'Heal / Reset Network', color='#004d1a', hovercolor='#00ff41')
        
        self.btn_strike.label.set_color('white')
        self.btn_reset.label.set_color('white')
        
        self.btn_strike.on_clicked(self.trigger_kinetic_strike)
        self.btn_reset.on_clicked(self.reset_network)

    def trigger_kinetic_strike(self, event=None):
        self.strike_text.set_text("KINETIC STRIKE: 20% LOSS")
        self.status_msg = "RECOVERING"
        
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

    def reset_network(self, event=None):
        self.strike_text.set_text("")
        self.status_msg = "NOMINAL"
        for sat in self.sats:
            sat.is_active = True
            sat.routing_table = {sat.node_id: (0, sat.node_id)}
            sat.table_changed = True

    def update_topology(self):
        active_sats = [s for s in self.sats if s.is_active]
        for s in active_sats: s.neighbors = []

        for i in range(len(active_sats)):
            for j in range(i + 1, len(active_sats)):
                sat_a = active_sats[i]
                sat_b = active_sats[j]
                
                # 3D Euclidean distance vector
                dist = np.linalg.norm(sat_a.pos_3d - sat_b.pos_3d)
                if dist <= MAX_LINK_RANGE_KM:
                    sat_a.neighbors.append(sat_b.node_id)
                    sat_b.neighbors.append(sat_a.node_id)

    def distribute_routing(self):
        active_sats = [s for s in self.sats if s.is_active]
        
        all_broadcasts = {}
        for sat in active_sats:
            table = sat.broadcast_table()
            if table: all_broadcasts[sat.node_id] = table
                
        for sat in active_sats:
            for neighbor_id in sat.neighbors:
                if neighbor_id in all_broadcasts:
                    sat.process_neighbor_update(neighbor_id, all_broadcasts[neighbor_id])

    def trace_packet_path(self, src_id, dst_id):
        if not self.sats[src_id].is_active: return []
        path_ids = [src_id]
        current_id = src_id
        visited = {src_id} 

        while current_id != dst_id:
            next_hop = self.sats[current_id].get_next_hop(dst_id)
            if next_hop is None or next_hop in visited: break 
            path_ids.append(next_hop)
            visited.add(next_hop)
            current_id = next_hop
            
        return path_ids

    def update_plot(self, frame_id):
        self.step_count += 1
        
        if self.status_msg == "RECOVERING" and self.step_count % 20 == 0:
             self.strike_text.set_text("") 

        # 1. Physics Engine
        for sat in self.sats: sat.update_physics(SIM_DT_SECONDS)
            
        # 2. Rebuild Graph Topology
        self.update_topology()
        
        # 3. Distributed Convergence Loop
        for _ in range(4): self.distribute_routing()

        # 4. Extract 3D Coordinates safely
        active_coords = np.array([s.pos_3d for s in self.sats if s.is_active])
        dead_coords = np.array([s.pos_3d for s in self.sats if not s.is_active])

        links_3d = []
        for s in [sat for sat in self.sats if sat.is_active]:
            for n_id in s.neighbors:
                if n_id > s.node_id and self.sats[n_id].is_active:
                    links_3d.append([s.pos_3d, self.sats[n_id].pos_3d])

        # Update 3D Scatter plots using internal Matplotlib 3D handlers
        if len(active_coords) > 0:
            self.p_active._offsets3d = (active_coords[:, 0], active_coords[:, 1], active_coords[:, 2])
        else:
            self.p_active._offsets3d = ([], [], [])

        if len(dead_coords) > 0:
            self.p_dead._offsets3d = (dead_coords[:, 0], dead_coords[:, 1], dead_coords[:, 2])
        else:
            self.p_dead._offsets3d = ([], [], [])

        self.line_collection.set_segments(links_3d)
        
        # Rotate the camera continuously for a true 3D orbit effect
        self.ax.view_init(elev=20, azim=self.step_count * 0.5)
        
        # Update UI Stats
        num_act = sum(1 for s in self.sats if s.is_active)
        num_dead = sum(1 for s in self.sats if not s.is_active)
        sim_time_s = self.step_count * SIM_DT_SECONDS
        
        self.stats_text.set_text(f"--- TELEMETRY ---\n"
                                 f"Step: {self.step_count}\n"
                                 f"T+ : {sim_time_s:.1f}s\n"
                                 f"Status: {self.status_msg}\n\n"
                                 f"Nodes Active: {num_act}\n"
                                 f"Nodes Offline: {num_dead}\n"
                                 f"Laser Links: {len(links_3d)}\n\n"
                                 f"--- ROUTING ---\n"
                                 f"Tracing:\nSat {self.trace_src} -> Sat {self.trace_dst}")
        
        # Update Path Trace in 3D
        path_trace = self.trace_packet_path(self.trace_src, self.trace_dst)
        if len(path_trace) > 1:
            pc = np.array([self.sats[pid].pos_3d for pid in path_trace])
            # Set 3D line data
            self.l_packet.set_data(pc[:, 0], pc[:, 1])
            self.l_packet.set_3d_properties(pc[:, 2])
            # Set 3D scatter start point
            self.p_packet._offsets3d = ([pc[0, 0]], [pc[0, 1]], [pc[0, 2]])
        else:
            self.l_packet.set_data([], [])
            self.l_packet.set_3d_properties([])
            self.p_packet._offsets3d = ([], [], [])

        return [self.p_active, self.p_dead, self.line_collection, 
                self.stats_text, self.strike_text, self.p_packet, self.l_packet]

    def run(self):
        ani = animation.FuncAnimation(self.fig, self.update_plot, interval=50, blit=False, cache_frame_data=False)
        self.fig.legend(handles=[self.p_active, self.p_dead, self.p_packet], 
                        loc='upper left', facecolor='#05050f', labelcolor='white', framealpha=0.8)
        plt.show()

if __name__ == "__main__":
    print("Starting 3D SpaceX Multi-Agent Router Simulation...")
    viz = ConstellationViz3D()
    viz.run()