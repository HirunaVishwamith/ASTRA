/* astra_sla — service-tier / SLA / user-terminal demand report.
 *
 * Models the commercial layer on top of the live constellation:
 *   supply  = min(space user-access, ground backhaul), both RF-budget derived
 *             - space   = connected sats * beams/sat * Ku user-beam Gbps
 *             - ground  = gateways      * beams/gw  * Ka  beam     Gbps
 *   demand  = busy-hour offered load from the service-tier subscriber mix
 *   scheduler allocates supply across tiers by strict priority, then we report
 *   SLA attainment, oversubscription, revenue, and revenue-at-risk.
 *
 * The sim's default shell models only a handful of gateways, so by default the
 * ground segment binds (a real, blunt finding: "provision gateways"). Use
 * --gateways N to size a realistic ground segment and see the priority
 * scheduler differentiate tiers. Read-only; never alters sim numerics.
 *
 * Usage: astra_sla [--steps N] [--planes P] [--per M] [--range KM] [--seed S]
 *                  [--gateways N] [--demand X] [--j2]
 */
#include "astra/sim.h"
#include "astra/rf.h"
#include "astra/sla.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BEAMS_PER_SAT 16    /* user-downlink spot beams per satellite */
#define BEAMS_PER_GW   8    /* Ka feeder beams per gateway            */

static SimState SIM;

int main(int argc, char **argv) {
    uint32_t steps = 1200, planes = ASTRA_NUM_PLANES, per = ASTRA_NUM_SATS_PER_PLANE;
    double range = 0.0, demand_scale = 1.0;
    uint64_t seed = 0xA57121u;
    int gateways = -1, j2 = 0;
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i],"--steps")    && i+1<argc) steps  = (uint32_t)strtoul(argv[++i],0,10);
        else if (!strcmp(argv[i],"--planes")   && i+1<argc) planes = (uint32_t)strtoul(argv[++i],0,10);
        else if (!strcmp(argv[i],"--per")      && i+1<argc) per    = (uint32_t)strtoul(argv[++i],0,10);
        else if (!strcmp(argv[i],"--range")    && i+1<argc) range  = strtod(argv[++i],0);
        else if (!strcmp(argv[i],"--seed")     && i+1<argc) seed   = strtoull(argv[++i],0,0);
        else if (!strcmp(argv[i],"--gateways") && i+1<argc) gateways = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--demand")   && i+1<argc) demand_scale = strtod(argv[++i],0);
        else if (!strcmp(argv[i],"--j2"))                   j2 = 1;
        else { fprintf(stderr,"unknown arg: %s\n", argv[i]); return 2; }
    }

    astra_sim_init_cfg(&SIM, seed, planes, per, range);
    SIM.j2_enabled = j2;
    if (gateways < 0) gateways = (int)SIM.ngs;

    /* steady-state average of connected satellites */
    double sum_connected = 0; uint32_t samples = 0, warmup = steps/2;
    static uint8_t deg[ASTRA_MAX_NODES];
    for (uint32_t step = 0; step < steps; ++step) {
        astra_sim_tick(&SIM);
        if (step < warmup) continue;
        memset(deg, 0, SIM.node_count);
        for (uint32_t e = 0; e < SIM.graph.link_count; ++e) {
            const Link *L = &SIM.graph.links[e];
            if (L->up) { deg[L->a]++; deg[L->b]++; }
        }
        uint32_t connected = 0;
        for (uint32_t i = 0; i < SIM.num_sats; ++i)
            if (SIM.field.alive[i] && deg[i] > 0) connected++;
        sum_connected += connected; samples++;
    }
    if (!samples) samples = 1;
    double connected = sum_connected / samples;

    /* RF-derived per-beam rates -> segment supplies (Gbps) */
    double ku = astra_rf_budget(&ASTRA_RF_KU_USER_DOWN, 700.0).rate_gbps;
    double ka = astra_rf_budget(&ASTRA_RF_KA_GW_UP,     900.0).rate_gbps;
    double space_gbps  = connected * BEAMS_PER_SAT * ku;
    double ground_gbps = (double)gateways * BEAMS_PER_GW * ka;
    int    ground_binds = ground_gbps < space_gbps;
    double supply = ground_binds ? ground_gbps : space_gbps;

    TierResult res[ASTRA_SLA_MAX_TIERS];
    astra_sla_allocate(ASTRA_SLA_TIERS, ASTRA_SLA_NTIERS, supply, demand_scale, res);

    double off_tot = 0, srv_tot = 0, rev_tot = 0, rev_risk = 0;
    for (uint32_t i = 0; i < ASTRA_SLA_NTIERS; ++i) {
        off_tot += res[i].offered_gbps; srv_tot += res[i].served_gbps;
        rev_tot += res[i].revenue_usd_mo;
        if (!res[i].sla_met) rev_risk += res[i].revenue_usd_mo;
    }

    printf("================= ASTRA SLA & demand report =================\n");
    printf("constellation : %u sats (%ux%u)  range %.0f km  ~%.0f connected\n",
           SIM.num_sats, planes, per, SIM.max_range, connected);
    printf("supply model  : space %.0f Gbps (%.0f sats x %d beams x %.2f Gbps Ku)\n",
           space_gbps, connected, BEAMS_PER_SAT, ku);
    printf("                ground %.0f Gbps (%d gw x %d beams x %.2f Gbps Ka)\n",
           ground_gbps, gateways, BEAMS_PER_GW, ka);
    printf("deliverable   : %.0f Gbps   [binding: %s]\n",
           supply, ground_binds ? "GROUND backhaul" : "SPACE access");
    printf("demand scale  : %.2f (1.0 = busy hour)\n\n", demand_scale);

    printf("  %-15s %8s %8s %8s %7s  %-4s %10s\n",
           "tier", "subs", "off Gbps", "srv Gbps", "attain", "SLA", "$/mo");
    for (uint32_t i = 0; i < ASTRA_SLA_NTIERS; ++i) {
        printf("  %-15s %8u %8.0f %8.0f %6.0f%%  %-4s $%8.1fM\n",
               ASTRA_SLA_TIERS[i].name, ASTRA_SLA_TIERS[i].subscribers,
               res[i].offered_gbps, res[i].served_gbps, 100.0*res[i].attainment,
               res[i].sla_met ? "OK" : "MISS", res[i].revenue_usd_mo/1e6);
    }

    printf("\n  offered %.0f Gbps / served %.0f Gbps  ->  oversubscription %.1fx\n",
           off_tot, srv_tot, supply > 1e-6 ? off_tot/supply : 0.0);
    printf("  monthly revenue   : $%.1f M\n", rev_tot/1e6);
    printf("  revenue at risk   : $%.1f M  (%.0f%% of book, tiers below SLA)\n",
           rev_risk/1e6, rev_tot > 0 ? 100.0*rev_risk/rev_tot : 0.0);
    if (ground_binds)
        printf("  verdict: GROUND-LIMITED — provision more gateways (try --gateways 150).\n");
    else if (off_tot > supply)
        printf("  verdict: capacity-limited at the busy hour — best-effort tiers shed first.\n");
    else
        printf("  verdict: demand fully served with margin.\n");
    printf("=============================================================\n");
    return 0;
}
