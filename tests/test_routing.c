/* test_routing.c — verify C Dijkstra all-pairs router against Python oracle
 * (tools/routing_vectors.txt): next-hop must match bit-exactly, cost to 1e-9. */
#include "astra/routing.h"
#include "astra/graph.h"
#include "astra/config.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static Router        R;       /* large: keep off the stack */
static NetworkGraph  G;

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "tools/routing_vectors.txt";
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s (run: python3 tools/oracle_routing.py)\n", path); return 2; }

    static vec3 pos[ASTRA_MAX_NODES];
    static node_id ids[ASTRA_MAX_NODES];
    int epochs = 0;
    long pairs = 0, nh_mismatch = 0;
    double worst_cost = 0;

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
        astra_build_isl_topology(&G, pos, ids, n, ASTRA_EARTH_RADIUS_KM, ASTRA_MAX_LINK_RANGE_KM);
        astra_build_csr(&G, n, ASTRA_COST_LATENCY);
        astra_router_init(&R, ASTRA_ROUTE_DIJKSTRA, ASTRA_COST_LATENCY);
        astra_router_step(&R, &G.csr);

        char t3[16]; int nroutes;
        if (fscanf(f, "%15s %d", t3, &nroutes) != 2) { fclose(f); return 4; }
        for (int e = 0; e < nroutes; ++e) {
            char t4[16]; int src, dst, py_nh; double py_cost;
            if (fscanf(f, "%15s %d %d %d %lf", t4, &src, &dst, &py_nh, &py_cost) != 5) { fclose(f); return 5; }
            node_id c_nh = astra_next_hop(&R, (node_id)src, (node_id)dst);
            int c_nh_i = (c_nh == ASTRA_INVALID) ? -1 : (int)c_nh;
            if (c_nh_i != py_nh) {
                if (nh_mismatch < 8)
                    printf("epoch %d: NH mismatch src=%d dst=%d py=%d c=%d\n", step, src, dst, py_nh, c_nh_i);
                nh_mismatch++;
            }
            if (isfinite(py_cost)) {
                double cc = astra_route_cost(&R, (node_id)src, (node_id)dst);
                double dc = fabs(cc - py_cost);
                if (dc > worst_cost) worst_cost = dc;
            }
            pairs++;
        }
        epochs++;
    }
    fclose(f);

    int ok = (epochs > 0 && nh_mismatch == 0 && worst_cost < 1e-9);
    printf("verified %d epochs, %ld pairs\n", epochs, pairs);
    printf("  next-hop mismatches: %ld   max cost err: %.3e s\n", nh_mismatch, worst_cost);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
