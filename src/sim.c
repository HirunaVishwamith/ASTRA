/* sim.c — simulation orchestrator + lock-free sim<->render boundary.
 * Ties together orbit/graph/routing/ground/failures/traffic/metrics into the
 * fixed-step tick, and implements the triple buffer + SPSC command ring. */
#include "astra/sim.h"
#include <string.h>
#include <math.h>
#include <time.h>

#define TB_MASK  3u
#define TB_DIRTY 4u

const char *const astra_prof_stage_name[PF_NSTAGES] = {
    "propagate", "active", "topology", "ground",
    "failures", "csr", "routing", "traffic", "metrics"
};

/* Monotonic nanosecond clock for stage timing. */
static inline uint64_t astra_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- construction -------------------------------------------------------- */
void astra_sim_init_cfg(SimState *s, uint64_t seed, uint32_t planes,
                        uint32_t per_plane, double max_range_km) {
    memset(s, 0, sizeof(*s));
    if (planes < 1u) planes = 1u;
    if (per_plane < 1u) per_plane = 1u;
    if ((uint64_t)planes * per_plane > ASTRA_MAX_SATS)
        per_plane = ASTRA_MAX_SATS / planes;     /* clamp to capacity */

    s->num_sats = planes * per_plane;
    s->earth_r = ASTRA_EARTH_RADIUS_KM;
    s->mu = ASTRA_MU_EARTH;
    s->max_range = (max_range_km > 0.0) ? max_range_km : ASTRA_MAX_LINK_RANGE_KM;
    s->gs_max_range = ASTRA_GROUND_SAT_MAX_RANGE_KM;
    s->sim_dt_s = 5.0;
    s->speed = 1.0;
    s->paused = 0;
    s->route_mode = ASTRA_ROUTE_DIJKSTRA;
    s->cost = ASTRA_COST_LATENCY;
    s->impair = astra_impair_none();

    /* constellation (Walker-Delta: RAAN per plane, phase per slot + plane skew) */
    SatField *f = &s->field;
    f->count = s->num_sats;
    for (uint32_t pl = 0; pl < planes; ++pl)
        for (uint32_t sl = 0; sl < per_plane; ++sl) {
            node_id id = pl * per_plane + sl;
            OrbitElements c = { ASTRA_SAT_SMA_KM, ASTRA_SAT_ECC,
                ASTRA_INCLINATION_DEG * M_PI/180.0, (2.0*M_PI/planes)*pl, 0.0,
                (2.0*M_PI/per_plane)*sl + pl*0.1 };
            f->coe0[id] = c;
            f->state[id] = astra_coe_to_rv(s->mu, c);
            f->alive[id] = 1;
            f->plane[id] = (uint16_t)pl;
            f->slot[id]  = (uint16_t)sl;
        }

    /* ground stations: gid = num_sats + i */
    s->ngs = astra_default_ground_stations(s->gs, s->num_sats);
    s->node_count = s->num_sats + s->ngs;

    astra_router_init(&s->router, s->route_mode, s->cost);
    astra_traffic_init(&s->traffic, TR_UNIFORM, 3.0, s->num_sats, seed);
    astra_metrics_init(&s->metrics, 40, seed ^ 0x9e3779b9u);
    rng_seed(&s->fail_rng, seed ^ 0xC0FFEEu, 5u);

    /* triple buffer: write=0, read=1, mid=2, not dirty */
    s->tb_write = 0; s->tb_read = 1;
    atomic_store(&s->tb_shared, 2u);
    atomic_store(&s->frame_seq, 0u);
    atomic_store(&s->cmd_head, 0u);
    atomic_store(&s->cmd_tail, 0u);
}

void astra_sim_init(SimState *s, uint64_t seed) {
    astra_sim_init_cfg(s, seed, ASTRA_NUM_PLANES, ASTRA_NUM_SATS_PER_PLANE, 0.0);
}

/* ---- command ring -------------------------------------------------------- */
int astra_cmd_push(SimState *s, Command c) {
    uint32_t head = atomic_load_explicit(&s->cmd_head, memory_order_relaxed);
    uint32_t next = (head + 1u) % ASTRA_CMD_RING;
    if (next == atomic_load_explicit(&s->cmd_tail, memory_order_acquire)) return 0; /* full */
    s->cmd[head] = c;
    atomic_store_explicit(&s->cmd_head, next, memory_order_release);
    return 1;
}

static void apply_command(SimState *s, Command c) {
    switch (c.type) {
    case CMD_STRIKE:
        if (c.u < s->num_sats && s->field.alive[c.u]) {
            s->field.alive[c.u] = 0;
            astra_metrics_notify_failure(&s->metrics, s->sim_time_s);
        }
        break;
    case CMD_REBOOT_ALL:
        for (uint32_t i = 0; i < s->num_sats; ++i) {
            if (!s->field.alive[i]) {
                /* re-sync revived sat to current epoch from initial elements */
                StateRV st0 = astra_coe_to_rv(s->mu, s->field.coe0[i]);
                s->field.state[i] = astra_propagate_kepler(s->mu, st0.r, st0.v, s->sim_time_s);
                s->field.alive[i] = 1;
            }
        }
        break;
    case CMD_ROUTE_MODE:
        s->route_mode = (c.u == 1u) ? ASTRA_ROUTE_DV : ASTRA_ROUTE_DIJKSTRA;
        astra_router_init(&s->router, s->route_mode, s->cost);
        break;
    case CMD_COST_MODE:
        s->cost = (c.u == 1u) ? ASTRA_COST_HOPS : ASTRA_COST_LATENCY;
        astra_router_init(&s->router, s->route_mode, s->cost);
        break;
    case CMD_SPEED: s->speed = c.f; break;
    case CMD_PAUSE: s->paused = (int)c.u; break;
    case CMD_RATE:  s->traffic.gen.rate_pps = c.f; break;
    }
}

static void drain_commands(SimState *s) {
    for (;;) {
        uint32_t tail = atomic_load_explicit(&s->cmd_tail, memory_order_relaxed);
        if (tail == atomic_load_explicit(&s->cmd_head, memory_order_acquire)) break;
        apply_command(s, s->cmd[tail]);
        atomic_store_explicit(&s->cmd_tail, (tail + 1u) % ASTRA_CMD_RING, memory_order_release);
    }
}

/* ---- publish: fill snapshot + triple-buffer swap ------------------------- */
static void publish(SimState *s) {
    RenderSnapshot *snap = &s->snap[s->tb_write];
    snap->frame_id   = atomic_fetch_add(&s->frame_seq, 1u) + 1u;
    snap->sim_time_s = s->sim_time_s;
    snap->step_count = (uint32_t)s->step_count;
    snap->active_nodes = s->active_count;

    snap->sat_count = s->num_sats;
    for (uint32_t i = 0; i < s->num_sats; ++i) {
        snap->sat[i].r = s->field.state[i].r;
        snap->sat[i].alive = s->field.alive[i];
    }
    snap->gs_count = s->ngs;
    for (uint32_t i = 0; i < s->ngs; ++i) {
        snap->gs[i].gid = s->gs[i].gid;
        snap->gs[i].best_sat = s->best_sat[i];
        snap->gs[i].lat_rad = s->gs[i].lat_rad;
        snap->gs[i].lon_rad = s->gs[i].lon_rad;
    }

    snap->link_count = s->graph.link_count;
    for (uint32_t e = 0; e < s->graph.link_count; ++e) {
        const Link *L = &s->graph.links[e];
        SnapLink *sl = &snap->link[e];
        sl->u = L->a; sl->v = L->b; sl->up = L->up;
        sl->dist_km = (float)L->distance_km;
        sl->latency_ms = (float)(L->props.latency_s * 1000.0);
        sl->bw_mbps = L->props.bandwidth_mbps;
        double cap = (double)L->props.bandwidth_mbps * 1e6 * s->sim_dt_s;
        double used = s->traffic.cap_used[2u*e] + s->traffic.cap_used[2u*e + 1u];
        sl->util = (cap > 0.0) ? (float)(used / (2.0 * cap)) : 0.0f;
    }

    snap->delivery_ratio = (float)s->last_metrics.delivery_ratio;
    snap->avg_delay_s    = (float)s->last_metrics.avg_delay_s;
    snap->avg_hops       = (float)s->last_metrics.avg_hops;
    snap->link_util      = (float)s->last_metrics.avg_link_util;
    snap->route_updates  = s->last_metrics.route_updates;

    uint32_t newsh = s->tb_write | TB_DIRTY;
    uint32_t old = atomic_exchange(&s->tb_shared, newsh);
    s->tb_write = old & TB_MASK;
}

const RenderSnapshot *astra_snapshot_acquire(SimState *s) {
    uint32_t cur = atomic_load(&s->tb_shared);
    if (!(cur & TB_DIRTY)) return NULL;             /* nothing newer */
    uint32_t old = atomic_exchange(&s->tb_shared, s->tb_read);
    s->tb_read = old & TB_MASK;
    return &s->snap[s->tb_read];
}

/* ---- the fixed-step tick ------------------------------------------------- */
/* STAGE(id) closes the previous stage's interval and opens the next; when
 * profiling is off it compiles down to nothing measurable (one branch). */
#define STAGE(id) do { if (prof) { uint64_t _t = astra_now_ns(); \
    s->prof.ns[_stage] += _t - _mark; _mark = _t; _stage = (id); } } while (0)

static void advance(SimState *s) {
    SatField *f = &s->field;
    double dt = s->sim_dt_s;
    const int prof = s->prof.enabled;
    uint64_t _mark = prof ? astra_now_ns() : 0;
    ProfStage _stage = PF_PROPAGATE;

    /* 1) physics: propagate alive sats */
    for (uint32_t i = 0; i < s->num_sats; ++i) {
        if (!f->alive[i]) continue;
        f->state[i] = astra_propagate_kepler(s->mu, f->state[i].r, f->state[i].v, dt);
    }
    STAGE(PF_ACTIVE);

    /* 2) active sets + position lookup */
    s->sat_alive_count = 0;
    s->active_count = 0;
    for (uint32_t i = 0; i < s->num_sats; ++i) {
        if (!f->alive[i]) continue;
        s->scratch_pos[i] = f->state[i].r;
        s->sat_alive_ids[s->sat_alive_count++] = i;
        s->active_ids[s->active_count++] = i;
    }
    for (uint32_t i = 0; i < s->ngs; ++i)
        s->active_ids[s->active_count++] = s->gs[i].gid;

    s->sim_time_s += dt;          /* sim_time used for ECEF this tick */
    s->step_count++;
    STAGE(PF_TOPOLOGY);

    /* 3) ISL topology */
    astra_build_isl_topology(&s->graph, s->scratch_pos, s->sat_alive_ids,
                             s->sat_alive_count, s->earth_r, s->max_range);
    STAGE(PF_GROUND);

    /* 4) ground links */
    astra_add_ground_links(&s->graph, s->gs, s->ngs, s->scratch_pos,
                           s->sat_alive_ids, s->sat_alive_count,
                           s->sim_time_s, s->earth_r, s->gs_max_range, s->best_sat);
    STAGE(PF_FAILURES);

    /* 5) impairments */
    astra_failures_apply(&s->graph, &s->impair, dt, &s->fail_rng);
    STAGE(PF_CSR);

    /* 6) CSR (skips down links) */
    astra_build_csr(&s->graph, s->node_count, s->cost);
    STAGE(PF_ROUTING);

    /* 7) routing */
    astra_router_step(&s->router, &s->graph.csr);
    STAGE(PF_TRAFFIC);

    /* 8) traffic */
    astra_traffic_step(&s->traffic, &s->graph, &s->router,
                       s->active_ids, s->active_count, s->sim_time_s, dt);
    STAGE(PF_METRICS);

    /* 9) metrics */
    s->last_metrics = astra_metrics_step(&s->metrics, &s->router,
                                         s->active_ids, s->active_count,
                                         &s->traffic.stats, s->sim_time_s);
    if (prof) { s->prof.ns[_stage] += astra_now_ns() - _mark; s->prof.samples++; }
}
#undef STAGE

void astra_sim_tick(SimState *s) {
    drain_commands(s);
    if (!s->paused) advance(s);
    publish(s);
}
