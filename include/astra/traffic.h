/* traffic.h — discrete packet simulation over the dynamic graph + router.
 * Port of traffic/sim.py, redesigned around a fixed packet pool (zero per-step
 * allocation) with running cumulative counters (Python rescanned an
 * ever-growing dict; we purge delivered/dropped packets back to the pool, so
 * memory is bounded and the sim can run indefinitely). */
#ifndef ASTRA_TRAFFIC_H
#define ASTRA_TRAFFIC_H

#include "astra/graph.h"
#include "astra/routing.h"
#include "astra/rng.h"

typedef enum { TR_UNIFORM, TR_HOTSPOT, TR_BURST } TrafficPattern;

enum { PK_FREE = 0, PK_AT_NODE, PK_IN_TRANSIT };

typedef struct {
    node_id  src, dst, cur;
    pkt_id   next;            /* intrusive freelist link when PK_FREE */
    double   created_t;
    double   arrival_t;       /* valid when PK_IN_TRANSIT */
    node_id  transit_to;
    uint16_t hops;
    int16_t  ttl;
    uint16_t size_bytes;
    uint8_t  state;
} Packet;

typedef struct {
    /* cumulative over the whole run */
    uint64_t generated, delivered;
    uint64_t drop_no_route, drop_broken, drop_ttl, drop_loss, drop_inactive, drop_overflow;
    double   sum_delay_s, sum_hops;
    /* instantaneous */
    uint32_t in_flight, at_node;
    double   last_link_util;     /* mean utilisation of links used this step */
} TrafficStats;

typedef struct {
    TrafficPattern pattern;
    double   rate_pps;
    node_id  hotspot;
    int      burst_left;
} TrafficGen;

typedef struct {
    Packet      pool[ASTRA_MAX_PACKETS];
    pkt_id      free_head;
    uint32_t    live;
    TrafficGen  gen;
    TrafficStats stats;
    Rng         rng;
    /* per-directed-link byte budget scratch (rebuilt each step) */
    double      cap_used[ASTRA_MAX_LINKS * 2];
} TrafficSim;

void astra_traffic_init(TrafficSim *t, TrafficPattern pattern, double rate_pps,
                        node_id hotspot, uint64_t seed);

/* One discrete step. active_ids[]/n list routable nodes (sats+ground). */
void astra_traffic_step(TrafficSim *t, const NetworkGraph *g, const Router *r,
                        const node_id *active_ids, uint32_t n,
                        double now_t, double dt_s);

/* delivered / (delivered + dropped), or 0 if none. */
double astra_delivery_ratio(const TrafficStats *s);

#endif /* ASTRA_TRAFFIC_H */
