/* test_dv.c — verify the Distance-Vector router. Python's DV is a partially
 * converged stateful heuristic, so we verify the property that MATTERS:
 *   (1) at convergence DV cost == Dijkstra (shortest-path) cost, and
 *   (2) DV next-hops induce a valid loop-free path whose summed weight
 *       equals the DV cost.
 * Topologies come from the routing oracle's POS dumps. */
#include "astra/routing.h"
#include "astra/graph.h"
#include "astra/config.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>

static Router       DJ, DV;
static NetworkGraph G;

/* weight of edge (u,v) from CSR, or -1 if absent */
static double edge_w(const CSRGraph *g, node_id u, node_id v) {
    for (uint32_t k = g->row_start[u]; k < g->row_start[u+1]; ++k)
        if (g->col[k] == v) return g->w[k];
    return -1.0;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "tools/routing_vectors.txt";
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }

    static vec3 pos[ASTRA_MAX_NODES];
    static node_id ids[ASTRA_MAX_NODES];
    int epochs = 0, fails = 0;
    double worst_cost = 0, worst_path = 0;
    long checked = 0;

    char tag[16];
    while (fscanf(f, "%15s", tag) == 1) {
        if (strcmp(tag, "EPOCH") != 0) continue;
        int step; if (fscanf(f, "%d", &step) != 1) break;
        uint32_t n = 0;
        for (uint32_t i = 0; i < ASTRA_NUM_TOTAL_SATS; ++i) {
            char t2[16]; int id; vec3 r;
            if (fscanf(f, "%15s %d %lf %lf %lf", t2, &id, &r.x, &r.y, &r.z) != 5) { fclose(f); return 3; }
            pos[id] = r; ids[n++] = (node_id)id;
        }
        char t3[16]; int nroutes;        /* skip the oracle ROUTES block */
        if (fscanf(f, "%15s %d", t3, &nroutes) != 2) { fclose(f); return 4; }
        for (int e = 0; e < nroutes; ++e) {
            char t4[24]; if (fscanf(f, "%15s %23s %23s %23s %23s", tag, t4, t4, t4, t4) != 5) { fclose(f); return 5; }
        }

        astra_build_isl_topology(&G, pos, ids, n, ASTRA_EARTH_RADIUS_KM, ASTRA_MAX_LINK_RANGE_KM);
        astra_build_csr(&G, n, ASTRA_COST_LATENCY);

        astra_router_init(&DJ, ASTRA_ROUTE_DIJKSTRA, ASTRA_COST_LATENCY);
        astra_router_step(&DJ, &G.csr);

        astra_router_init(&DV, ASTRA_ROUTE_DV, ASTRA_COST_LATENCY);
        for (int s = 0; s < 80; ++s) astra_router_step(&DV, &G.csr);  /* converge */

        for (node_id src = 0; src < n; ++src)
            for (node_id dst = 0; dst < n; ++dst) {
                double cj = astra_route_cost(&DJ, src, dst);
                double cv = astra_route_cost(&DV, src, dst);
                int reach_j = (cj < DBL_MAX);
                int reach_v = (cv < DBL_MAX);
                if (reach_j != reach_v) { fails++; continue; }
                if (!reach_j) continue;
                checked++;
                double dc = fabs(cj - cv);
                if (dc > worst_cost) worst_cost = dc;
                if (dc > 1e-9) fails++;

                /* (2) walk DV next-hops, accumulate weight, must reach dst */
                if (src == dst) continue;
                node_id cur = src; double acc = 0; int ok = 0;
                for (uint32_t h = 0; h < n; ++h) {
                    node_id nh = astra_next_hop(&DV, cur, dst);
                    if (nh == ASTRA_INVALID) break;
                    double w = edge_w(&G.csr, cur, nh);
                    if (w < 0) break;          /* next-hop not a real neighbour */
                    acc += w; cur = nh;
                    if (cur == dst) { ok = 1; break; }
                }
                if (!ok) { fails++; continue; }
                double dp = fabs(acc - cv);
                if (dp > worst_path) worst_path = dp;
                if (dp > 1e-9) fails++;
            }
        epochs++;
    }
    fclose(f);

    int pass = (epochs > 0 && fails == 0 && worst_cost < 1e-9 && worst_path < 1e-9);
    printf("verified %d epochs, %ld reachable pairs\n", epochs, checked);
    printf("  DV-vs-Dijkstra cost err: %.3e s   path-vs-cost err: %.3e s   fails: %d\n",
           worst_cost, worst_path, fails);
    printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
