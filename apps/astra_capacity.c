/* astra_capacity — constellation capacity & coverage KPI report.
 *
 * Runs the simulation headless and, using the physical RF/optical link-budget
 * engine (src/rf.c), turns the live topology into the numbers a constellation
 * operator actually sells and reports on:
 *   - deliverable network capacity (Tbps), split optical-ISL vs ground-RF
 *   - coverage: connected satellites, active gateways, ground reach
 *   - RF health: links the budget puts in outage, mean link margin
 *   - a first-order economic view ($/Gbps of delivered capacity)
 *
 * Read-only analysis: it never alters the parity-verified sim numerics; it just
 * reads SIM.graph after each tick and prices every link with rf.c. KPIs are
 * averaged over the steady-state second half of the run.
 *
 * Usage: astra_capacity [--steps N] [--planes P] [--per M] [--range KM] [--seed S]
 */
#include "astra/sim.h"
#include "astra/rf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Illustrative unit economics (NOT real figures) for the $/Gbps headline. */
#define SAT_CAPEX_USD     500000.0
#define GATEWAY_CAPEX_USD 1500000.0

static SimState SIM;

int main(int argc, char **argv) {
    uint32_t steps = 1200, planes = ASTRA_NUM_PLANES, per = ASTRA_NUM_SATS_PER_PLANE;
    double range = 0.0;
    uint64_t seed = 0xA57121u;
    int j2 = 0;
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--steps")  && i+1 < argc) steps  = (uint32_t)strtoul(argv[++i],0,10);
        else if (!strcmp(argv[i], "--planes") && i+1 < argc) planes = (uint32_t)strtoul(argv[++i],0,10);
        else if (!strcmp(argv[i], "--per")    && i+1 < argc) per    = (uint32_t)strtoul(argv[++i],0,10);
        else if (!strcmp(argv[i], "--range")  && i+1 < argc) range  = strtod(argv[++i],0);
        else if (!strcmp(argv[i], "--seed")   && i+1 < argc) seed   = strtoull(argv[++i],0,0);
        else if (!strcmp(argv[i], "--j2"))                   j2 = 1;
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }

    astra_sim_init_cfg(&SIM, seed, planes, per, range);
    SIM.j2_enabled = j2;

    /* steady-state accumulators (averaged over the second half of the run) */
    double sum_isl_gbps = 0, sum_gnd_gbps = 0;
    double sum_isl_links = 0, sum_gnd_links = 0;
    double sum_isl_outage = 0, sum_gnd_outage = 0;
    double sum_margin = 0, sum_margin_n = 0;
    double sum_connected = 0, sum_gateways = 0, sum_ground_reach = 0;
    uint32_t samples = 0;
    uint32_t warmup = steps / 2;

    static uint8_t deg[ASTRA_MAX_NODES];
    static uint8_t has_gnd[ASTRA_MAX_NODES];   /* sat has a direct ground link */

    for (uint32_t step = 0; step < steps; ++step) {
        astra_sim_tick(&SIM);
        if (step < warmup) continue;

        memset(deg, 0, SIM.node_count);
        memset(has_gnd, 0, SIM.node_count);
        double isl_gbps = 0, gnd_gbps = 0;
        uint32_t isl_links = 0, gnd_links = 0, isl_outage = 0, gnd_outage = 0;
        double margin_acc = 0; uint32_t margin_n = 0;

        for (uint32_t e = 0; e < SIM.graph.link_count; ++e) {
            const Link *L = &SIM.graph.links[e];
            if (!L->up) continue;
            int ground = (L->a >= SIM.num_sats) || (L->b >= SIM.num_sats);
            deg[L->a]++; deg[L->b]++;
            if (ground) {
                RfLink r = astra_rf_budget(&ASTRA_RF_KA_GW_UP, L->distance_km);
                gnd_gbps += r.rate_gbps; gnd_links++;
                if (!r.closed) gnd_outage++;
                margin_acc += r.margin_db; margin_n++;
                if (L->a < SIM.num_sats) has_gnd[L->a] = 1;
                if (L->b < SIM.num_sats) has_gnd[L->b] = 1;
            } else {
                OpticalLink o = astra_optical_budget(&ASTRA_OPTICAL_ISL, L->distance_km);
                isl_gbps += o.rate_gbps; isl_links++;
                if (!o.closed) isl_outage++;
                margin_acc += o.margin_db; margin_n++;
            }
        }

        uint32_t connected = 0, ground_reach = 0;
        for (uint32_t i = 0; i < SIM.num_sats; ++i) {
            if (!SIM.field.alive[i]) continue;
            if (deg[i] > 0) connected++;
            if (has_gnd[i]) ground_reach++;
        }
        uint32_t gateways = 0;
        for (uint32_t i = SIM.num_sats; i < SIM.node_count; ++i)
            if (deg[i] > 0) gateways++;

        sum_isl_gbps += isl_gbps; sum_gnd_gbps += gnd_gbps;
        sum_isl_links += isl_links; sum_gnd_links += gnd_links;
        sum_isl_outage += isl_outage; sum_gnd_outage += gnd_outage;
        if (margin_n) { sum_margin += margin_acc; sum_margin_n += margin_n; }
        sum_connected += connected; sum_gateways += gateways; sum_ground_reach += ground_reach;
        samples++;
    }

    if (!samples) samples = 1;
    double isl_gbps = sum_isl_gbps / samples;
    double gnd_gbps = sum_gnd_gbps / samples;
    double total_gbps = isl_gbps + gnd_gbps;
    double connected = sum_connected / samples;
    double gateways  = sum_gateways / samples;
    double ground_reach = sum_ground_reach / samples;
    double mean_margin = sum_margin_n ? sum_margin / sum_margin_n : 0.0;

    double capex = SIM.num_sats * SAT_CAPEX_USD + SIM.ngs * GATEWAY_CAPEX_USD;
    /* deliverable-to-ground capacity is the gateway (Ka) floor */
    double usd_per_gbps = gnd_gbps > 1e-6 ? capex / gnd_gbps : 0.0;

    printf("================ ASTRA capacity & coverage report ================\n");
    printf("constellation : %u sats (%ux%u)  %u gateways  range %.0f km\n",
           SIM.num_sats, planes, per, SIM.ngs, SIM.max_range);
    printf("averaged over : %u steady-state steps (after %u warmup)\n\n", samples, warmup);

    printf("-- capacity (achievable, link-budget priced) --\n");
    printf("  optical ISL backbone : %8.2f Tbps   (%.0f links up)\n",
           isl_gbps/1000.0, sum_isl_links/samples);
    printf("  ground Ka access     : %8.2f Tbps   (%.0f links up)\n",
           gnd_gbps/1000.0, sum_gnd_links/samples);
    printf("  total switching cap. : %8.2f Tbps\n", total_gbps/1000.0);
    printf("  deliverable to ground: %8.2f Tbps   <- gateway-limited floor\n\n", gnd_gbps/1000.0);

    printf("-- coverage --\n");
    printf("  connected sats       : %8.1f / %u  (%.0f%%)\n",
           connected, SIM.num_sats, 100.0*connected/(SIM.num_sats?SIM.num_sats:1));
    printf("  sats with ground link: %8.1f  (direct gateway reach)\n", ground_reach);
    printf("  active gateways      : %8.1f / %u\n\n", gateways, SIM.ngs);

    printf("-- RF health --\n");
    printf("  mean link margin     : %8.2f dB\n", mean_margin);
    printf("  ISL links in outage  : %8.1f / %.0f\n", sum_isl_outage/samples, sum_isl_links/samples);
    printf("  ground links outage  : %8.1f / %.0f\n\n", sum_gnd_outage/samples, sum_gnd_links/samples);

    printf("-- first-order economics (illustrative unit costs) --\n");
    printf("  modeled capex        : $%.0f M  ($%.0fk/sat, $%.1fM/gateway)\n",
           capex/1e6, SAT_CAPEX_USD/1e3, GATEWAY_CAPEX_USD/1e6);
    printf("  capex per Gbps deliv.: $%.0f / Gbps\n", usd_per_gbps);
    printf("==================================================================\n");
    return 0;
}
