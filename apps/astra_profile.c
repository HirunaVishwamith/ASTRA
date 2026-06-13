/* astra_profile.c — headless end-to-end runner + performance profiler.
 *
 * Drives the full simulation pipeline (no graphics) single-threaded for a fixed
 * number of steps, with per-stage timing enabled, and reports:
 *   - wall time, steps/s, and real-time factor (sim seconds per wall second)
 *   - per-stage breakdown (where the tick budget goes)
 *   - final network metrics (delivery ratio, delay, hops, churn)
 *   - static memory footprint
 *
 * Usage:
 *   astra_profile [--steps N] [--planes P] [--per M] [--range KM] [--seed S]
 *   astra_profile --sweep [--steps N]      # scaling sweep over shell sizes
 */
#include "astra/sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* one configuration -> static SimState is large, keep it off the stack */
static SimState SIM;

static void run_one(uint64_t seed, uint32_t planes, uint32_t per_plane,
                    double range_km, uint32_t steps, int verbose) {
    astra_sim_init_cfg(&SIM, seed, planes, per_plane, range_km);
    SIM.prof.enabled = 1;

    /* warm up a few ticks so topology/routing tables are populated and the
     * branch predictor / caches are hot before we start the clock */
    for (int w = 0; w < 5; ++w) astra_sim_tick(&SIM);
    memset(&SIM.prof.ns, 0, sizeof(SIM.prof.ns));
    SIM.prof.samples = 0;

    uint64_t t0 = now_ns();
    for (uint32_t i = 0; i < steps; ++i) astra_sim_tick(&SIM);
    uint64_t t1 = now_ns();

    double wall_s   = (double)(t1 - t0) / 1e9;
    double per_step_us = wall_s * 1e6 / (double)steps;
    double steps_per_s = (double)steps / wall_s;
    double sim_seconds = (double)steps * SIM.sim_dt_s;
    double rtf = sim_seconds / wall_s;          /* sim-s per wall-s */

    if (verbose) {
        printf("\n=== ASTRA headless profile ===\n");
        printf("shell: %u planes x %u = %u sats, %u ground, range %.0f km\n",
               planes, per_plane, SIM.num_sats, SIM.ngs, SIM.max_range);
        printf("steps: %u   dt: %.1fs\n", steps, SIM.sim_dt_s);
        printf("wall:  %.3f s   %.2f us/step   %.0f steps/s\n",
               wall_s, per_step_us, steps_per_s);
        printf("real-time factor: %.0fx  (%.1f sim-min per wall-s)\n",
               rtf, rtf / 60.0);
        printf("budget vs 16ms GUI tick: %.2f%% used\n", per_step_us / 16000.0 * 100.0);

        printf("\nper-stage (mean over %llu steps):\n",
               (unsigned long long)SIM.prof.samples);
        uint64_t total = 0;
        for (int k = 0; k < PF_NSTAGES; ++k) total += SIM.prof.ns[k];
        for (int k = 0; k < PF_NSTAGES; ++k) {
            double mean_us = (double)SIM.prof.ns[k] / (double)SIM.prof.samples / 1000.0;
            double pct = total ? 100.0 * (double)SIM.prof.ns[k] / (double)total : 0.0;
            printf("  %-10s %8.3f us  %5.1f%%  ", astra_prof_stage_name[k], mean_us, pct);
            int bars = (int)(pct / 2.0 + 0.5);
            for (int b = 0; b < bars; ++b) putchar('#');
            putchar('\n');
        }

        const StepMetrics *m = &SIM.last_metrics;
        printf("\nfinal metrics:\n");
        printf("  delivery ratio : %.3f\n", m->delivery_ratio);
        printf("  avg delay      : %.4f s\n", m->avg_delay_s);
        printf("  avg hops       : %.2f\n", m->avg_hops);
        printf("  avg path len   : %.2f\n", m->avg_path_len_hops);
        printf("  links          : %u\n", SIM.graph.link_count);
        printf("  route updates  : %u\n", m->route_updates);
        printf("\nmemory: SimState=%.1f MB  snapshot=%.1f KB (x3 buffers)\n",
               sizeof(SimState) / 1048576.0, sizeof(RenderSnapshot) / 1024.0);
    } else {
        /* sweep row: sats, links, us/step, steps/s, rtf, delivery ratio */
        printf("%5u  %6u  %8.2f  %9.0f  %9.0f  %7.3f\n",
               SIM.num_sats, SIM.graph.link_count, per_step_us,
               steps_per_s, rtf, SIM.last_metrics.delivery_ratio);
    }
}

int main(int argc, char **argv) {
    uint32_t steps = 1000, planes = ASTRA_NUM_PLANES, per = ASTRA_NUM_SATS_PER_PLANE;
    double range = 0.0;
    uint64_t seed = 0xA57121u;
    int sweep = 0;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--steps")  && i+1 < argc) steps  = (uint32_t)strtoul(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--planes") && i+1 < argc) planes = (uint32_t)strtoul(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--per")    && i+1 < argc) per    = (uint32_t)strtoul(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--range")  && i+1 < argc) range  = strtod(argv[++i], 0);
        else if (!strcmp(argv[i], "--seed")   && i+1 < argc) seed   = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--sweep")) sweep = 1;
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }

    if (sweep) {
        printf("scaling sweep (%u steps each, range %.0f km)\n",
               steps, range > 0 ? range : (double)ASTRA_MAX_LINK_RANGE_KM);
        printf(" sats   links   us/step   steps/s       rtf    deliv\n");
        const uint32_t shells[][2] = { {10,10}, {12,12}, {18,18}, {24,24}, {32,32} };
        for (size_t k = 0; k < sizeof(shells)/sizeof(shells[0]); ++k)
            run_one(seed, shells[k][0], shells[k][1], range, steps, 0);
    } else {
        run_one(seed, planes, per, range, steps, 1);
    }
    return 0;
}
