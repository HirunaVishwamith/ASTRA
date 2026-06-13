/* test_sla.c — invariant verification for the priority SLA scheduler.
 *   (1) conservation: sum(served) <= supply and served <= offered per tier.
 *   (2) strict priority: a lower-priority tier gets served only after every
 *       higher-priority tier is fully satisfied.
 *   (3) abundant supply: all tiers fully served, all SLA met.
 *   (4) zero supply: nothing served, every committed tier misses SLA.
 *   (5) monotonicity: more supply never reduces any tier's served rate.
 *   (6) purity: identical inputs -> identical outputs. */
#include "astra/sla.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void) {
    const ServiceTier *T = ASTRA_SLA_TIERS;
    uint32_t n = ASTRA_SLA_NTIERS;
    TierResult r[ASTRA_SLA_MAX_TIERS];

    /* offered total at busy hour, for picking a "scarce" supply */
    astra_sla_allocate(T, n, 1e12, 1.0, r);   /* huge supply -> all offered */
    double off_tot = 0;
    for (uint32_t i = 0; i < n; ++i) off_tot += r[i].offered_gbps;

    /* (3) abundant */
    for (uint32_t i = 0; i < n; ++i) {
        CHECK(fabs(r[i].served_gbps - r[i].offered_gbps) < 1e-6, "abundant: tier served != offered");
        CHECK(r[i].sla_met, "abundant: tier %u should meet SLA", i);
        CHECK(r[i].attainment > 0.999, "abundant: attainment ~1");
    }

    /* scarce: half the total offered */
    double supply = off_tot * 0.5;
    astra_sla_allocate(T, n, supply, 1.0, r);
    double srv_tot = 0;
    for (uint32_t i = 0; i < n; ++i) {
        srv_tot += r[i].served_gbps;
        CHECK(r[i].served_gbps <= r[i].offered_gbps + 1e-9, "served <= offered");  /* (1) */
        CHECK(r[i].served_gbps >= -1e-12, "served >= 0");
    }
    CHECK(srv_tot <= supply + 1e-6, "sum served <= supply");                        /* (1) */

    /* (2) strict priority: if a tier is partially/​un-served, every
     * strictly-higher-priority tier must be fully served. */
    for (uint32_t i = 0; i < n; ++i) {
        if (r[i].served_gbps + 1e-9 < r[i].offered_gbps) {
            for (uint32_t j = 0; j < n; ++j)
                if (T[j].priority < T[i].priority)
                    CHECK(r[j].served_gbps + 1e-6 >= r[j].offered_gbps,
                          "priority %d starved while lower-priority %d served",
                          T[j].priority, T[i].priority);
        }
    }

    /* (4) zero supply */
    astra_sla_allocate(T, n, 0.0, 1.0, r);
    for (uint32_t i = 0; i < n; ++i) {
        CHECK(r[i].served_gbps == 0.0, "zero supply: nothing served");
        CHECK(r[i].committed_gbps <= 0.0 || !r[i].sla_met, "zero supply: committed tier must miss SLA");
    }

    /* (5) monotonicity in supply */
    TierResult lo[ASTRA_SLA_MAX_TIERS], hi[ASTRA_SLA_MAX_TIERS];
    astra_sla_allocate(T, n, supply,        1.0, lo);
    astra_sla_allocate(T, n, supply * 1.25, 1.0, hi);
    for (uint32_t i = 0; i < n; ++i)
        CHECK(hi[i].served_gbps + 1e-9 >= lo[i].served_gbps, "more supply must not reduce served (tier %u)", i);

    /* (6) purity (compare fields, not padding bytes) */
    TierResult a[ASTRA_SLA_MAX_TIERS], b[ASTRA_SLA_MAX_TIERS];
    astra_sla_allocate(T, n, supply, 0.8, a);
    astra_sla_allocate(T, n, supply, 0.8, b);
    for (uint32_t i = 0; i < n; ++i)
        CHECK(a[i].served_gbps == b[i].served_gbps && a[i].attainment == b[i].attainment
              && a[i].offered_gbps == b[i].offered_gbps && a[i].sla_met == b[i].sla_met,
              "allocate must be a pure function (tier %u)", i);

    if (fails) { printf("FAIL: %d invariant(s) violated\n", fails); return 1; }
    printf("SLA scheduler: conservation/priority/abundance/outage/monotonic/purity OK\n");
    printf("PASS\n");
    return 0;
}
