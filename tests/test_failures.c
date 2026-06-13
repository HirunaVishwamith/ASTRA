/* test_failures.c — invariant + reproducibility checks for the impairment
 * engine (RNG-driven, so not bit-compared to Python). */
#include "astra/failures.h"
#include "astra/graph.h"
#include "astra/orbit.h"
#include "astra/config.h"
#include <stdio.h>
#include <math.h>

static NetworkGraph G;
static vec3 pos[ASTRA_MAX_NODES];
static node_id ids[ASTRA_NUM_TOTAL_SATS];

static void build_constellation(void) {
    uint32_t n = 0;
    for (uint32_t pl = 0; pl < ASTRA_NUM_PLANES; ++pl)
        for (uint32_t s = 0; s < ASTRA_NUM_SATS_PER_PLANE; ++s) {
            node_id id = pl * ASTRA_NUM_SATS_PER_PLANE + s;
            OrbitElements c = {
                ASTRA_SAT_SMA_KM, ASTRA_SAT_ECC, ASTRA_INCLINATION_DEG * M_PI / 180.0,
                (2.0*M_PI/ASTRA_NUM_PLANES) * pl, 0.0,
                (2.0*M_PI/ASTRA_NUM_SATS_PER_PLANE) * s + pl * 0.1 };
            StateRV st = astra_coe_to_rv(ASTRA_MU_EARTH, c);
            pos[id] = st.r; ids[n++] = id;
        }
}

static uint32_t count_down(void) {
    uint32_t d = 0;
    for (uint32_t e = 0; e < G.link_count; ++e) if (!G.links[e].up) d++;
    return d;
}

int main(void) {
    build_constellation();
    int fails = 0;

    /* 1) No-op impairment: nothing drops, latency/loss unchanged. */
    astra_build_isl_topology(&G, pos, ids, ASTRA_NUM_TOTAL_SATS, ASTRA_EARTH_RADIUS_KM, ASTRA_MAX_LINK_RANGE_KM);
    uint32_t nlinks = G.link_count;
    double lat0 = G.links[0].props.latency_s, loss0 = G.links[0].props.loss_prob;
    LinkImpairments none = astra_impair_none();
    Rng rng; rng_seed(&rng, 12345, 1);
    astra_failures_apply(&G, &none, 5.0, &rng);
    if (count_down() != 0) { printf("no-op dropped links\n"); fails++; }
    if (G.links[0].props.latency_s != lat0 || G.links[0].props.loss_prob != (float)loss0) {
        printf("no-op changed props\n"); fails++; }

    /* 2) Blackout statistics: fraction down ~ p_black over many trials. */
    LinkImpairments bl = {0.5, 0.0, 0.0, 1.0};
    double p_black = 1.0 - pow(1.0 - 0.5, 1.0);  /* dt=1 */
    long down = 0, total = 0;
    rng_seed(&rng, 999, 1);
    for (int t = 0; t < 300; ++t) {
        astra_build_isl_topology(&G, pos, ids, ASTRA_NUM_TOTAL_SATS, ASTRA_EARTH_RADIUS_KM, ASTRA_MAX_LINK_RANGE_KM);
        astra_failures_apply(&G, &bl, 1.0, &rng);
        down += count_down(); total += G.link_count;
    }
    double frac = (double)down / (double)total;
    if (fabs(frac - p_black) > 0.02) { printf("blackout frac %.4f != %.4f\n", frac, p_black); fails++; }

    /* 3) Loss multiplier scales + clamps. */
    astra_build_isl_topology(&G, pos, ids, ASTRA_NUM_TOTAL_SATS, ASTRA_EARTH_RADIUS_KM, ASTRA_MAX_LINK_RANGE_KM);
    double pre = G.links[0].props.loss_prob;
    LinkImpairments lm = {0.0, 0.0, 0.0, 3.0};
    rng_seed(&rng, 7, 1);
    astra_failures_apply(&G, &lm, 1.0, &rng);
    double expect = pre * 3.0; if (expect > 1.0) expect = 1.0;
    if (fabs((double)G.links[0].props.loss_prob - expect) > 1e-6) {
        printf("loss mult wrong %.6f != %.6f\n", (double)G.links[0].props.loss_prob, expect); fails++; }

    /* 4) Reproducibility: same seed -> identical down-set. */
    static uint8_t a[ASTRA_MAX_LINKS], b[ASTRA_MAX_LINKS];
    LinkImpairments bl2 = {0.3, 0.1, 0.05, 1.0};
    astra_build_isl_topology(&G, pos, ids, ASTRA_NUM_TOTAL_SATS, ASTRA_EARTH_RADIUS_KM, ASTRA_MAX_LINK_RANGE_KM);
    rng_seed(&rng, 42, 7); astra_failures_apply(&G, &bl2, 2.0, &rng);
    for (uint32_t e = 0; e < G.link_count; ++e) a[e] = G.links[e].up;
    astra_build_isl_topology(&G, pos, ids, ASTRA_NUM_TOTAL_SATS, ASTRA_EARTH_RADIUS_KM, ASTRA_MAX_LINK_RANGE_KM);
    rng_seed(&rng, 42, 7); astra_failures_apply(&G, &bl2, 2.0, &rng);
    for (uint32_t e = 0; e < G.link_count; ++e) b[e] = G.links[e].up;
    int repro = 1;
    for (uint32_t e = 0; e < G.link_count; ++e) if (a[e] != b[e]) repro = 0;
    if (!repro) { printf("not reproducible with same seed\n"); fails++; }

    printf("links=%u  blackout frac=%.4f (expect %.4f)  reproducible=%d\n",
           nlinks, frac, p_black, repro);
    printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
