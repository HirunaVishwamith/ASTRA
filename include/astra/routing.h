/* routing.h — pluggable routing over the CSR graph, zero-allocation.
 *
 * Dijkstra is run from every source each step (all-pairs next-hop table),
 * matching routing/algorithms.py. The min-heap uses (dist, node) ordering to
 * reproduce Python's heapq tie-breaking, so next-hop tables are bit-identical
 * for verification. */
#ifndef ASTRA_ROUTING_H
#define ASTRA_ROUTING_H

#include "astra/graph.h"

typedef enum { ASTRA_ROUTE_DIJKSTRA, ASTRA_ROUTE_DV } RouteMode;

typedef struct { double d; node_id n; } HeapEntry;

typedef struct {
    RouteMode mode;
    CostMode  cost;
    uint32_t  node_count;

    /* all-pairs forwarding tables: indexed [src*node_count + dst].
     * Large; the Router must live in static/heap storage, never the stack. */
    node_id   next_hop[ASTRA_MAX_NODES * ASTRA_MAX_NODES];
    double    cost_tbl[ASTRA_MAX_NODES * ASTRA_MAX_NODES];

    /* --- per-source Dijkstra scratch (reused, never reallocated) --- */
    double    s_dist[ASTRA_MAX_NODES];
    node_id   s_prev[ASTRA_MAX_NODES];
    /* lazy binary heap of (dist,node); capacity bounds total relaxations */
    HeapEntry heap[ASTRA_MAX_LINKS * 2 + ASTRA_MAX_NODES];
    uint32_t  heap_len;
} Router;

void    astra_router_init(Router *r, RouteMode mode, CostMode cost);
void    astra_router_step(Router *r, const CSRGraph *g);
node_id astra_next_hop(const Router *r, node_id src, node_id dst);
double  astra_route_cost(const Router *r, node_id src, node_id dst);

#endif /* ASTRA_ROUTING_H */
