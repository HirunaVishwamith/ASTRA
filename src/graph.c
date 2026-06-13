/* graph.c — link budget + topology + CSR. Port of link_budget.py and
 * network/graph.py (build_topology). Verified against Python topology dumps. */
#include "astra/graph.h"
#include "astra/orbit.h"
#include <string.h>
#include <math.h>

LinkProps astra_inverse_square_budget(double distance_km, double base_bw,
                                      double base_loss, double ref_km) {
    double d = distance_km < 1.0 ? 1.0 : distance_km;
    double scale = (ref_km / d) * (ref_km / d);
    double bw_scale = scale < 0.05 ? 0.05 : (scale > 1.0 ? 1.0 : scale);
    double inv = 1.0 / (scale < 1e-9 ? 1e-9 : scale);
    double loss = base_loss * (inv < 1.0 ? 1.0 : inv);
    if (loss < 0.0) loss = 0.0; else if (loss > 0.2) loss = 0.2;
    LinkProps p;
    p.latency_s      = 0.0;                         /* caller fills latency */
    p.bandwidth_mbps = (float)(base_bw * bw_scale);
    p.loss_prob      = (float)loss;
    return p;
}

void astra_graph_clear(NetworkGraph *g) {
    g->link_count = 0;
    g->adj_count  = 0;
    memset(g->adj_first, -1, sizeof(g->adj_first));
}

void astra_graph_add(NetworkGraph *g, node_id u, node_id v,
                     double distance_km, LinkProps props) {
    if (g->link_count >= ASTRA_MAX_LINKS) return;
    link_id eid = g->link_count++;
    Link *L = &g->links[eid];
    L->a = u; L->b = v; L->distance_km = distance_km; L->props = props; L->up = 1;

    /* push both directions onto the intrusive adjacency chains */
    uint32_t s0 = g->adj_count++;
    g->adj_to[s0]  = v; g->adj_eid[s0] = eid;
    g->adj_next[s0] = g->adj_first[u]; g->adj_first[u] = (int32_t)s0;

    uint32_t s1 = g->adj_count++;
    g->adj_to[s1]  = u; g->adj_eid[s1] = eid;
    g->adj_next[s1] = g->adj_first[v]; g->adj_first[v] = (int32_t)s1;
}

const Link *astra_get_link(const NetworkGraph *g, node_id u, node_id v) {
    for (int32_t s = g->adj_first[u]; s >= 0; s = g->adj_next[s])
        if (g->adj_to[s] == v) return &g->links[g->adj_eid[s]];
    return NULL;
}

int astra_has_link(const NetworkGraph *g, node_id u, node_id v) {
    return astra_get_link(g, u, v) != NULL;
}

void astra_build_isl_topology(NetworkGraph *g, const vec3 *pos,
                              const node_id *ids, uint32_t n,
                              double earth_r, double max_range) {
    astra_graph_clear(g);
    for (uint32_t i = 0; i < n; ++i) {
        node_id a = ids[i];
        vec3 ra = pos[a];
        for (uint32_t j = i + 1; j < n; ++j) {
            node_id b = ids[j];
            vec3 rb = pos[b];
            double d = v3_norm(v3_sub(ra, rb));
            if (d > max_range) continue;
            if (!astra_has_line_of_sight(ra, rb, earth_r, 0.0)) continue;

            LinkProps p = astra_inverse_square_budget(d, ASTRA_BASE_BW_MBPS,
                                                      ASTRA_BASE_LOSS, ASTRA_ISL_REF_KM);
            p.latency_s = d / ASTRA_C_LIGHT_KMS + ASTRA_EXTRA_LATENCY_S;
            astra_graph_add(g, a, b, d, p);
        }
    }
}

void astra_build_csr(NetworkGraph *g, uint32_t node_count, CostMode cost) {
    CSRGraph *c = &g->csr;
    c->node_count = node_count;
    /* counting sort by source node: each undirected link contributes 2 edges */
    uint32_t deg[ASTRA_MAX_NODES] = {0};
    for (uint32_t e = 0; e < g->link_count; ++e) {
        if (!g->links[e].up) continue;
        deg[g->links[e].a]++; deg[g->links[e].b]++;
    }
    uint32_t acc = 0;
    for (uint32_t i = 0; i < node_count; ++i) { c->row_start[i] = acc; acc += deg[i]; }
    c->row_start[node_count] = acc;
    c->nnz = acc;

    uint32_t cur[ASTRA_MAX_NODES];
    memcpy(cur, c->row_start, sizeof(uint32_t) * (node_count));
    for (uint32_t e = 0; e < g->link_count; ++e) {
        const Link *L = &g->links[e];
        if (!L->up) continue;
        double w = (cost == ASTRA_COST_HOPS) ? 1.0 : L->props.latency_s;
        uint32_t pa = cur[L->a]++, pb = cur[L->b]++;
        c->col[pa] = L->b; c->w[pa] = w; c->eid[pa] = e;
        c->col[pb] = L->a; c->w[pb] = w; c->eid[pb] = e;
    }
}
