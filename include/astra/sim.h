/* sim.h — simulation orchestrator + lock-free sim<->render boundary.
 *
 * The simulation thread is the sole writer of all truth. It publishes immutable
 * RenderSnapshots through a lock-free triple buffer; the render thread reads the
 * latest complete snapshot. Commands flow render->sim through an SPSC ring. A
 * node strike is therefore a command applied at a defined point in the tick, so
 * no snapshot can ever reference a half-dead node (strike safety by design). */
#ifndef ASTRA_SIM_H
#define ASTRA_SIM_H

#include "astra/orbit.h"
#include "astra/graph.h"
#include "astra/routing.h"
#include "astra/traffic.h"
#include "astra/metrics.h"
#include "astra/failures.h"
#include "astra/ground.h"
#include <stdatomic.h>

/* ---- Satellite field (hot state + cold elements) ------------------------- */
typedef struct {
    StateRV       state[ASTRA_MAX_SATS];
    OrbitElements coe0 [ASTRA_MAX_SATS];   /* initial elements (for reboot)   */
    uint8_t       alive[ASTRA_MAX_SATS];
    uint16_t      plane[ASTRA_MAX_SATS];
    uint16_t      slot [ASTRA_MAX_SATS];
    uint32_t      count;
} SatField;

/* ---- Immutable render snapshot: the ONLY thing the render thread reads ---- */
typedef struct { vec3 r; uint8_t alive; }                              SnapSat;
typedef struct { node_id u, v; uint8_t up; float util, latency_ms, dist_km, bw_mbps; } SnapLink;
typedef struct { node_id gid, best_sat; double lat_rad, lon_rad; vec3 r; } SnapGS;

typedef struct {
    uint64_t frame_id;
    double   sim_time_s;
    uint32_t step_count;
    uint32_t sat_count, link_count, gs_count, active_nodes;
    SnapSat  sat [ASTRA_MAX_SATS];
    SnapLink link[ASTRA_MAX_LINKS];
    SnapGS   gs  [ASTRA_MAX_GROUND];
    float    delivery_ratio, avg_delay_s, avg_hops, link_util;
    uint32_t route_updates;
} RenderSnapshot;

/* ---- Commands: render -> sim (SPSC ring) --------------------------------- */
typedef enum { CMD_STRIKE, CMD_REBOOT_ALL, CMD_ROUTE_MODE, CMD_COST_MODE,
               CMD_SPEED, CMD_PAUSE, CMD_RATE } CmdType;
typedef struct { CmdType type; uint32_t u; double f; } Command;
#define ASTRA_CMD_RING 256u

/* ---- Optional per-stage profiling (off by default, ~0 cost when off) ----- */
typedef enum {
    PF_PROPAGATE, PF_ACTIVE, PF_TOPOLOGY, PF_GROUND, PF_FAILURES,
    PF_CSR, PF_ROUTING, PF_TRAFFIC, PF_METRICS, PF_NSTAGES
} ProfStage;
typedef struct {
    int      enabled;
    uint64_t ns[PF_NSTAGES];   /* accumulated nanoseconds per stage */
    uint64_t samples;          /* number of advance() calls measured */
} SimProfile;
extern const char *const astra_prof_stage_name[PF_NSTAGES];

typedef struct {
    /* ---- simulation truth (sim thread owns exclusively) ---- */
    uint32_t      num_sats, ngs, node_count;
    double        earth_r, mu, max_range, gs_max_range;
    SatField      field;
    GroundStation gs[ASTRA_MAX_GROUND];
    node_id       active_ids[ASTRA_MAX_NODES];    /* alive sats + ground   */
    uint32_t      active_count;
    node_id       sat_alive_ids[ASTRA_MAX_SATS];  /* alive sats only       */
    uint32_t      sat_alive_count;
    vec3          scratch_pos[ASTRA_MAX_NODES];    /* sat ECI by id         */
    node_id       best_sat[ASTRA_MAX_GROUND];

    NetworkGraph     graph;
    Router           router;
    TrafficSim       traffic;
    MetricsCollector metrics;
    LinkImpairments  impair;
    Rng              fail_rng;

    double    sim_time_s, sim_dt_s, speed;
    int       paused;
    uint64_t  step_count;
    RouteMode route_mode;
    CostMode  cost;
    StepMetrics last_metrics;

    /* ---- publish channel: triple buffer + atomic index ---- */
    RenderSnapshot   snap[3];
    _Atomic uint32_t tb_shared;     /* mid index (bits0-1) | dirty (bit2) */
    uint32_t         tb_write, tb_read;
    _Atomic uint64_t frame_seq;

    /* ---- command channel: SPSC ring ---- */
    Command          cmd[ASTRA_CMD_RING];
    _Atomic uint32_t cmd_head, cmd_tail;

    /* ---- optional profiling (sim thread fills; set prof.enabled=1) ---- */
    SimProfile       prof;
} SimState;

/* ---- lifecycle ---------------------------------------------------------- */
void astra_sim_init(SimState *s, uint64_t seed);    /* default 10x10 shell */
/* Custom Walker-Delta shell (planes*per_plane must be <= ASTRA_MAX_SATS).
 * max_range_km <= 0 selects the config default. */
void astra_sim_init_cfg(SimState *s, uint64_t seed, uint32_t planes,
                        uint32_t per_plane, double max_range_km);
void astra_sim_tick(SimState *s);          /* drain commands, one step, publish */

/* ---- render-side API (call from the render thread) ---------------------- */
/* Returns the newest complete snapshot, or NULL if none newer than last call.
 * The pointer stays valid until the next acquire call. */
const RenderSnapshot *astra_snapshot_acquire(SimState *s);
int  astra_cmd_push(SimState *s, Command c);   /* 0 if ring full */

#endif /* ASTRA_SIM_H */
