/* graph.h — dynamic network topology + CSR projection.
 *
 * Edge list is the source of truth (rebuilt each tick); CSR is regenerated
 * from it for cache-friendly Dijkstra traversal. No per-tick allocation:
 * all storage is fixed-capacity from config.h. */
#ifndef ASTRA_GRAPH_H
#define ASTRA_GRAPH_H

#include "astra/config.h"
#include "astra/vec3.h"

typedef struct {
    double latency_s;        /* double: it is the routing weight; must match
                              * Python (float64) exactly for path parity */
    float  bandwidth_mbps;
    float  loss_prob;
} LinkProps;

typedef struct {
    node_id   a, b;
    double    distance_km;
    LinkProps props;
    uint8_t   up;        /* 0 if blacked out by impairment engine */
} Link;

/* Compressed Sparse Row adjacency, rebuilt each tick into fixed buffers. */
typedef struct {
    uint32_t row_start[ASTRA_MAX_NODES + 1];
    node_id  col [ASTRA_MAX_LINKS * 2];
    double   w   [ASTRA_MAX_LINKS * 2];   /* routing cost (latency or 1.0) */
    link_id  eid [ASTRA_MAX_LINKS * 2];   /* back-ref into links[]         */
    uint32_t nnz;
    uint32_t node_count;
} CSRGraph;

typedef struct {
    Link     links[ASTRA_MAX_LINKS];
    uint32_t link_count;
    /* adjacency offset index for O(1) get_link / has_link without rebuild */
    int32_t  adj_first[ASTRA_MAX_NODES];   /* head of per-node link chain   */
    int32_t  adj_next [ASTRA_MAX_LINKS*2]; /* intrusive chain (both dirs)   */
    node_id  adj_to   [ASTRA_MAX_LINKS*2]; /* neighbor for that chain slot  */
    link_id  adj_eid  [ASTRA_MAX_LINKS*2];
    uint32_t adj_count;
    CSRGraph csr;
} NetworkGraph;

/* Heuristic inverse-square link budget (port of link_budget.py). */
LinkProps astra_inverse_square_budget(double distance_km, double base_bw_mbps,
                                      double base_loss, double ref_km);

void astra_graph_clear(NetworkGraph *g);

/* Add an undirected edge with precomputed properties. */
void astra_graph_add(NetworkGraph *g, node_id u, node_id v,
                     double distance_km, LinkProps props);

/* O(N^2) ISL topology from satellite ECI positions (LOS + range gated).
 * positions[] indexed by sat id; ids[] lists the active sat ids. */
void astra_build_isl_topology(NetworkGraph *g, const vec3 *positions,
                              const node_id *ids, uint32_t n,
                              double earth_r_km, double max_range_km);

/* Project the edge list into CSR with the given routing cost mode. */
typedef enum { ASTRA_COST_LATENCY, ASTRA_COST_HOPS } CostMode;
void astra_build_csr(NetworkGraph *g, uint32_t node_count, CostMode cost);

/* Query helpers over the intrusive adjacency chain (no CSR needed). */
const Link *astra_get_link(const NetworkGraph *g, node_id u, node_id v);
int         astra_has_link(const NetworkGraph *g, node_id u, node_id v);

#endif /* ASTRA_GRAPH_H */
