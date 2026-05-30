import sys
import math
import random
import numpy as np

from PyQt6.QtCore import QTimer, Qt
from PyQt6.QtGui import QPixmap, QImage, QColor
from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QHBoxLayout, QVBoxLayout,
    QPushButton, QLabel, QListWidget, QSplitter, QGroupBox, QSlider,
    QTabWidget, QTableWidget, QTableWidgetItem, QHeaderView, QAbstractItemView,
    QComboBox, QProgressBar, QFileDialog, QDoubleSpinBox, QFormLayout,
)

import pyqtgraph as pg
import pyqtgraph.opengl as gl

from orbit import (
    OrbitalElements, CartesianState,
    coe_to_rv, rv_to_coe,
    propagate_kepler_universal,
    eci_to_ecef, ecef_to_geodetic_spherical,
    is_visible_from_station_ecef,
)
from network_model import NetworkGraph, build_topology, Link, LinkProperties, SPEED_OF_LIGHT_KM_S
from routing import DijkstraRouter, DistanceVectorRouter, Router
from traffic import TrafficGenerator, TrafficSimulator
from failures import FailureModel, LinkImpairments
from ground import GroundStation
from link_budget import inverse_square_budget
from logging_utils import CSVRunLogger
from metrics import MetricsCollector

# ── Physical constants ──────────────────────────────────────────────────────
EARTH_RADIUS_KM     = 6378.137
MU_EARTH            = 398600.4418

# ── Constellation (Starlink-like Walker-Delta) ──────────────────────────────
LEO_ALTITUDE_KM     = 550.0
SAT_ALTITUDE_KM     = EARTH_RADIUS_KM + LEO_ALTITUDE_KM
MAX_LINK_RANGE_KM   = 2500.0
NUM_PLANES          = 10
NUM_SATS_PER_PLANE  = 10
NUM_TOTAL_SATS      = NUM_PLANES * NUM_SATS_PER_PLANE
INCLINATION_RAD     = math.radians(53.0)

# ── Network / Traffic defaults ───────────────────────────────────────────────
ROUTING_MODE        = "dijkstra"        # "dijkstra" | "distance_vector"
ROUTING_COST        = "latency"         # "latency"  | "hops"
TRAFFIC_PATTERN     = "uniform"         # "uniform"  | "hotspot" | "burst"
TRAFFIC_RATE_PPS    = 3.0

# ── Ground stations ──────────────────────────────────────────────────────────
GROUND_MIN_ELEV_DEG     = 10.0
GROUND_SAT_MAX_RANGE_KM = 3000.0
GROUND_BW_Mbps          = 300.0
GROUND_BASE_LOSS        = 0.002

GROUND_STATIONS = [
    ("Sri Lanka",  7.8731,  80.7718),
    ("Singapore",  1.3521, 103.8198),
    ("London",    51.5074,  -0.1278),
    ("New York",  40.7128, -74.0060),
    ("Tokyo",     35.6762, 139.6503),
]

# ── Render ───────────────────────────────────────────────────────────────────
EARTH_TEXTURE_PATH      = "img/earth_texture.jpg"
TOPOLOGY_HISTORY_STEPS  = 300

# ── Throttle intervals (in simulation ticks, timer fires ~60 Hz) ─────────────
# 3D GL:        every tick  (60 Hz)
# Telemetry HUD: every tick  (60 Hz, text only, cheap)
# Metrics plots: every 5 ticks (~12 Hz)
# 2D map:        every 10 ticks (~6 Hz) and only when tab visible
# Ground table:  every 5 ticks (~12 Hz)
PLOT_UPDATE_EVERY = 5
MAP_UPDATE_EVERY  = 10

# ── Dark theme ───────────────────────────────────────────────────────────────
DARK_THEME = """
QMainWindow, QWidget {
    background-color: #0d1117; color: #c9d1d9;
    font-family: 'Segoe UI', Arial, sans-serif;
}
QGroupBox {
    border: 1px solid #30363d; border-radius: 6px;
    margin-top: 14px; font-weight: bold; font-size: 12px;
}
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; color: #58a6ff; }
QPushButton {
    background-color: #21262d; border: 1px solid #30363d;
    border-radius: 5px; padding: 7px; font-weight: bold; color: #c9d1d9;
}
QPushButton:hover { background-color: #30363d; border: 1px solid #8b949e; }
QPushButton:checked { background-color: #1f6feb; border-color: #388bfd; color: white; }
QListWidget {
    background-color: #010409; border: 1px solid #30363d;
    border-radius: 5px; padding: 4px; outline: none;
}
QListWidget::item { padding: 3px; border-radius: 3px; }
QListWidget::item:selected { background-color: #1f6feb; color: white; font-weight: bold; }
QListWidget::item:hover { background-color: #21262d; }
QSlider::groove:horizontal {
    border: 1px solid #30363d; height: 5px;
    background: #010409; border-radius: 3px;
}
QSlider::handle:horizontal {
    background: #58a6ff; width: 13px; margin: -4px 0; border-radius: 6px;
}
QSlider::handle:horizontal:hover { background: #79c0ff; }
QTabWidget::pane { border: 1px solid #30363d; border-radius: 4px; }
QTabBar::tab {
    background: #161b22; border: 1px solid #30363d;
    padding: 5px 12px; color: #8b949e; font-size: 11px;
}
QTabBar::tab:selected {
    background: #21262d; color: #c9d1d9;
    border-bottom: 2px solid #58a6ff;
}
QTabBar::tab:hover { background: #21262d; color: #c9d1d9; }
QTableWidget {
    background-color: #010409; border: 1px solid #30363d;
    gridline-color: #21262d; font-size: 11px;
}
QTableWidget::item { padding: 3px; }
QTableWidget::item:selected { background-color: #1f6feb; color: white; }
QTableWidget::item:alternate { background-color: #0d1117; }
QHeaderView::section {
    background-color: #161b22; border: 1px solid #30363d;
    padding: 4px 6px; font-weight: bold; font-size: 10px; color: #8b949e;
}
QLabel { font-size: 12px; }
QComboBox {
    background-color: #21262d; border: 1px solid #30363d;
    border-radius: 4px; padding: 4px 8px; color: #c9d1d9;
}
QComboBox::drop-down { border: none; }
QComboBox QAbstractItemView {
    background-color: #161b22; border: 1px solid #30363d; color: #c9d1d9;
}
QProgressBar {
    border: 1px solid #30363d; border-radius: 4px;
    background-color: #010409; text-align: center;
    color: #c9d1d9; font-size: 11px;
}
QProgressBar::chunk { background-color: #1f6feb; border-radius: 3px; }
QDoubleSpinBox {
    background-color: #21262d; border: 1px solid #30363d;
    border-radius: 4px; padding: 3px 6px; color: #c9d1d9;
}
"""


# ─────────────────────────────────────────────────────────────────────────────
class FileTransferSession:
    """Tracks a single ground-station-to-ground-station payload transfer."""

    STATUS_IDLE         = "IDLE"
    STATUS_ROUTING      = "ROUTING..."
    STATUS_TRANSFERRING = "TRANSFERRING"
    STATUS_COMPLETE     = "COMPLETE"
    STATUS_NO_ROUTE     = "NO ROUTE"
    STATUS_INTERRUPTED  = "INTERRUPTED"

    def __init__(self, src_gs_id: int, dst_gs_id: int, total_bytes: float):
        self.src_gs_id    = src_gs_id
        self.dst_gs_id    = dst_gs_id
        self.total_bytes  = float(total_bytes)
        self.bytes_sent   = 0.0
        self.start_sim_t  = 0.0          # sim time when transfer started
        self.elapsed_sim_s = 0.0
        self.status       = self.STATUS_ROUTING
        self.path         = []           # current hop list (node IDs)
        self.bottleneck_bw_mbps  = 0.0
        self.bottleneck_link     = (None, None)   # (u, v)
        self.instantaneous_MBps  = 0.0
        self.total_latency_ms    = 0.0
        self.payload_label       = ""    # human-readable filename/size


# ─────────────────────────────────────────────────────────────────────────────
class SatelliteAgent:
    def __init__(self, node_id, orbit_plane_idx, phase_offset_rad):
        self.node_id   = node_id
        self.name      = f"SAT-{orbit_plane_idx:02d}-{node_id % NUM_SATS_PER_PLANE:02d}"
        self.plane_idx = orbit_plane_idx

        self.RAAN_rad     = (2 * math.pi / NUM_PLANES) * orbit_plane_idx
        self.inclination  = INCLINATION_RAD
        self.argp_rad     = 0.0
        self.ecc          = 0.001
        self.a_km         = float(SAT_ALTITUDE_KM)
        self.nu_rad       = float(phase_offset_rad)

        self.coe = OrbitalElements(
            a_km=self.a_km, e=self.ecc, i_rad=self.inclination,
            raan_rad=self.RAAN_rad, argp_rad=self.argp_rad, nu_rad=self.nu_rad,
        )
        self.state = coe_to_rv(MU_EARTH, self.coe)
        self.pos_3d        = np.zeros(3)
        self.vel_eci_km_s  = np.zeros(3)
        self.is_active     = True
        self.neighbors     = []
        self._sync_render_state()

    def _sync_render_state(self):
        self.pos_3d       = self.state.r_km / 1000.0
        self.vel_eci_km_s = self.state.v_km_s.copy()
        try:
            self.coe    = rv_to_coe(MU_EARTH, self.state.r_km, self.state.v_km_s)
            self.nu_rad = self.coe.nu_rad
        except Exception:
            pass

    def update_physics(self, dt_seconds):
        if not self.is_active:
            return
        r, v = propagate_kepler_universal(MU_EARTH, self.state.r_km, self.state.v_km_s, float(dt_seconds))
        self.state = CartesianState(r_km=r, v_km_s=v)
        self._sync_render_state()


# ─────────────────────────────────────────────────────────────────────────────
class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("ASTRA — Satellite Constellation Simulator")
        self.resize(1700, 980)

        # ── Satellites ───────────────────────────────────────────────────────
        self.sats = []
        for plane in range(NUM_PLANES):
            for sat_idx in range(NUM_SATS_PER_PLANE):
                node_id      = plane * NUM_SATS_PER_PLANE + sat_idx
                phase_offset = (2 * math.pi / NUM_SATS_PER_PLANE) * sat_idx + plane * 0.1
                self.sats.append(SatelliteAgent(node_id, plane, phase_offset))

        # ── Ground stations ──────────────────────────────────────────────────
        self.ground_nodes    = []
        self.ground_node_ids = []
        for i, (name, lat_deg, lon_deg) in enumerate(GROUND_STATIONS):
            gid = NUM_TOTAL_SATS + i
            gs  = GroundStation(
                gid=gid, name=name,
                lat_rad=math.radians(float(lat_deg)),
                lon_rad=math.radians(float(lon_deg)),
                alt_km=0.0, min_elev_deg=GROUND_MIN_ELEV_DEG,
            )
            self.ground_nodes.append(gs)
            self.ground_node_ids.append(gid)

        self.best_sat_for_station = {gs.gid: None for gs in self.ground_nodes}
        self.gs_link_stats        = {}   # gid -> dict | None

        # ── Simulation state ─────────────────────────────────────────────────
        self.trace_src         = 12
        self.trace_dst         = 77
        self.step_count        = 0
        self._sim_tick         = 0       # throttle counter
        self.selected_sat_id   = None
        self.status_msg        = "NOMINAL"
        self.last_sim_time_s   = 0.0
        self.sim_dt_seconds    = 5.0
        self.strike_severity   = 0.20
        self.occlusion_enabled = True

        # ── Network / Routing / Traffic ──────────────────────────────────────
        self.graph             = NetworkGraph()
        self.router: Router    = self._make_router()
        self.traffic           = TrafficSimulator(
            TrafficGenerator(pattern=TRAFFIC_PATTERN, rate_pps=TRAFFIC_RATE_PPS, hotspot_id=self.trace_dst)
        )
        self.last_traffic_stats = None
        self.last_step_metrics  = None
        self.metrics            = MetricsCollector(sample_pairs=40)

        self.failure_model = FailureModel(
            LinkImpairments(
                blackout_prob_per_s=0.0000,
                latency_spike_prob_per_s=0.0000,
                latency_spike_add_s=0.030,
                loss_multiplier=1.0,
            )
        )

        # ── Topology tracking (for 2D map diff) ─────────────────────────────
        self.prev_edge_set = set()
        self.failed_edges  = []         # [(u, v, ttl_steps)]
        self.metric_hist   = {"t": [], "delivery": [], "delay": [], "util": []}

        # ── CSV logger (off by default) ──────────────────────────────────────
        self.log_to_csv = False
        self.run_logger = CSVRunLogger("runs/latest_run.csv") if self.log_to_csv else None

        # ── Cached state shared between GL render and throttled 2D map ───────
        self._node_ll   = {}     # sid -> (lon_deg, lat_deg)
        self._edge_set  = set()
        self._path_trace = []
        self._failed_edges_ll = []   # [(u,v,ttl)] for 2D rendering

        self._metrics_expanded = True

        # ── File transfer session ────────────────────────────────────────────
        self.transfer: FileTransferSession | None = None
        self._transfer_path_pts = np.empty((0, 3))   # cached 3D coords for GL

        self.setup_ui()
        self.init_gl_objects()

        self.timer = QTimer()
        self.timer.timeout.connect(self.update_simulation)
        self.timer.start(16)

    # ── Router factory ────────────────────────────────────────────────────────
    def _make_router(self) -> Router:
        if ROUTING_MODE == "dijkstra":
            return DijkstraRouter(weight=ROUTING_COST)
        return DistanceVectorRouter(cost=ROUTING_COST, rounds_per_step=3)

    # ─────────────────────────────────────────────────────────────────────────
    # UI SETUP
    # ─────────────────────────────────────────────────────────────────────────
    def setup_ui(self):
        root = QWidget()
        self.setCentralWidget(root)
        root_layout = QVBoxLayout(root)
        root_layout.setContentsMargins(8, 8, 8, 4)
        root_layout.setSpacing(4)

        # ── Top: 3D view + tab panel ─────────────────────────────────────────
        top_splitter = QSplitter(Qt.Orientation.Horizontal)
        root_layout.addWidget(top_splitter, stretch=1)

        self.gl_view = gl.GLViewWidget()
        self.gl_view.setCameraPosition(distance=25, elevation=30, azimuth=40)
        self.gl_view.setBackgroundColor('#05070a')
        top_splitter.addWidget(self.gl_view)

        self.tab_panel = QTabWidget()
        self.tab_panel.setMinimumWidth(340)
        self.tab_panel.setMaximumWidth(430)
        top_splitter.addWidget(self.tab_panel)
        top_splitter.setSizes([1260, 400])
        top_splitter.setStretchFactor(0, 1)
        top_splitter.setStretchFactor(1, 0)

        self._build_mission_tab()
        self._build_inspector_tab()
        self._build_ground_tab()
        self._build_map_tab()
        self._build_transfer_tab()

        # ── Bottom: collapsible metrics bar ──────────────────────────────────
        self._build_metrics_bar(root_layout)

    # ── MISSION tab ───────────────────────────────────────────────────────────
    def _build_mission_tab(self):
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(6, 6, 6, 6)
        lay.setSpacing(6)

        # Telemetry HUD
        self.lbl_telemetry = QLabel("INITIALIZING...")
        self.lbl_telemetry.setStyleSheet(
            "font-family:'Consolas',monospace; font-size:12px; color:#3fb950;"
            "background-color:#010409; padding:12px;"
            "border:1px solid #30363d; border-radius:6px;"
        )
        self.lbl_telemetry.setWordWrap(True)
        lay.addWidget(self.lbl_telemetry)

        # Controls
        ctrl = QGroupBox("MISSION CONTROL")
        cl   = QVBoxLayout()
        cl.setSpacing(4)

        self.lbl_speed   = QLabel(f"Sim Speed (ΔT): {self.sim_dt_seconds:.0f} s/step")
        self.slider_speed = QSlider(Qt.Orientation.Horizontal)
        self.slider_speed.setRange(1, 50)
        self.slider_speed.setValue(int(self.sim_dt_seconds))
        self.slider_speed.valueChanged.connect(self.update_speed)
        cl.addWidget(self.lbl_speed)
        cl.addWidget(self.slider_speed)

        self.lbl_severity   = QLabel(f"Strike Severity: {int(self.strike_severity*100)}% nodes")
        self.slider_severity = QSlider(Qt.Orientation.Horizontal)
        self.slider_severity.setRange(5, 80)
        self.slider_severity.setValue(int(self.strike_severity * 100))
        self.slider_severity.valueChanged.connect(self.update_severity)
        cl.addWidget(self.lbl_severity)
        cl.addWidget(self.slider_severity)

        row1 = QHBoxLayout()
        self.btn_strike = QPushButton("EXECUTE STRIKE")
        self.btn_strike.setStyleSheet("background-color:#da3633;color:white;border-color:#b3261e;")
        self.btn_strike.clicked.connect(self.trigger_kinetic_strike)
        self.btn_reset = QPushButton("SYSTEM REBOOT")
        self.btn_reset.setStyleSheet("background-color:#238636;color:white;border-color:#2ea043;")
        self.btn_reset.clicked.connect(self.reset_network)
        row1.addWidget(self.btn_strike)
        row1.addWidget(self.btn_reset)

        row2 = QHBoxLayout()
        self.btn_pause    = QPushButton("PAUSE")
        self.btn_pause.setCheckable(True)
        self.btn_pause.toggled.connect(self.toggle_pause)
        self.btn_step     = QPushButton("STEP")
        self.btn_step.clicked.connect(self.step_once)
        self.btn_occlusion = QPushButton("OCCLUDE: ON")
        self.btn_occlusion.setCheckable(True)
        self.btn_occlusion.setChecked(True)
        self.btn_occlusion.toggled.connect(self.toggle_occlusion)
        row2.addWidget(self.btn_pause)
        row2.addWidget(self.btn_step)
        row2.addWidget(self.btn_occlusion)

        cl.addLayout(row1)
        cl.addLayout(row2)
        ctrl.setLayout(cl)
        lay.addWidget(ctrl)
        lay.addStretch()
        self.tab_panel.addTab(w, "MISSION")

    # ── INSPECT tab ───────────────────────────────────────────────────────────
    def _build_inspector_tab(self):
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(6, 6, 6, 6)
        lay.setSpacing(6)

        self.sat_list_widget = QListWidget()
        self.sat_list_widget.setMaximumHeight(200)
        for sat in self.sats:
            self.sat_list_widget.addItem(f"{sat.name}  (ID {sat.node_id})")
        self.sat_list_widget.currentRowChanged.connect(self.sat_selected)
        lay.addWidget(self.sat_list_widget)

        self.lbl_spec = QLabel("Select a satellite above.")
        self.lbl_spec.setStyleSheet(
            "font-family:'Consolas',monospace; font-size:11px;"
            "background-color:#010409; padding:10px;"
            "border:1px solid #30363d; border-radius:5px;"
        )
        self.lbl_spec.setWordWrap(True)
        lay.addWidget(self.lbl_spec)
        lay.addStretch()
        self.tab_panel.addTab(w, "INSPECT")

    # ── GROUND tab ────────────────────────────────────────────────────────────
    def _build_ground_tab(self):
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(6, 6, 6, 6)
        lay.setSpacing(6)

        hdr = QLabel("Ground Station Link Monitor")
        hdr.setStyleSheet("font-weight:bold; color:#58a6ff; font-size:12px;")
        lay.addWidget(hdr)

        cols = ["Station", "Best Sat", "Elev °", "BW Mbps", "Lat ms"]
        self.gs_table = QTableWidget(len(self.ground_nodes), len(cols))
        self.gs_table.setHorizontalHeaderLabels(cols)
        self.gs_table.verticalHeader().setVisible(False)
        self.gs_table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        self.gs_table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.gs_table.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.gs_table.setAlternatingRowColors(True)
        self.gs_table.setFixedHeight(165)
        self.gs_table.itemSelectionChanged.connect(self._gs_row_selected)

        for i, gs in enumerate(self.ground_nodes):
            for j in range(len(cols)):
                item = QTableWidgetItem(gs.name if j == 0 else "—")
                item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
                self.gs_table.setItem(i, j, item)
        lay.addWidget(self.gs_table)

        self.lbl_gs_detail = QLabel("Select a row for link detail.")
        self.lbl_gs_detail.setStyleSheet(
            "font-family:'Consolas',monospace; font-size:11px;"
            "background-color:#010409; padding:10px;"
            "border:1px solid #30363d; border-radius:5px;"
        )
        self.lbl_gs_detail.setWordWrap(True)
        lay.addWidget(self.lbl_gs_detail)
        lay.addStretch()
        self.tab_panel.addTab(w, "GROUND")

    def _gs_row_selected(self):
        rows = self.gs_table.selectedItems()
        if not rows:
            return
        row = self.gs_table.currentRow()
        if row < 0 or row >= len(self.ground_nodes):
            return
        gs    = self.ground_nodes[row]
        stats = self.gs_link_stats.get(gs.gid)
        if stats is None:
            self.lbl_gs_detail.setText(
                f"<b>{gs.name}</b><br>"
                f"<span style='color:#ff7b72;'>NO LINK — no satellite above {gs.min_elev_deg}° elevation</span>"
            )
        else:
            self.lbl_gs_detail.setText(
                f"<b>{gs.name}</b><br>"
                f"Lat {math.degrees(gs.lat_rad):.2f}°  Lon {math.degrees(gs.lon_rad):.2f}°<br><br>"
                f"<b>Locked sat :</b> {stats['sat']}<br>"
                f"<b>Elevation  :</b> {stats['elev_deg']:.1f}°<br>"
                f"<b>Slant range:</b> {stats['dist_km']:.0f} km<br>"
                f"<b>Bandwidth  :</b> {stats['bw_mbps']:.1f} Mbps<br>"
                f"<b>Latency    :</b> {stats['lat_ms']:.1f} ms<br>"
            )

    # ── MAP tab ───────────────────────────────────────────────────────────────
    def _build_map_tab(self):
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(4, 4, 4, 4)

        self.topo_plot = pg.PlotWidget()
        self.topo_plot.setBackground('#010409')
        self.topo_plot.showGrid(x=True, y=True, alpha=0.12)
        self.topo_plot.setLabel('bottom', 'Longitude °', color='#8b949e', size='9pt')
        self.topo_plot.setLabel('left',   'Latitude °',  color='#8b949e', size='9pt')
        self.topo_plot.setXRange(-180, 180, padding=0.01)
        self.topo_plot.setYRange(-90,   90, padding=0.01)
        self.topo_plot.getAxis('bottom').setStyle(tickFont=pg.QtGui.QFont('Consolas', 8))
        self.topo_plot.getAxis('left').setStyle(tickFont=pg.QtGui.QFont('Consolas', 8))

        self.topo_edges_item  = pg.PlotDataItem(pen=pg.mkPen((80, 120, 200, 65), width=1))
        self.topo_failed_item = pg.PlotDataItem(pen=pg.mkPen((220, 60,  60, 180), width=2))
        self.topo_route_item  = pg.PlotDataItem(pen=pg.mkPen((255, 200, 40, 230), width=3))
        self.topo_nodes_item  = pg.ScatterPlotItem(size=5, brush=pg.mkBrush(60, 180, 80, 200), pen=None)
        self.topo_ground_item = pg.ScatterPlotItem(
            size=9,
            brush=pg.mkBrush(120, 160, 255, 220),
            pen=pg.mkPen((200, 220, 255, 180), width=1),
        )
        for item in (self.topo_edges_item, self.topo_failed_item, self.topo_route_item,
                     self.topo_nodes_item, self.topo_ground_item):
            self.topo_plot.addItem(item)

        lay.addWidget(self.topo_plot)
        self.tab_panel.addTab(w, "2D MAP")

    # ── TRANSFER tab ─────────────────────────────────────────────────────────
    def _build_transfer_tab(self):
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(8, 8, 8, 8)
        lay.setSpacing(8)

        # ── Route selection ──────────────────────────────────────────────────
        route_group = QGroupBox("TRANSFER ROUTE")
        rg = QFormLayout()
        rg.setSpacing(6)

        gs_names = [gs.name for gs in self.ground_nodes]

        self.cmb_src = QComboBox()
        self.cmb_src.addItems(gs_names)
        # Default: New York → London
        ny_idx = next((i for i, gs in enumerate(self.ground_nodes) if "York" in gs.name), 0)
        ld_idx = next((i for i, gs in enumerate(self.ground_nodes) if "London" in gs.name), 1)
        self.cmb_src.setCurrentIndex(ny_idx)
        rg.addRow("Source:", self.cmb_src)

        self.cmb_dst = QComboBox()
        self.cmb_dst.addItems(gs_names)
        self.cmb_dst.setCurrentIndex(ld_idx)
        rg.addRow("Destination:", self.cmb_dst)
        route_group.setLayout(rg)
        lay.addWidget(route_group)

        # ── Payload selection ─────────────────────────────────────────────────
        payload_group = QGroupBox("PAYLOAD")
        pg_lay = QVBoxLayout()
        pg_lay.setSpacing(4)

        file_row = QHBoxLayout()
        self.btn_pick_file = QPushButton("📁  Select File")
        self.btn_pick_file.clicked.connect(self._pick_transfer_file)
        self.lbl_file_name = QLabel("No file selected")
        self.lbl_file_name.setStyleSheet("color:#8b949e; font-size:11px;")
        file_row.addWidget(self.btn_pick_file)
        file_row.addWidget(self.lbl_file_name, stretch=1)
        pg_lay.addLayout(file_row)

        size_row = QHBoxLayout()
        lbl_sz = QLabel("Size (MB):")
        lbl_sz.setStyleSheet("font-size:11px;")
        self.spin_size = QDoubleSpinBox()
        self.spin_size.setRange(0.001, 10000.0)
        self.spin_size.setDecimals(3)
        self.spin_size.setSingleStep(1.0)
        self.spin_size.setValue(500.0)
        self.spin_size.setSuffix(" MB")
        size_row.addWidget(lbl_sz)
        size_row.addWidget(self.spin_size, stretch=1)
        pg_lay.addLayout(size_row)
        payload_group.setLayout(pg_lay)
        lay.addWidget(payload_group)

        # ── Start / Cancel ────────────────────────────────────────────────────
        self.btn_start_transfer = QPushButton("▶  START TRANSFER")
        self.btn_start_transfer.setStyleSheet(
            "background-color:#1f6feb; color:white; border-color:#388bfd;"
            "font-size:13px; padding:9px; font-weight:bold;"
        )
        self.btn_start_transfer.clicked.connect(self._start_transfer)
        lay.addWidget(self.btn_start_transfer)

        # ── Status ────────────────────────────────────────────────────────────
        self.lbl_transfer_status = QLabel("IDLE")
        self.lbl_transfer_status.setStyleSheet(
            "font-family:'Consolas',monospace; font-size:12px; font-weight:bold;"
            "color:#8b949e; padding:2px;"
        )
        lay.addWidget(self.lbl_transfer_status)

        self.transfer_progress = QProgressBar()
        self.transfer_progress.setRange(0, 1000)
        self.transfer_progress.setValue(0)
        self.transfer_progress.setFormat("%p%")
        self.transfer_progress.setFixedHeight(22)
        lay.addWidget(self.transfer_progress)

        # ── Live stats ────────────────────────────────────────────────────────
        self.lbl_transfer_stats = QLabel("")
        self.lbl_transfer_stats.setStyleSheet(
            "font-family:'Consolas',monospace; font-size:11px;"
            "background-color:#010409; padding:10px;"
            "border:1px solid #30363d; border-radius:5px;"
        )
        self.lbl_transfer_stats.setWordWrap(True)
        lay.addWidget(self.lbl_transfer_stats)
        lay.addStretch()

        self.tab_panel.addTab(w, "TRANSFER")

    def _pick_transfer_file(self):
        path, _ = QFileDialog.getOpenFileName(self, "Select File to Transfer", "", "All Files (*)")
        if path:
            import os
            size_bytes = os.path.getsize(path)
            size_mb    = size_bytes / 1_048_576
            self.spin_size.setValue(size_mb)
            self.lbl_file_name.setText(os.path.basename(path))
            self._transfer_file_label = os.path.basename(path)

    def _start_transfer(self):
        if self.transfer is not None and self.transfer.status == FileTransferSession.STATUS_TRANSFERRING:
            # Cancel running transfer
            self.transfer.status = FileTransferSession.STATUS_IDLE
            self.transfer = None
            self.btn_start_transfer.setText("▶  START TRANSFER")
            self.btn_start_transfer.setStyleSheet(
                "background-color:#1f6feb; color:white; border-color:#388bfd;"
                "font-size:13px; padding:9px; font-weight:bold;"
            )
            self._transfer_path_pts = np.empty((0, 3))
            return

        src_idx = self.cmb_src.currentIndex()
        dst_idx = self.cmb_dst.currentIndex()
        if src_idx == dst_idx:
            self.lbl_transfer_status.setText("ERROR: source = destination")
            return

        src_gs  = self.ground_nodes[src_idx]
        dst_gs  = self.ground_nodes[dst_idx]
        total_b = self.spin_size.value() * 1_048_576

        self.transfer = FileTransferSession(src_gs.gid, dst_gs.gid, total_b)
        self.transfer.start_sim_t   = self.last_sim_time_s
        file_label = getattr(self, '_transfer_file_label', f"{self.spin_size.value():.3f} MB payload")
        self.transfer.payload_label = file_label

        self.btn_start_transfer.setText("■  CANCEL")
        self.btn_start_transfer.setStyleSheet(
            "background-color:#da3633; color:white; border-color:#b3261e;"
            "font-size:13px; padding:9px; font-weight:bold;"
        )

    def _node_pos_3d(self, node_id: int) -> np.ndarray:
        """Return world-space 3D position for a satellite or ground station node."""
        if node_id < NUM_TOTAL_SATS:
            return np.array(self.sats[node_id].pos_3d, dtype=float)
        gs = self.ground_nodes[node_id - NUM_TOTAL_SATS]
        return gs.ecef_position_km(EARTH_RADIUS_KM) / 1000.0

    def _update_transfer_session(self, sim_time_s: float, dt_s: float):
        """Called every simulation tick to advance the file transfer."""
        xfer = self.transfer
        if xfer is None or xfer.status in (
            FileTransferSession.STATUS_COMPLETE,
            FileTransferSession.STATUS_IDLE,
        ):
            return

        # Find current path through constellation
        path = self.router.path(xfer.src_gs_id, xfer.dst_gs_id, max_hops=64)

        if len(path) < 2:
            xfer.status = FileTransferSession.STATUS_NO_ROUTE
            xfer.instantaneous_MBps = 0.0
            self._transfer_path_pts = np.empty((0, 3))
            self._refresh_transfer_ui(sim_time_s)
            return

        xfer.path = path

        # Bottleneck = minimum bandwidth link on path (Shannon bottleneck)
        min_bw_mbps   = float('inf')
        min_link      = (None, None)
        total_lat_ms  = 0.0
        for u, v in zip(path[:-1], path[1:]):
            lnk = self.graph.get_link(u, v)
            if lnk is None:
                # Path exists in routing table but link dropped this tick → interrupted
                xfer.status = FileTransferSession.STATUS_INTERRUPTED
                xfer.instantaneous_MBps = 0.0
                self._transfer_path_pts = np.empty((0, 3))
                self._refresh_transfer_ui(sim_time_s)
                return
            total_lat_ms += lnk.props.latency_s * 1000.0
            if lnk.props.bandwidth_mbps < min_bw_mbps:
                min_bw_mbps = lnk.props.bandwidth_mbps
                min_link    = (u, v)

        # Bytes transferred this step (bottleneck limited)
        # Mbps → bytes/s = Mbps * 1e6 / 8
        bytes_this_step = min_bw_mbps * 1e6 / 8.0 * dt_s
        xfer.bytes_sent = min(xfer.total_bytes, xfer.bytes_sent + bytes_this_step)

        xfer.bottleneck_bw_mbps  = min_bw_mbps
        xfer.bottleneck_link     = min_link
        xfer.instantaneous_MBps  = min_bw_mbps / 8.0
        xfer.total_latency_ms    = total_lat_ms
        xfer.elapsed_sim_s       = sim_time_s - xfer.start_sim_t
        xfer.status              = FileTransferSession.STATUS_TRANSFERRING

        if xfer.bytes_sent >= xfer.total_bytes:
            xfer.status = FileTransferSession.STATUS_COMPLETE
            self.btn_start_transfer.setText("▶  START TRANSFER")
            self.btn_start_transfer.setStyleSheet(
                "background-color:#238636; color:white; border-color:#2ea043;"
                "font-size:13px; padding:9px; font-weight:bold;"
            )

        # Build 3D path coords for GL rendering
        pts = []
        for nid in path:
            pts.append(self._node_pos_3d(nid))
        self._transfer_path_pts = np.array(pts, dtype=float) if pts else np.empty((0, 3))

        self._refresh_transfer_ui(sim_time_s)

    def _refresh_transfer_ui(self, sim_time_s: float):
        xfer = self.transfer
        if xfer is None:
            return

        # Status label colour
        colour_map = {
            FileTransferSession.STATUS_TRANSFERRING: "#58a6ff",
            FileTransferSession.STATUS_COMPLETE:     "#3fb950",
            FileTransferSession.STATUS_NO_ROUTE:     "#ff7b72",
            FileTransferSession.STATUS_INTERRUPTED:  "#e3b341",
            FileTransferSession.STATUS_ROUTING:      "#8b949e",
        }
        colour = colour_map.get(xfer.status, "#8b949e")
        self.lbl_transfer_status.setText(xfer.status)
        self.lbl_transfer_status.setStyleSheet(
            f"font-family:'Consolas',monospace; font-size:12px; font-weight:bold;"
            f"color:{colour}; padding:2px;"
        )

        # Progress bar
        pct = int(xfer.bytes_sent / max(1, xfer.total_bytes) * 1000)
        self.transfer_progress.setValue(pct)
        # Colour chunk by status
        if xfer.status == FileTransferSession.STATUS_COMPLETE:
            chunk_col = "#3fb950"
        elif xfer.status in (FileTransferSession.STATUS_NO_ROUTE, FileTransferSession.STATUS_INTERRUPTED):
            chunk_col = "#da3633"
        else:
            chunk_col = "#1f6feb"
        self.transfer_progress.setStyleSheet(
            f"QProgressBar::chunk {{ background-color:{chunk_col}; border-radius:3px; }}"
        )

        sent_mb  = xfer.bytes_sent / 1_048_576
        total_mb = xfer.total_bytes / 1_048_576
        remain_b = max(0, xfer.total_bytes - xfer.bytes_sent)
        eta_s    = remain_b / max(1, xfer.instantaneous_MBps * 1_048_576)

        # Bottleneck node names
        bu, bv = xfer.bottleneck_link
        def _node_name(nid):
            if nid is None: return "—"
            if nid < NUM_TOTAL_SATS: return self.sats[nid].name
            return self.ground_nodes[nid - NUM_TOTAL_SATS].name

        src_name = _node_name(xfer.src_gs_id)
        dst_name = _node_name(xfer.dst_gs_id)
        hops     = max(0, len(xfer.path) - 1)

        stats = (
            f"<b>Route</b>  {src_name} → {dst_name}<br>"
            f"<b>Payload</b>  {xfer.payload_label}<br><br>"
            f"<b>Throughput  </b> {xfer.instantaneous_MBps:.2f} MBps "
            f"({xfer.bottleneck_bw_mbps:.0f} Mbps link)<br>"
            f"<b>Transferred </b> {sent_mb:.3f} / {total_mb:.3f} MB<br>"
            f"<b>Elapsed     </b> {xfer.elapsed_sim_s:.2f} s (sim)<br>"
        )
        if xfer.status == FileTransferSession.STATUS_COMPLETE:
            stats += f"<b>Completed   </b> ✓ in {xfer.elapsed_sim_s:.3f} s<br>"
        else:
            stats += f"<b>ETA         </b> {eta_s:.2f} s<br>"
        stats += (
            f"<b>Bottleneck  </b> {_node_name(bu)} → {_node_name(bv)}<br>"
            f"<b>Path hops   </b> {hops}  ({src_name} → {hops-1} sats → {dst_name})<br>"
            f"<b>Path latency</b> {xfer.total_latency_ms:.1f} ms<br>"
        )
        self.lbl_transfer_stats.setText(stats)

    # ── Metrics bar (bottom) ───────────────────────────────────────────────────
    def _build_metrics_bar(self, parent_layout: QVBoxLayout):
        bar = QWidget()
        bar.setMaximumHeight(155)
        bl  = QVBoxLayout(bar)
        bl.setContentsMargins(0, 0, 0, 0)
        bl.setSpacing(2)

        # Header row: toggle button + one-line summary
        hrow = QHBoxLayout()
        self.btn_metrics_toggle = QPushButton("▼  METRICS")
        self.btn_metrics_toggle.setCheckable(True)
        self.btn_metrics_toggle.setChecked(True)
        self.btn_metrics_toggle.setFixedWidth(105)
        self.btn_metrics_toggle.setFixedHeight(20)
        self.btn_metrics_toggle.setStyleSheet(
            "font-size:10px; padding:2px 6px; background:#161b22; border:1px solid #30363d;"
        )
        self.btn_metrics_toggle.toggled.connect(self._toggle_metrics_bar)

        self.lbl_metrics_summary = QLabel("Delivery: —% | Delay: — s | Util: —%")
        self.lbl_metrics_summary.setStyleSheet(
            "font-family:'Consolas',monospace; font-size:11px; color:#8b949e; padding-left:8px;"
        )
        hrow.addWidget(self.btn_metrics_toggle)
        hrow.addWidget(self.lbl_metrics_summary)
        hrow.addStretch()
        bl.addLayout(hrow)

        # Three mini plots side by side
        self.metrics_plots_widget = QWidget()
        prow = QHBoxLayout(self.metrics_plots_widget)
        prow.setContentsMargins(0, 0, 0, 0)
        prow.setSpacing(4)

        def _mini_plot(ylabel, color):
            p = pg.PlotWidget()
            p.setBackground('#010409')
            p.setMaximumHeight(115)
            p.showGrid(x=False, y=True, alpha=0.18)
            p.setLabel('left', ylabel, color='#8b949e', size='8pt')
            p.getAxis('bottom').setStyle(showValues=False, tickLength=0)
            p.getAxis('left').setStyle(tickFont=pg.QtGui.QFont('Consolas', 7))
            p.setMouseEnabled(x=False, y=False)
            curve = p.plot(pen=pg.mkPen(color, width=2))
            return p, curve

        self.plot_delivery, self.curve_delivery = _mini_plot("Delivery %", (80,  200, 120))
        self.plot_delay,    self.curve_delay    = _mini_plot("Delay s",    (120, 160, 255))
        self.plot_util,     self.curve_util     = _mini_plot("Util %",     (255, 200,  40))

        prow.addWidget(self.plot_delivery)
        prow.addWidget(self.plot_delay)
        prow.addWidget(self.plot_util)
        bl.addWidget(self.metrics_plots_widget)

        parent_layout.addWidget(bar)

    def _toggle_metrics_bar(self, checked: bool):
        self._metrics_expanded = checked
        self.metrics_plots_widget.setVisible(checked)
        self.btn_metrics_toggle.setText("▼  METRICS" if checked else "▶  METRICS")

    # ─────────────────────────────────────────────────────────────────────────
    # GL INIT
    # ─────────────────────────────────────────────────────────────────────────
    def init_gl_objects(self):
        earth_r = EARTH_RADIUS_KM / 1000.0
        md      = gl.MeshData.sphere(rows=900, cols=900, radius=earth_r)
        verts   = md.vertexes()
        colors  = np.zeros((verts.shape[0], 4), dtype=float)

        tex_img = QImage(EARTH_TEXTURE_PATH)
        if not tex_img.isNull():
            tw, th = tex_img.width(), tex_img.height()
            for i, (x, y, z) in enumerate(verts):
                r   = math.sqrt(x*x + y*y + z*z) + 1e-12
                lat = math.asin(max(-1.0, min(1.0, z / r)))
                lon = math.atan2(y, x)
                u   = int((lon / (2*math.pi) + 0.5) * (tw - 1)) % tw
                v_  = int((0.5 - lat / math.pi) * (th - 1)) % th
                c   = QColor(tex_img.pixel(u, v_))
                colors[i] = [c.redF(), c.greenF(), c.blueF(), 0.95]
        else:
            for i, (x, y, z) in enumerate(verts):
                r     = math.sqrt(x*x + y*y + z*z) + 1e-12
                lat   = math.asin(z / r)
                lon   = math.atan2(y, x)
                lat_d = math.degrees(lat)
                if abs(lat_d) > 70:
                    col = (0.95, 0.97, 1.0, 0.95)
                else:
                    n = (math.sin(3*lon) + 0.6*math.sin(2*lat)
                         + 0.4*math.sin(5*(lon+lat)) + 0.2*math.sin(11*lon+3*lat))
                    if n > 0.35:
                        col = (0.62, 0.52, 0.28, 0.95) if abs(lat_d) < 25 and n > 0.75 else (0.15, 0.45, 0.22, 0.95)
                    else:
                        col = (0.05, 0.22, 0.35, 0.92) if 0.25 < n < 0.35 else (0.05, 0.12, 0.28, 0.92)
                colors[i] = col

        try:
            md.setVertexColors(colors)
            earth_color = (1, 1, 1, 1)
        except Exception:
            earth_color = (0.08, 0.12, 0.2, 0.75)

        self.earth_mesh = gl.GLMeshItem(meshdata=md, smooth=True, color=earth_color,
                                        shader='shaded', glOptions='translucent')
        self.gl_view.addItem(self.earth_mesh)

        # Equator + prime meridian guides
        steps = 200
        eq = np.zeros((steps+1, 3))
        pm = np.zeros((steps+1, 3))
        for k in range(steps+1):
            a = (2*math.pi/steps)*k
            eq[k] = [earth_r*math.cos(a), earth_r*math.sin(a), 0]
            pm[k] = [earth_r*math.cos(a), 0, earth_r*math.sin(a)]
        self.gl_view.addItem(gl.GLLinePlotItem(pos=eq, color=(1,1,1,0.2), width=1, antialias=True))
        self.gl_view.addItem(gl.GLLinePlotItem(pos=pm, color=(1,0.3,0.3,0.22), width=1, antialias=True))

        # Orbit track outlines
        for plane in range(NUM_PLANES):
            pts = np.zeros((101, 3))
            Om  = (2*math.pi/NUM_PLANES)*plane
            r   = SAT_ALTITUDE_KM / 1000.0
            for s in range(101):
                nu = (2*math.pi/100)*s
                pts[s] = [
                    r*(math.cos(Om)*math.cos(nu) - math.sin(Om)*math.sin(nu)*math.cos(INCLINATION_RAD)),
                    r*(math.sin(Om)*math.cos(nu) + math.cos(Om)*math.sin(nu)*math.cos(INCLINATION_RAD)),
                    r*(math.sin(nu)*math.sin(INCLINATION_RAD)),
                ]
            self.gl_view.addItem(gl.GLLinePlotItem(pos=pts, color=(0.15,0.2,0.3,0.35), width=1, antialias=True))

        self.gl_active_scatter   = gl.GLScatterPlotItem()
        self.gl_dead_scatter     = gl.GLScatterPlotItem()
        self.gl_selected_scatter = gl.GLScatterPlotItem()
        self.gl_ground_scatter   = gl.GLScatterPlotItem()
        for item in (self.gl_active_scatter, self.gl_dead_scatter,
                     self.gl_selected_scatter, self.gl_ground_scatter):
            self.gl_view.addItem(item)

        self.gl_network_links   = gl.GLLinePlotItem(mode='lines', color=(0.2,0.3,0.5,0.28), width=1.5, antialias=True)
        self.gl_packet_path     = gl.GLLinePlotItem(color=(1.0,0.8,0.1,1.0), width=3, antialias=True)
        # Transfer path: bright cyan, thicker, rendered on top
        self.gl_transfer_path   = gl.GLLinePlotItem(color=(0.0,1.0,0.9,1.0), width=5, antialias=True)
        self.gl_transfer_ends   = gl.GLScatterPlotItem()   # highlight src/dst GS during transfer
        self.gl_view.addItem(self.gl_network_links)
        self.gl_view.addItem(self.gl_packet_path)
        self.gl_view.addItem(self.gl_transfer_path)
        self.gl_view.addItem(self.gl_transfer_ends)

    # ─────────────────────────────────────────────────────────────────────────
    # CONTROL CALLBACKS
    # ─────────────────────────────────────────────────────────────────────────
    def sat_selected(self, index):
        if index >= 0:
            self.selected_sat_id = index

    def update_speed(self, value):
        self.sim_dt_seconds = float(value)
        self.lbl_speed.setText(f"Sim Speed (ΔT): {value:.0f} s/step")

    def update_severity(self, value):
        self.strike_severity = value / 100.0
        self.lbl_severity.setText(f"Strike Severity: {value}% nodes")

    def toggle_pause(self, paused: bool):
        self.btn_pause.setText("RESUME" if paused else "PAUSE")
        self.timer.stop() if paused else self.timer.start(16)

    def step_once(self):
        if not self.timer.isActive():
            self.update_simulation()

    def toggle_occlusion(self, enabled: bool):
        self.occlusion_enabled = bool(enabled)
        self.btn_occlusion.setText("OCCLUDE: ON" if self.occlusion_enabled else "OCCLUDE: OFF")

    def trigger_kinetic_strike(self):
        self.status_msg = f"CRITICAL: {int(self.strike_severity*100)}% KINETIC LOSS"
        self.metrics.notify_failure(self.last_sim_time_s)
        active_ids = [s.node_id for s in self.sats if s.is_active]
        if not active_ids:
            return
        doomed = random.sample(active_ids, int(len(active_ids) * self.strike_severity))
        for nid in (self.trace_src, self.trace_dst):
            if nid in doomed:
                doomed.remove(nid)
        for sat in self.sats:
            if sat.node_id in doomed:
                sat.is_active = False

    def reset_network(self):
        self.status_msg = "NOMINAL"
        for sat in self.sats:
            sat.is_active = True
        self.router  = self._make_router()
        self.traffic = TrafficSimulator(
            TrafficGenerator(pattern=TRAFFIC_PATTERN, rate_pps=TRAFFIC_RATE_PPS, hotspot_id=self.trace_dst)
        )

    # ─────────────────────────────────────────────────────────────────────────
    # CAMERA / OCCLUSION HELPERS
    # ─────────────────────────────────────────────────────────────────────────
    def _camera_position_world(self) -> np.ndarray:
        try:
            p = self.gl_view.cameraPosition()
            return np.array([float(p.x()), float(p.y()), float(p.z())], dtype=float)
        except Exception:
            try:
                d  = float(self.gl_view.opts.get("distance", 25.0))
                az = math.radians(float(self.gl_view.opts.get("azimuth", 0.0)))
                el = math.radians(float(self.gl_view.opts.get("elevation", 0.0)))
                return np.array([d*math.cos(el)*math.cos(az),
                                 d*math.cos(el)*math.sin(az),
                                 d*math.sin(el)], dtype=float)
            except Exception:
                return np.array([0.0, 0.0, 25.0], dtype=float)

    def _is_occluded_by_earth(self, point_world: np.ndarray, earth_radius_world: float) -> bool:
        cam  = self._camera_position_world()
        p    = np.asarray(point_world, dtype=float)
        d    = p - cam
        dd   = float(np.dot(d, d))
        if dd <= 1e-12:
            return False
        a    = dd
        b    = 2.0 * float(np.dot(cam, d))
        c    = float(np.dot(cam, cam)) - earth_radius_world**2
        disc = b*b - 4*a*c
        if disc <= 0:
            return False
        sq   = math.sqrt(disc)
        t1   = (-b - sq) / (2*a)
        t2   = (-b + sq) / (2*a)
        return (0 < t1 < 1) or (0 < t2 < 1)

    # ─────────────────────────────────────────────────────────────────────────
    # THROTTLED UPDATE HELPERS
    # ─────────────────────────────────────────────────────────────────────────
    def _update_2d_map(self):
        node_ll    = self._node_ll
        edge_set   = self._edge_set
        path_trace = self._path_trace

        # Satellite scatter
        sat_pts = [{"pos": node_ll[sid]} for sid in node_ll if sid < NUM_TOTAL_SATS]
        self.topo_nodes_item.setData(sat_pts)

        # Ground scatter
        gs_pts = [{"pos": (math.degrees(gs.lon_rad), math.degrees(gs.lat_rad))}
                  for gs in self.ground_nodes]
        self.topo_ground_item.setData(gs_pts)

        # ISL edges
        ex, ey = [], []
        for (u, v) in edge_set:
            if u not in node_ll or v not in node_ll:
                continue
            (x1,y1),(x2,y2) = node_ll[u], node_ll[v]
            ex += [x1, x2]; ey += [y1, y2]
        self.topo_edges_item.setData(ex, ey, connect="pairs")

        # Failed edge flash (TTL decay already handled in main loop)
        fx, fy = [], []
        for (u, v, _ttl) in self._failed_edges_ll:
            if u in node_ll and v in node_ll:
                (x1,y1),(x2,y2) = node_ll[u], node_ll[v]
                fx += [x1, x2]; fy += [y1, y2]
        self.topo_failed_item.setData(fx, fy, connect="pairs")

        # Active route
        rx, ry = [], []
        if len(path_trace) > 1:
            for a, b in zip(path_trace[:-1], path_trace[1:]):
                if a in node_ll and b in node_ll:
                    (x1,y1),(x2,y2) = node_ll[a], node_ll[b]
                    rx += [x1, x2]; ry += [y1, y2]
        self.topo_route_item.setData(rx, ry, connect="pairs")

    def _update_ground_table(self):
        for i, gs in enumerate(self.ground_nodes):
            stats = self.gs_link_stats.get(gs.gid)
            if stats:
                self.gs_table.item(i, 1).setText(stats['sat'])
                self.gs_table.item(i, 2).setText(f"{stats['elev_deg']:.1f}")
                self.gs_table.item(i, 3).setText(f"{stats['bw_mbps']:.0f}")
                self.gs_table.item(i, 4).setText(f"{stats['lat_ms']:.1f}")
            else:
                for j in range(1, 5):
                    self.gs_table.item(i, j).setText("NO LINK")
                    self.gs_table.item(i, j).setForeground(
                        pg.QtGui.QColor(220, 60, 60))

    def _update_metrics_plots(self, sim_time_s: float):
        if self.last_step_metrics is None:
            return
        m = self.last_step_metrics
        h = self.metric_hist
        h["t"].append(float(sim_time_s))
        h["delivery"].append(m.delivery_ratio * 100.0)
        h["delay"].append(m.avg_delay_s)
        h["util"].append(m.avg_link_utilization * 100.0)
        if len(h["t"]) > TOPOLOGY_HISTORY_STEPS:
            for k in h:
                h[k] = h[k][-TOPOLOGY_HISTORY_STEPS:]
        x = np.arange(len(h["t"]))
        self.curve_delivery.setData(x, h["delivery"])
        self.curve_delay.setData(x, h["delay"])
        self.curve_util.setData(x, h["util"])

    # ─────────────────────────────────────────────────────────────────────────
    # MAIN SIMULATION LOOP
    # ─────────────────────────────────────────────────────────────────────────
    def update_simulation(self):
        self.step_count     += 1
        self._sim_tick      += 1
        sim_time_s           = self.step_count * self.sim_dt_seconds
        dt_s                 = float(self.sim_dt_seconds)
        self.last_sim_time_s = sim_time_s

        # 1) Physics ──────────────────────────────────────────────────────────
        for sat in self.sats:
            sat.update_physics(dt_s)

        # 2) ISL topology ─────────────────────────────────────────────────────
        active_sats  = [s for s in self.sats if s.is_active]
        active_ids   = [s.node_id for s in active_sats]
        active_ids_all = active_ids + list(self.ground_node_ids)
        r_eci_by_id  = {s.node_id: s.state.r_km for s in active_sats}
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

        # 3) Ground ↔ satellite links ─────────────────────────────────────────
        sat_ecef_by_id = {
            sid: eci_to_ecef(r_eci_by_id[sid], sim_time_s, theta0_rad=0.0)
            for sid in active_ids
        }
        eye         = self._camera_position_world()
        earth_r_w   = EARTH_RADIUS_KM / 1000.0
        ground_pts_scaled = []

        for gs in self.ground_nodes:
            gs_ecef  = gs.ecef_position_km(EARTH_RADIUS_KM)
            p_world  = gs_ecef / 1000.0

            # Rendering visibility (camera-side only)
            cam_side = float(np.dot(p_world, eye)) > earth_r_w * earth_r_w
            if cam_side:
                ground_pts_scaled.append(p_world)

            # Physics: build links for ALL ground stations regardless of camera
            best = None; best_el = -1e9; best_d_km = 0.0
            for sid in active_ids:
                sat_ecef = sat_ecef_by_id[sid]
                if not is_visible_from_station_ecef(gs_ecef, sat_ecef, EARTH_RADIUS_KM, gs.min_elev_deg):
                    continue
                d_km = float(np.linalg.norm(sat_ecef - gs_ecef))
                if d_km > GROUND_SAT_MAX_RANGE_KM:
                    continue

                up       = gs_ecef / float(np.linalg.norm(gs_ecef))
                rho      = sat_ecef - gs_ecef
                rho_norm = float(np.linalg.norm(rho))
                el_sin   = float(np.dot(rho / max(1e-9, rho_norm), up))
                if el_sin > best_el:
                    best_el   = el_sin
                    best      = sid
                    best_d_km = d_km

                latency = d_km / SPEED_OF_LIGHT_KM_S + 0.003
                lb      = inverse_square_budget(d_km, GROUND_BW_Mbps, GROUND_BASE_LOSS, ref_km=800.0)
                props   = LinkProperties(latency_s=float(latency),
                                         bandwidth_mbps=float(lb.bandwidth_mbps),
                                         loss_prob=float(lb.loss_prob))
                self.graph.add_undirected(gs.gid, sid, Link(a=gs.gid, b=sid, distance_km=float(d_km), props=props))

            self.best_sat_for_station[gs.gid] = best

            # Track stats for ground panel
            if best is not None:
                lb_best  = inverse_square_budget(best_d_km, GROUND_BW_Mbps, GROUND_BASE_LOSS, ref_km=800.0)
                el_deg   = math.degrees(math.asin(max(-1.0, min(1.0, best_el))))
                lat_ms   = (best_d_km / SPEED_OF_LIGHT_KM_S + 0.003) * 1000.0
                sat_name = self.sats[best].name if best < NUM_TOTAL_SATS else f"SAT-{best}"
                self.gs_link_stats[gs.gid] = {
                    'sat':      sat_name,
                    'elev_deg': el_deg,
                    'dist_km':  best_d_km,
                    'bw_mbps':  lb_best.bandwidth_mbps,
                    'lat_ms':   lat_ms,
                }
            else:
                self.gs_link_stats[gs.gid] = None

        # 4) Failure / impairments ────────────────────────────────────────────
        self.failure_model.apply(self.graph, dt_s=dt_s)

        # 5) Routing ──────────────────────────────────────────────────────────
        self.router.step(self.graph, active_ids_all)

        # 6) Traffic ──────────────────────────────────────────────────────────
        self.last_traffic_stats = self.traffic.step(
            graph=self.graph, router=self.router,
            active_ids=active_ids_all, now_t=sim_time_s, dt_s=dt_s,
        )
        self.last_step_metrics = self.metrics.step(
            graph=self.graph, router=self.router,
            active_ids=active_ids_all,
            traffic_stats=self.last_traffic_stats, now_s=sim_time_s,
        )

        # 7) Neighbor lists (inspector) ───────────────────────────────────────
        for s in self.sats:
            s.neighbors = []
        for u in active_ids:
            self.sats[u].neighbors = [
                n for n in self.graph.neighbors(u) if n < NUM_TOTAL_SATS
            ]

        # 8) Compute node lat/lon lookup (shared by GL path + 2D map) ─────────
        node_ll: dict = {}
        for sid in active_ids:
            r_ecef = sat_ecef_by_id.get(sid)
            if r_ecef is None:
                continue
            lat_r, lon_r, _ = ecef_to_geodetic_spherical(r_ecef)
            node_ll[sid] = (math.degrees(lon_r), math.degrees(lat_r))
        for gs in self.ground_nodes:
            node_ll[gs.gid] = (math.degrees(gs.lon_rad), math.degrees(gs.lat_rad))
        self._node_ll = node_ll

        # 9) Active route path ────────────────────────────────────────────────
        path_trace = []
        if (0 <= self.trace_src < len(self.sats) and 0 <= self.trace_dst < len(self.sats)
                and self.sats[self.trace_src].is_active and self.sats[self.trace_dst].is_active):
            path_trace = self.router.path(self.trace_src, self.trace_dst, max_hops=256)
        self._path_trace = path_trace

        # 10) Edge set + failed edge tracking ─────────────────────────────────
        edge_set = set()
        for u, v, _ in self.graph.edges():
            edge_set.add((min(u,v), max(u,v)))

        removed = self.prev_edge_set - edge_set
        for (u, v) in removed:
            self.failed_edges.append((u, v, 40))
        self.prev_edge_set = set(edge_set)

        surviving = []
        for (u, v, ttl) in self.failed_edges:
            if ttl > 0:
                surviving.append((u, v, ttl-1))
        self.failed_edges       = surviving
        self._failed_edges_ll   = surviving
        self._edge_set          = edge_set

        # 11) 3D GL render (every tick) ───────────────────────────────────────
        link_coords = []
        for u, v, _ in self.graph.edges():
            pu = self.sats[u].pos_3d if u < NUM_TOTAL_SATS else (
                self.ground_nodes[u - NUM_TOTAL_SATS].ecef_position_km(EARTH_RADIUS_KM) / 1000.0)
            pv = self.sats[v].pos_3d if v < NUM_TOTAL_SATS else (
                self.ground_nodes[v - NUM_TOTAL_SATS].ecef_position_km(EARTH_RADIUS_KM) / 1000.0)
            if self.occlusion_enabled:
                mid = (np.asarray(pu) + np.asarray(pv)) * 0.5
                if self._is_occluded_by_earth(mid, earth_r_w):
                    continue
            link_coords.append(pu)
            link_coords.append(pv)

        if ground_pts_scaled:
            self.gl_ground_scatter.setData(pos=np.array(ground_pts_scaled),
                                           color=(0.58,0.65,1.0,0.95), size=7, pxMode=True)
        else:
            self.gl_ground_scatter.setData(pos=np.empty((0,3)))

        active_pts = np.array([s.pos_3d for s in self.sats if s.is_active])
        dead_pts   = np.array([s.pos_3d for s in self.sats if not s.is_active])

        self.gl_active_scatter.setData(
            pos=active_pts if len(active_pts) else np.empty((0,3)),
            color=(0.24,0.72,0.31,0.9), size=6, pxMode=True)
        self.gl_dead_scatter.setData(
            pos=dead_pts if len(dead_pts) else np.empty((0,3)),
            color=(0.85,0.21,0.2,0.8), size=8, pxMode=True)
        self.gl_network_links.setData(
            pos=np.array(link_coords) if link_coords else np.empty((0,3)))

        if len(path_trace) > 1:
            pts = np.array([self.sats[pid].pos_3d for pid in path_trace], dtype=float)
            if self.occlusion_enabled:
                mid = pts[len(pts)//2]
                if self._is_occluded_by_earth(mid, earth_r_w):
                    pts = np.empty((0,3))
            self.gl_packet_path.setData(pos=pts)
        else:
            self.gl_packet_path.setData(pos=np.empty((0,3)))

        if self.selected_sat_id is not None:
            sat = self.sats[self.selected_sat_id]
            self.gl_selected_scatter.setData(
                pos=np.array([sat.pos_3d]), color=(1,1,1,1), size=12, pxMode=True)

        # 11b) File transfer — physics step + 3D path render ──────────────────
        self._update_transfer_session(sim_time_s, dt_s)

        if self._transfer_path_pts.shape[0] > 1 and self.transfer is not None \
                and self.transfer.status == FileTransferSession.STATUS_TRANSFERRING:
            # Pulse alpha with sim tick for a "data flowing" visual
            pulse = 0.7 + 0.3 * math.sin(self._sim_tick * 0.4)
            self.gl_transfer_path.setData(pos=self._transfer_path_pts,
                                          color=(0.0, 1.0, 0.9, pulse))
            # Highlight src and dst ground stations
            src_gs = self.ground_nodes[self.transfer.src_gs_id - NUM_TOTAL_SATS]
            dst_gs = self.ground_nodes[self.transfer.dst_gs_id - NUM_TOTAL_SATS]
            end_pts = np.array([
                src_gs.ecef_position_km(EARTH_RADIUS_KM) / 1000.0,
                dst_gs.ecef_position_km(EARTH_RADIUS_KM) / 1000.0,
            ])
            self.gl_transfer_ends.setData(pos=end_pts, color=(0.0, 1.0, 0.9, 1.0),
                                          size=14, pxMode=True)
        elif self.transfer is not None and self.transfer.status == FileTransferSession.STATUS_COMPLETE:
            # Keep path visible in green when done
            if self._transfer_path_pts.shape[0] > 1:
                self.gl_transfer_path.setData(pos=self._transfer_path_pts,
                                              color=(0.24, 0.92, 0.31, 0.9))
        else:
            self.gl_transfer_path.setData(pos=np.empty((0, 3)))
            self.gl_transfer_ends.setData(pos=np.empty((0, 3)))

        # 12) Telemetry HUD (every tick) ──────────────────────────────────────
        color_hex = "#ff7b72" if "CRITICAL" in self.status_msg else "#3fb950"
        if self.last_traffic_stats is not None:
            t = self.last_traffic_stats
            m = self.last_step_metrics
            traffic_html = (
                f"<b>Traffic</b>  in-flight={t.in_flight}  "
                f"delivered={t.delivered}  dropped={t.dropped}<br>"
                f"<b>Avg Delay:</b> {t.avg_delay_s:.3f} s &nbsp; "
                f"<b>Avg Hops:</b> {t.avg_hops:.2f}<br>"
                f"<b>Delivery:</b> {m.delivery_ratio*100:.1f}% &nbsp; "
                f"<b>Path:</b> {m.avg_path_len_hops:.1f} hops<br>"
                f"<b>Link Util:</b> {m.avg_link_utilization*100:.1f}% &nbsp; "
                f"<b>Rt Updates:</b> {m.route_updates}<br>"
                f"<b>Router:</b> {self.router.name} / {ROUTING_COST}<br><br>"
            )
        else:
            traffic_html = ""
        self.lbl_telemetry.setText(
            f"<b>GLOBAL TELEMETRY</b><br><br>"
            f"<b>T+</b> {sim_time_s:.1f} s<br>"
            f"<b>Status:</b> <span style='color:{color_hex}'>{self.status_msg}</span><br>"
            f"<b>Nodes:</b> {len(active_pts)} / {NUM_TOTAL_SATS}<br>"
            f"<b>ISLs:</b> {len(link_coords)//2}<br><br>"
            f"{traffic_html}"
            f"<b>Tracer:</b> {self.trace_src} → {self.trace_dst}"
        )

        # Inspector (only if inspect tab active or sat selected) ──────────────
        if self.selected_sat_id is not None:
            sat = self.sats[self.selected_sat_id]
            s_color = "#3fb950" if sat.is_active else "#da3633"
            s_text  = "ONLINE"  if sat.is_active else "OFFLINE"
            try:
                coe = sat.coe
                a_km, e = coe.a_km, coe.e
                inc_deg  = math.degrees(coe.i_rad)
                raan_deg = math.degrees(coe.raan_rad)
            except Exception:
                a_km = e = inc_deg = raan_deg = float("nan")
            try:
                r_ecef = eci_to_ecef(sat.state.r_km, sim_time_s, theta0_rad=0.0)
                lat_r, lon_r, rmag = ecef_to_geodetic_spherical(r_ecef)
                lat_deg = math.degrees(lat_r)
                lon_deg = (math.degrees(lon_r) + 540) % 360 - 180
                alt_km  = rmag - EARTH_RADIUS_KM
            except Exception:
                lat_deg = lon_deg = alt_km = float("nan")
            self.lbl_spec.setText(
                f"<span style='color:#58a6ff;font-weight:bold;'>{sat.name}</span><hr>"
                f"<b>Status:</b> <span style='color:{s_color}'>{s_text}</span><br>"
                f"<b>Plane:</b> {sat.plane_idx}<br>"
                f"<b>a:</b> {a_km:.1f} km  <b>e:</b> {e:.4f}  "
                f"<b>i:</b> {inc_deg:.1f}°  <b>Ω:</b> {raan_deg:.1f}°<br>"
                f"<b>ν:</b> {sat.nu_rad:.3f} rad<br>"
                f"<b>Subpoint:</b> {lat_deg:.2f}°, {lon_deg:.2f}°, {alt_km:.1f} km<br>"
                f"<b>Neighbors:</b> {len(sat.neighbors)}<br><br>"
                f"<b>r_ECI (km):</b> [{sat.pos_3d[0]*1000:.0f}, {sat.pos_3d[1]*1000:.0f}, {sat.pos_3d[2]*1000:.0f}]<br>"
                f"<b>v_ECI (km/s):</b> [{sat.vel_eci_km_s[0]:.3f}, {sat.vel_eci_km_s[1]:.3f}, {sat.vel_eci_km_s[2]:.3f}]"
            )

        # 13) Metrics summary bar (every tick, cheap text) ────────────────────
        if self.last_step_metrics is not None:
            m = self.last_step_metrics
            self.lbl_metrics_summary.setText(
                f"Delivery: {m.delivery_ratio*100:.1f}%  |  "
                f"Delay: {m.avg_delay_s:.3f} s  |  "
                f"Util: {m.avg_link_utilization*100:.1f}%  |  "
                f"Avg Hops: {m.avg_path_len_hops:.1f}  |  "
                f"T+ {sim_time_s:.0f} s"
            )

        # 14) Throttled updates ───────────────────────────────────────────────
        if self._sim_tick % PLOT_UPDATE_EVERY == 0:
            self._update_ground_table()
            if self._metrics_expanded:
                self._update_metrics_plots(sim_time_s)

        MAP_TAB = 3
        if self._sim_tick % MAP_UPDATE_EVERY == 0 and self.tab_panel.currentIndex() == MAP_TAB:
            self._update_2d_map()

        # 15) CSV logging ─────────────────────────────────────────────────────
        if self.run_logger is not None and self.last_traffic_stats is not None:
            t = self.last_traffic_stats
            m = self.last_step_metrics
            self.run_logger.log_row({
                "time_s":              float(sim_time_s),
                "active_sats":         int(len(active_pts)),
                "links_total":         int(len(link_coords)//2),
                "router":              str(self.router.name),
                "routing_cost":        str(ROUTING_COST),
                "traffic_pattern":     str(TRAFFIC_PATTERN),
                "in_flight":           int(t.in_flight),
                "delivered_total":     int(t.delivered),
                "dropped_total":       int(t.dropped),
                "avg_delay_s":         float(t.avg_delay_s),
                "avg_hops":            float(t.avg_hops),
                "delivery_ratio":      float(m.delivery_ratio) if m else 0.0,
                "avg_path_len_hops":   float(m.avg_path_len_hops) if m else 0.0,
                "avg_link_utilization":float(m.avg_link_utilization) if m else 0.0,
                "route_updates":       int(m.route_updates) if m else 0,
                "convergence_s":       float(m.convergence_s) if (m and m.convergence_s) else "",
            })


# ─────────────────────────────────────────────────────────────────────────────
if __name__ == '__main__':
    app = QApplication(sys.argv)
    app.setStyleSheet(DARK_THEME)
    window = MainWindow()
    window.show()
    sys.exit(app.exec())
