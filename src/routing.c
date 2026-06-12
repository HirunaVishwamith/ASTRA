/* routing.c — zero-allocation Dijkstra all-pairs router.
 *
 * Port of routing/algorithms.py DijkstraRouter. Uses a lazy binary min-heap
 * ordered by (dist, node) to reproduce Python heapq tie-breaking exactly, so
 * next-hop tables are bit-identical to the reference. */
#include "astra/routing.h"
#include <string.h>
#include <float.h>

#define INF DBL_MAX

void astra_router_init(Router *r, RouteMode mode, CostMode cost) {
    r->mode = mode;
    r->cost = cost;
    r->node_count = 0;
    r->heap_len = 0;
}

/* (d,n) lexicographic min-heap, matching Python tuple ordering. */
static int heap_less(const Router *r, uint32_t i, uint32_t j) {
    if (r->heap[i].d != r->heap[j].d) return r->heap[i].d < r->heap[j].d;
    return r->heap[i].n < r->heap[j].n;
}

static void heap_push(Router *r, double d, node_id n) {
    uint32_t i = r->heap_len++;
    r->heap[i].d = d; r->heap[i].n = n;
    while (i > 0) {
        uint32_t p = (i - 1) / 2;
        if (heap_less(r, i, p)) {
            HeapEntry t = r->heap[i];
            r->heap[i] = r->heap[p]; r->heap[p] = t; i = p;
        } else break;
    }
}

static void heap_pop(Router *r, double *d, node_id *n) {
    *d = r->heap[0].d; *n = r->heap[0].n;
    r->heap[0] = r->heap[--r->heap_len];
    uint32_t i = 0;
    for (;;) {
        uint32_t l = 2*i+1, rgt = 2*i+2, m = i;
        if (l < r->heap_len && heap_less(r, l, m)) m = l;
        if (rgt < r->heap_len && heap_less(r, rgt, m)) m = rgt;
        if (m == i) break;
        HeapEntry t = r->heap[i];
        r->heap[i] = r->heap[m]; r->heap[m] = t; i = m;
    }
}

static void dijkstra_from(Router *r, const CSRGraph *g, node_id src) {
    uint32_t N = g->node_count;
    for (uint32_t i = 0; i < N; ++i) { r->s_dist[i] = INF; r->s_prev[i] = ASTRA_INVALID; }
    r->s_dist[src] = 0.0;
    r->heap_len = 0;
    heap_push(r, 0.0, src);

    while (r->heap_len > 0) {
        double d; node_id u;
        heap_pop(r, &d, &u);
        if (d != r->s_dist[u]) continue;             /* stale entry */
        for (uint32_t k = g->row_start[u]; k < g->row_start[u+1]; ++k) {
            node_id v = g->col[k];
            double nd = d + g->w[k];
            if (nd < r->s_dist[v]) {
                r->s_dist[v] = nd;
                r->s_prev[v] = u;
                heap_push(r, nd, v);
            }
        }
    }

    /* reconstruct next-hop for every destination (walk prev back to src) */
    node_id *nh   = &r->next_hop[(size_t)src * N];
    double  *cost = &r->cost_tbl[(size_t)src * N];
    for (uint32_t dst = 0; dst < N; ++dst) {
        if (dst == src) { nh[dst] = src; cost[dst] = 0.0; continue; }
        if (r->s_dist[dst] == INF) { nh[dst] = ASTRA_INVALID; cost[dst] = INF; continue; }
        node_id cur = (node_id)dst;
        while (r->s_prev[cur] != src && r->s_prev[cur] != ASTRA_INVALID)
            cur = r->s_prev[cur];
        nh[dst]   = (r->s_prev[cur] == src) ? cur : ASTRA_INVALID;
        cost[dst] = r->s_dist[dst];
    }
}

void astra_router_step(Router *r, const CSRGraph *g) {
    r->node_count = g->node_count;
    if (r->mode == ASTRA_ROUTE_DIJKSTRA) {
        for (node_id src = 0; src < g->node_count; ++src)
            dijkstra_from(r, g, src);
    }
    /* ASTRA_ROUTE_DV implemented in a later module. */
}

node_id astra_next_hop(const Router *r, node_id src, node_id dst) {
    if (src >= r->node_count || dst >= r->node_count) return ASTRA_INVALID;
    return r->next_hop[(size_t)src * r->node_count + dst];
}

double astra_route_cost(const Router *r, node_id src, node_id dst) {
    if (src >= r->node_count || dst >= r->node_count) return INF;
    return r->cost_tbl[(size_t)src * r->node_count + dst];
}
