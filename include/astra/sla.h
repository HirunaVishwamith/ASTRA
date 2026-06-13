/* sla.h — service tiers, user-terminal demand, and a priority SLA scheduler.
 *
 * The commercial layer: subscribers buy service tiers (committed rate, burst,
 * priority, price). At the busy hour they offer aggregate demand to the
 * network; a strict-priority scheduler allocates the constellation's
 * deliverable capacity across tiers, and we measure SLA attainment, revenue,
 * and revenue-at-risk. Pure allocation math — no dependency on the sim or rf;
 * the report tool feeds it a supply figure derived from the live constellation.
 *
 * Additive analysis layer: it never touches the parity-verified sim numerics. */
#ifndef ASTRA_SLA_H
#define ASTRA_SLA_H

#include <stdint.h>

#define ASTRA_SLA_MAX_TIERS 16u
#define ASTRA_SLA_TARGET    0.95   /* served/committed for the SLA to be "met" */

typedef struct {
    const char *name;
    uint32_t    subscribers;   /* provisioned user terminals in this tier   */
    double      cir_mbps;      /* committed information rate per active UT   */
    double      burst_mbps;    /* busy-hour demand pull per active UT        */
    double      activity;      /* busy-hour concurrency factor [0..1]        */
    int         priority;      /* 0 = highest scheduling priority            */
    double      price_usd_mo;  /* monthly price per UT                       */
} ServiceTier;

typedef struct {
    double offered_gbps;       /* busy-hour offered load (scaled)            */
    double committed_gbps;     /* SLA-committed floor for active UTs         */
    double served_gbps;        /* capacity actually allocated                */
    double attainment;         /* served / offered  [0..1]                   */
    double revenue_usd_mo;     /* subscribers * price                        */
    int    sla_met;            /* served >= committed_gbps                   */
} TierResult;

/* Allocate `supply_gbps` of deliverable capacity across n tiers by strict
 * priority (0 first), filling each tier's offered demand greedily. demand_scale
 * scales the offered load (e.g. a diurnal factor; 1.0 = busy hour). out[i]
 * aligns with tiers[i]. */
void astra_sla_allocate(const ServiceTier *tiers, uint32_t n,
                        double supply_gbps, double demand_scale, TierResult *out);

/* Representative consumer/enterprise tier mix (illustrative, not real specs). */
extern const ServiceTier ASTRA_SLA_TIERS[];
extern const uint32_t    ASTRA_SLA_NTIERS;

#endif /* ASTRA_SLA_H */
