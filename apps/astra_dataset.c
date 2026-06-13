/* astra_dataset.c — network-behavior profile generator.
 *
 * Runs the full simulation headless and emits a per-step CSV time series of the
 * network metrics, plus a JSON run summary. Supports scripted disturbances —
 * deterministic satellite strikes, a reboot, and a stochastic link-blackout
 * rate — so the dataset captures failure response and routing convergence, not
 * just steady state. This is the data backing the "network-behavior" profile.
 *
 * Usage:
 *   astra_dataset [--steps N] [--planes P] [--per M] [--range KM] [--seed S]
 *                 [--blackout PER_S] [--strike STEP:SAT]... [--reboot STEP]
 *                 [--csv FILE]   (default stdout) [--json FILE]
 */
#include "astra/sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SimState SIM;

#define MAX_STRIKES 64
static uint32_t strike_step[MAX_STRIKES], strike_sat[MAX_STRIKES];
static uint32_t n_strikes = 0;

int main(int argc, char **argv) {
    uint32_t steps = 1200, planes = ASTRA_NUM_PLANES, per = ASTRA_NUM_SATS_PER_PLANE;
    double range = 0.0, blackout = 0.0;
    uint64_t seed = 0xA57121u;
    uint32_t reboot_step = 0xFFFFFFFFu;
    const char *csv_path = NULL, *json_path = NULL;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--steps")    && i+1 < argc) steps  = (uint32_t)strtoul(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--planes")   && i+1 < argc) planes = (uint32_t)strtoul(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--per")      && i+1 < argc) per    = (uint32_t)strtoul(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--range")    && i+1 < argc) range  = strtod(argv[++i], 0);
        else if (!strcmp(argv[i], "--seed")     && i+1 < argc) seed   = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--blackout") && i+1 < argc) blackout = strtod(argv[++i], 0);
        else if (!strcmp(argv[i], "--reboot")   && i+1 < argc) reboot_step = (uint32_t)strtoul(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "--csv")      && i+1 < argc) csv_path = argv[++i];
        else if (!strcmp(argv[i], "--json")     && i+1 < argc) json_path = argv[++i];
        else if (!strcmp(argv[i], "--strike")   && i+1 < argc) {
            char *colon = strchr(argv[++i], ':');
            if (!colon || n_strikes >= MAX_STRIKES) { fprintf(stderr, "bad --strike\n"); return 2; }
            *colon = 0;
            strike_step[n_strikes] = (uint32_t)strtoul(argv[i], 0, 10);
            strike_sat[n_strikes]  = (uint32_t)strtoul(colon+1, 0, 10);
            n_strikes++;
        }
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }

    astra_sim_init_cfg(&SIM, seed, planes, per, range);
    SIM.impair.blackout_prob_per_s = blackout;

    FILE *csv = csv_path ? fopen(csv_path, "w") : stdout;
    if (!csv) { fprintf(stderr, "cannot open %s\n", csv_path); return 1; }
    fprintf(csv, "step,sim_time_s,active_nodes,links,delivery_ratio,avg_delay_s,"
                 "avg_hops,avg_path_len,route_updates,convergence_s,avg_link_util,"
                 "generated,delivered,drop_no_route,drop_loss,drop_broken,in_flight\n");

    /* track aggregate stats for the JSON summary */
    double sum_deliv = 0.0, min_deliv = 1e9, max_delay = 0.0;
    double worst_conv = -1.0; uint64_t conv_events = 0;
    uint32_t max_links = 0;

    for (uint32_t step = 0; step < steps; ++step) {
        /* scripted disturbances applied via the real command ring */
        for (uint32_t k = 0; k < n_strikes; ++k)
            if (strike_step[k] == step) {
                Command c = { CMD_STRIKE, strike_sat[k], 0.0 };
                astra_cmd_push(&SIM, c);
            }
        if (step == reboot_step) {
            Command c = { CMD_REBOOT_ALL, 0, 0.0 };
            astra_cmd_push(&SIM, c);
        }

        astra_sim_tick(&SIM);

        const StepMetrics *m = &SIM.last_metrics;
        const TrafficStats *ts = &SIM.traffic.stats;
        fprintf(csv,
            "%u,%.1f,%u,%u,%.5f,%.5f,%.3f,%.3f,%u,%.2f,%.5f,"
            "%llu,%llu,%llu,%llu,%llu,%u\n",
            step, SIM.sim_time_s, SIM.active_count, SIM.graph.link_count,
            m->delivery_ratio, m->avg_delay_s, m->avg_hops, m->avg_path_len_hops,
            m->route_updates, m->convergence_s, m->avg_link_util,
            (unsigned long long)ts->generated, (unsigned long long)ts->delivered,
            (unsigned long long)ts->drop_no_route, (unsigned long long)ts->drop_loss,
            (unsigned long long)ts->drop_broken, ts->in_flight);

        sum_deliv += m->delivery_ratio;
        if (m->delivery_ratio < min_deliv) min_deliv = m->delivery_ratio;
        if (m->avg_delay_s > max_delay) max_delay = m->avg_delay_s;
        if (m->convergence_s >= 0.0) { worst_conv = m->convergence_s > worst_conv ? m->convergence_s : worst_conv; conv_events++; }
        if (SIM.graph.link_count > max_links) max_links = SIM.graph.link_count;
    }
    if (csv != stdout) fclose(csv);

    /* JSON run summary */
    FILE *js = json_path ? fopen(json_path, "w") : stderr;
    if (js) {
        fprintf(js,
            "{\n"
            "  \"sats\": %u, \"ground\": %u, \"range_km\": %.0f, \"steps\": %u,\n"
            "  \"dt_s\": %.1f, \"blackout_per_s\": %.4f, \"strikes\": %u,\n"
            "  \"mean_delivery_ratio\": %.4f,\n"
            "  \"min_delivery_ratio\": %.4f,\n"
            "  \"max_avg_delay_s\": %.4f,\n"
            "  \"max_links\": %u,\n"
            "  \"convergence_events\": %llu,\n"
            "  \"worst_convergence_s\": %.2f,\n"
            "  \"total_generated\": %llu,\n"
            "  \"total_delivered\": %llu\n"
            "}\n",
            SIM.num_sats, SIM.ngs, SIM.max_range, steps, SIM.sim_dt_s,
            blackout, n_strikes, sum_deliv / (double)steps, min_deliv, max_delay,
            max_links, (unsigned long long)conv_events, worst_conv,
            (unsigned long long)SIM.traffic.stats.generated,
            (unsigned long long)SIM.traffic.stats.delivered);
        if (js != stderr) fclose(js);
    }
    return 0;
}
