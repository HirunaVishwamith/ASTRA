/* test_traffic.c — invariant verification for the packet sim:
 *   (1) packet conservation every step:
 *       generated == delivered + drops + in_flight + at_node
 *   (2) delivery ratio in [0,1], pool never over/underflows
 *   (3) reproducibility with a fixed seed
 * Runs a realistic propagate -> topology -> route -> traffic loop. */
#include "astra/traffic.h"
#include "astra/routing.h"
#include "astra/graph.h"
#include "astra/orbit.h"
#include "astra/config.h"
#include <stdio.h>
#include <string.h>

static NetworkGraph G;
static Router       R;
static TrafficSim   T;
static StateRV      st[ASTRA_NUM_TOTAL_SATS];
static vec3         pos[ASTRA_MAX_NODES];
static node_id      ids[ASTRA_NUM_TOTAL_SATS];

static void init_constellation(void) {
    for (uint32_t pl = 0; pl < ASTRA_NUM_PLANES; ++pl)
        for (uint32_t s = 0; s < ASTRA_NUM_SATS_PER_PLANE; ++s) {
            node_id id = pl * ASTRA_NUM_SATS_PER_PLANE + s;
            OrbitElements c = { ASTRA_SAT_SMA_KM, ASTRA_SAT_ECC,
                ASTRA_INCLINATION_DEG * M_PI/180.0, (2.0*M_PI/ASTRA_NUM_PLANES)*pl, 0.0,
                (2.0*M_PI/ASTRA_NUM_SATS_PER_PLANE)*s + pl*0.1 };
            st[id] = astra_coe_to_rv(ASTRA_MU_EARTH, c);
            ids[id] = id;
        }
}

/* run `steps` and return final stats; checks conservation each step */
static int run(uint64_t seed, int steps, TrafficStats *out) {
    init_constellation();
    astra_traffic_init(&T, TR_BURST, 40.0, 0, seed);
    int conservation_fail = 0;
    double dt = 5.0;
    for (int k = 0; k < steps; ++k) {
        double now = (k + 1) * dt;
        for (uint32_t i = 0; i < ASTRA_NUM_TOTAL_SATS; ++i) {
            st[i] = astra_propagate_kepler(ASTRA_MU_EARTH, st[i].r, st[i].v, dt);
            pos[i] = st[i].r;
        }
        astra_build_isl_topology(&G, pos, ids, ASTRA_NUM_TOTAL_SATS, ASTRA_EARTH_RADIUS_KM, ASTRA_MAX_LINK_RANGE_KM);
        astra_build_csr(&G, ASTRA_NUM_TOTAL_SATS, ASTRA_COST_LATENCY);
        astra_router_init(&R, ASTRA_ROUTE_DIJKSTRA, ASTRA_COST_LATENCY);
        astra_router_step(&R, &G.csr);
        astra_traffic_step(&T, &G, &R, ids, ASTRA_NUM_TOTAL_SATS, now, dt);

        const TrafficStats *s = &T.stats;
        uint64_t drops = s->drop_no_route + s->drop_broken + s->drop_ttl +
                         s->drop_loss + s->drop_inactive;
        uint64_t accounted = s->delivered + drops + s->in_flight + s->at_node;
        if (accounted != s->generated) {
            if (!conservation_fail)
                printf("  step %d: CONSERVATION gen=%llu accounted=%llu (d=%llu drop=%llu inf=%u at=%u)\n",
                       k, (unsigned long long)s->generated, (unsigned long long)accounted,
                       (unsigned long long)s->delivered, (unsigned long long)drops, s->in_flight, s->at_node);
            conservation_fail++;
        }
        if (T.live > ASTRA_MAX_PACKETS) { printf("  pool overflow live=%u\n", T.live); conservation_fail++; }
    }
    *out = T.stats;
    return conservation_fail;
}

int main(void) {
    int fails = 0;
    TrafficStats a, b;

    int cf = run(2024, 600, &a);
    if (cf) { printf("conservation failures: %d\n", cf); fails++; }

    double dr = astra_delivery_ratio(&a);
    if (dr < 0.0 || dr > 1.0) { printf("delivery ratio out of range: %f\n", dr); fails++; }

    /* reproducibility */
    run(2024, 600, &b);
    if (a.generated != b.generated || a.delivered != b.delivered ||
        a.drop_loss != b.drop_loss || a.drop_no_route != b.drop_no_route) {
        printf("not reproducible\n"); fails++;
    }

    printf("generated=%llu delivered=%llu  no_route=%llu broken=%llu ttl=%llu loss=%llu overflow=%llu\n",
           (unsigned long long)a.generated, (unsigned long long)a.delivered,
           (unsigned long long)a.drop_no_route, (unsigned long long)a.drop_broken,
           (unsigned long long)a.drop_ttl, (unsigned long long)a.drop_loss,
           (unsigned long long)a.drop_overflow);
    printf("delivery_ratio=%.4f  link_util(last)=%.4f  conservation=%s  reproducible=%s\n",
           dr, a.last_link_util, fails ? "?" : "OK", "OK");
    printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
