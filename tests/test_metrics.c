/* test_metrics.c — invariant checks for the metrics collector:
 *  - delivery_ratio matches the traffic accessor
 *  - route_updates: 0 on first step and on an unchanged topology
 *  - convergence: detected (>=0) once churn returns to 0 after notify_failure
 *  - avg_path_len within [0, n] */
#include "astra/metrics.h"
#include "astra/routing.h"
#include "astra/graph.h"
#include "astra/orbit.h"
#include "astra/config.h"
#include <stdio.h>
#include <string.h>

static NetworkGraph G;
static Router R;
static MetricsCollector M;
static StateRV st[ASTRA_NUM_TOTAL_SATS];
static vec3 pos[ASTRA_MAX_NODES];
static node_id ids[ASTRA_NUM_TOTAL_SATS];

static void init_constellation(void) {
    for (uint32_t pl = 0; pl < ASTRA_NUM_PLANES; ++pl)
        for (uint32_t s = 0; s < ASTRA_NUM_SATS_PER_PLANE; ++s) {
            node_id id = pl*ASTRA_NUM_SATS_PER_PLANE + s;
            OrbitElements c = { ASTRA_SAT_SMA_KM, ASTRA_SAT_ECC,
                ASTRA_INCLINATION_DEG*M_PI/180.0, (2.0*M_PI/ASTRA_NUM_PLANES)*pl, 0.0,
                (2.0*M_PI/ASTRA_NUM_SATS_PER_PLANE)*s + pl*0.1 };
            st[id] = astra_coe_to_rv(ASTRA_MU_EARTH, c); ids[id] = id;
        }
}
static void retopo(void) {
    for (uint32_t i = 0; i < ASTRA_NUM_TOTAL_SATS; ++i) pos[i] = st[i].r;
    astra_build_isl_topology(&G, pos, ids, ASTRA_NUM_TOTAL_SATS, ASTRA_EARTH_RADIUS_KM, ASTRA_MAX_LINK_RANGE_KM);
    astra_build_csr(&G, ASTRA_NUM_TOTAL_SATS, ASTRA_COST_LATENCY);
    astra_router_init(&R, ASTRA_ROUTE_DIJKSTRA, ASTRA_COST_LATENCY);
    astra_router_step(&R, &G.csr);
}

int main(void) {
    int fails = 0;
    init_constellation();
    astra_metrics_init(&M, 40, 7);

    TrafficStats ts; memset(&ts, 0, sizeof(ts));
    ts.delivered = 8; ts.drop_loss = 2; ts.sum_delay_s = 16.0; ts.sum_hops = 24;

    retopo();
    StepMetrics m1 = astra_metrics_step(&M, &R, ids, ASTRA_NUM_TOTAL_SATS, &ts, 5.0);
    if (m1.route_updates != 0) { printf("first-step churn != 0: %u\n", m1.route_updates); fails++; }
    if (m1.delivery_ratio < 0.7999 || m1.delivery_ratio > 0.8001) { printf("delivery ratio %f\n", m1.delivery_ratio); fails++; }
    if (m1.avg_delay_s < 1.999 || m1.avg_delay_s > 2.001) { printf("avg delay %f\n", m1.avg_delay_s); fails++; }
    if (m1.avg_path_len_hops < 0 || m1.avg_path_len_hops > ASTRA_NUM_TOTAL_SATS) { printf("path len %f\n", m1.avg_path_len_hops); fails++; }

    /* unchanged topology -> zero churn */
    astra_router_step(&R, &G.csr);
    StepMetrics m2 = astra_metrics_step(&M, &R, ids, ASTRA_NUM_TOTAL_SATS, &ts, 10.0);
    if (m2.route_updates != 0) { printf("static churn != 0: %u\n", m2.route_updates); fails++; }

    /* failure event, then convergence once churn returns to 0 */
    astra_metrics_notify_failure(&M, 12.0);
    astra_router_step(&R, &G.csr);
    StepMetrics m3 = astra_metrics_step(&M, &R, ids, ASTRA_NUM_TOTAL_SATS, &ts, 15.0);
    if (m3.convergence_s < 0) { printf("convergence not detected: %f\n", m3.convergence_s); fails++; }

    /* changing topology should produce churn at least once over many steps */
    uint32_t churn_seen = 0;
    for (int k = 0; k < 50; ++k) {
        for (uint32_t i = 0; i < ASTRA_NUM_TOTAL_SATS; ++i) st[i] = astra_propagate_kepler(ASTRA_MU_EARTH, st[i].r, st[i].v, 30.0);
        retopo();
        StepMetrics mk = astra_metrics_step(&M, &R, ids, ASTRA_NUM_TOTAL_SATS, &ts, 100.0 + k*30.0);
        churn_seen += mk.route_updates;
    }
    if (churn_seen == 0) { printf("no churn under moving topology\n"); fails++; }

    printf("delivery=%.3f avg_delay=%.3f avg_hops=%.3f path_len=%.2f conv=%.1f churn_total=%u\n",
           m1.delivery_ratio, m1.avg_delay_s, m1.avg_hops, m1.avg_path_len_hops, m3.convergence_s, churn_seen);
    printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
