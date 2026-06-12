/* traffic.c — discrete packet sim over the dynamic graph. Port of
 * traffic/sim.py with a fixed packet pool and running counters. */
#include "astra/traffic.h"
#include <string.h>

#define PKT_TTL      32
#define PKT_SIZE     1200u

static pkt_id pool_alloc(TrafficSim *t) {
    pkt_id id = t->free_head;
    if (id == ASTRA_INVALID) return ASTRA_INVALID;
    t->free_head = t->pool[id].next;
    t->live++;
    return id;
}
static void pool_free(TrafficSim *t, pkt_id id) {
    t->pool[id].state = PK_FREE;
    t->pool[id].next = t->free_head;
    t->free_head = id;
    t->live--;
}

void astra_traffic_init(TrafficSim *t, TrafficPattern pattern, double rate_pps,
                        node_id hotspot, uint64_t seed) {
    memset(t, 0, sizeof(*t));
    for (uint32_t i = 0; i < ASTRA_MAX_PACKETS; ++i) {
        t->pool[i].state = PK_FREE;
        t->pool[i].next = (i + 1 < ASTRA_MAX_PACKETS) ? (pkt_id)(i + 1) : ASTRA_INVALID;
    }
    t->free_head = 0;
    t->live = 0;
    t->gen.pattern = pattern;
    t->gen.rate_pps = rate_pps;
    t->gen.hotspot = hotspot;
    t->gen.burst_left = 0;
    rng_seed(&t->rng, seed, 0xA5A5u);
}

double astra_delivery_ratio(const TrafficStats *s) {
    uint64_t dropped = s->drop_no_route + s->drop_broken + s->drop_ttl +
                       s->drop_loss + s->drop_inactive;
    uint64_t denom = s->delivered + dropped;
    return denom ? (double)s->delivered / (double)denom : 0.0;
}

/* spawn a packet; returns 1 on success, 0 if pool exhausted */
static int spawn(TrafficSim *t, node_id src, node_id dst, double now) {
    pkt_id id = pool_alloc(t);
    if (id == ASTRA_INVALID) { t->stats.drop_overflow++; return 0; }
    Packet *p = &t->pool[id];
    p->src = src; p->dst = dst; p->cur = src;
    p->created_t = now; p->arrival_t = 0; p->transit_to = ASTRA_INVALID;
    p->hops = 0; p->ttl = PKT_TTL; p->size_bytes = (uint16_t)PKT_SIZE;
    p->state = PK_AT_NODE;
    t->stats.generated++;
    return 1;
}

static void generate(TrafficSim *t, const node_id *act, uint32_t n, double now, double dt) {
    if (n < 2) return;
    double n_expect = t->gen.rate_pps * dt;
    int k = (int)n_expect;
    if (rng_double(&t->rng) < (n_expect - (double)k)) k++;

    if (t->gen.pattern == TR_BURST) {
        if (t->gen.burst_left <= 0 && rng_double(&t->rng) < 0.03)
            t->gen.burst_left = 20 + (int)rng_below(&t->rng, 61);   /* [20,80] */
        if (t->gen.burst_left > 0) {
            int add = t->gen.burst_left < 50 ? t->gen.burst_left : 50;
            k += add; t->gen.burst_left -= k;
        }
    }
    for (int i = 0; i < k; ++i) {
        node_id src, dst;
        if (t->gen.pattern == TR_HOTSPOT) {
            node_id other = act[rng_below(&t->rng, n)];
            if (rng_double(&t->rng) < 0.5) { src = other; dst = t->gen.hotspot; }
            else { src = t->gen.hotspot; dst = other; }
            if (src == dst) continue;
        } else {
            uint32_t a = rng_below(&t->rng, n), b = rng_below(&t->rng, n);
            while (b == a) b = rng_below(&t->rng, n);
            src = act[a]; dst = act[b];
        }
        spawn(t, src, dst, now);
    }
}

void astra_traffic_step(TrafficSim *t, const NetworkGraph *g, const Router *r,
                        const node_id *act, uint32_t n, double now, double dt) {
    /* active bitset */
    static uint8_t active[ASTRA_MAX_NODES];
    memset(active, 0, sizeof(active));
    for (uint32_t i = 0; i < n; ++i) active[act[i]] = 1;

    /* 1) deliver arrivals */
    for (uint32_t i = 0; i < ASTRA_MAX_PACKETS; ++i) {
        Packet *p = &t->pool[i];
        if (p->state != PK_IN_TRANSIT) continue;
        if (p->arrival_t > now) continue;
        p->cur = p->transit_to;
        if (p->cur == p->dst) {
            t->stats.delivered++;
            t->stats.sum_delay_s += now - p->created_t;
            t->stats.sum_hops    += p->hops;
            pool_free(t, (pkt_id)i);
        } else {
            p->state = PK_AT_NODE;
        }
    }

    /* 2) generate */
    generate(t, act, n, now, dt);

    /* 3) route AT_NODE packets, dropping the un-routable; stash pending hop */
    for (uint32_t i = 0; i < ASTRA_MAX_PACKETS; ++i) {
        Packet *p = &t->pool[i];
        if (p->state != PK_AT_NODE) continue;
        if (p->cur >= ASTRA_MAX_NODES || !active[p->cur]) { t->stats.drop_inactive++; pool_free(t, (pkt_id)i); continue; }
        if (p->ttl <= 0) { t->stats.drop_ttl++; pool_free(t, (pkt_id)i); continue; }
        if (p->cur == p->dst) {
            t->stats.delivered++;
            t->stats.sum_delay_s += now - p->created_t;
            t->stats.sum_hops    += p->hops;
            pool_free(t, (pkt_id)i); continue;
        }
        node_id nh = astra_next_hop(r, p->cur, p->dst);
        if (nh == ASTRA_INVALID) { t->stats.drop_no_route++; pool_free(t, (pkt_id)i); continue; }
        const Link *L = astra_get_link(g, p->cur, nh);
        if (!L || !L->up) { t->stats.drop_broken++; pool_free(t, (pkt_id)i); continue; }
        p->transit_to = nh;   /* pending next hop for the bandwidth phase */
    }

    /* 4) transmit subject to per-directed-link bandwidth */
    memset(t->cap_used, 0, sizeof(t->cap_used));
    for (uint32_t i = 0; i < ASTRA_MAX_PACKETS; ++i) {
        Packet *p = &t->pool[i];
        if (p->state != PK_AT_NODE) continue;
        node_id nh = p->transit_to;
        if (nh == ASTRA_INVALID) continue;
        const Link *L = astra_get_link(g, p->cur, nh);
        if (!L || !L->up) { t->stats.drop_broken++; pool_free(t, (pkt_id)i); continue; }
        uint32_t eid = (uint32_t)(L - g->links);
        uint32_t dir = (p->cur == L->a) ? 0u : 1u;
        uint32_t ci  = 2u * eid + dir;
        double capacity = (double)L->props.bandwidth_mbps * 1e6 * dt;
        double bits = (double)p->size_bytes * 8.0;
        if (t->cap_used[ci] + bits > capacity) continue;          /* waits (queued) */
        if (rng_double(&t->rng) < (double)L->props.loss_prob) {
            t->stats.drop_loss++; pool_free(t, (pkt_id)i); continue;
        }
        t->cap_used[ci] += bits;
        p->ttl--; p->hops++;
        p->state = PK_IN_TRANSIT;
        p->arrival_t = now + L->props.latency_s;
        /* p->transit_to already = nh; p->cur updated on arrival */
    }

    /* 5) stats snapshot */
    uint32_t in_flight = 0, at_node = 0;
    for (uint32_t i = 0; i < ASTRA_MAX_PACKETS; ++i) {
        if (t->pool[i].state == PK_IN_TRANSIT) in_flight++;
        else if (t->pool[i].state == PK_AT_NODE) at_node++;
    }
    t->stats.in_flight = in_flight;
    t->stats.at_node   = at_node;

    double sum_util = 0; uint32_t used_links = 0;
    for (uint32_t e = 0; e < g->link_count; ++e) {
        double cap = (double)g->links[e].props.bandwidth_mbps * 1e6 * dt;
        for (uint32_t dir = 0; dir < 2; ++dir) {
            double u = t->cap_used[2u*e + dir];
            if (u > 0.0 && cap > 0.0) { sum_util += u / cap; used_links++; }
        }
    }
    t->stats.last_link_util = used_links ? sum_util / (double)used_links : 0.0;
}
