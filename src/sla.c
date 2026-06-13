/* sla.c — strict-priority SLA scheduler + default tier mix (see sla.h). */
#include "astra/sla.h"
#include <stddef.h>

void astra_sla_allocate(const ServiceTier *t, uint32_t n,
                        double supply_gbps, double demand_scale, TierResult *out) {
    if (n > ASTRA_SLA_MAX_TIERS) n = ASTRA_SLA_MAX_TIERS;

    for (uint32_t i = 0; i < n; ++i) {
        double active = (double)t[i].subscribers * t[i].activity;
        out[i].offered_gbps   = active * t[i].burst_mbps * demand_scale / 1000.0;
        out[i].committed_gbps = active * t[i].cir_mbps / 1000.0;
        out[i].revenue_usd_mo = (double)t[i].subscribers * t[i].price_usd_mo;
        out[i].served_gbps = 0.0;
        out[i].attainment  = 0.0;
        out[i].sla_met     = 0;
    }

    /* priority-ordered indices (0 highest); stable insertion sort */
    uint32_t ord[ASTRA_SLA_MAX_TIERS];
    for (uint32_t i = 0; i < n; ++i) ord[i] = i;
    for (uint32_t i = 1; i < n; ++i) {
        uint32_t k = ord[i], j = i;
        while (j > 0 && t[ord[j-1]].priority > t[k].priority) { ord[j] = ord[j-1]; --j; }
        ord[j] = k;
    }

    double rem = supply_gbps < 0.0 ? 0.0 : supply_gbps;
    for (uint32_t p = 0; p < n; ++p) {
        uint32_t i = ord[p];
        double grant = out[i].offered_gbps;
        if (grant > rem) grant = rem;
        out[i].served_gbps = grant;
        rem -= grant;
        out[i].attainment = out[i].offered_gbps > 1e-12 ? grant / out[i].offered_gbps : 1.0;
        out[i].sla_met    = (out[i].served_gbps + 1e-9 >= out[i].committed_gbps);
    }
}

/* Representative tier mix (illustrative figures, not official specs). Priority:
 * 0 = enterprise/gov (highest), 3 = residential best-effort (lowest). */
const ServiceTier ASTRA_SLA_TIERS[] = {
    { "Enterprise/Gov", 4000,    100.0, 400.0, 0.25, 0, 5000.0 },
    { "Priority/Bus.",  80000,    40.0, 200.0, 0.08, 1,  250.0 },
    { "Mobility",       30000,    50.0, 200.0, 0.15, 2, 1000.0 },
    { "Residential",    1500000,  20.0,  80.0, 0.03, 3,  120.0 },
};
const uint32_t ASTRA_SLA_NTIERS = sizeof(ASTRA_SLA_TIERS)/sizeof(ASTRA_SLA_TIERS[0]);
