/* test_topology.c — verify C ISL topology + link budget against the Python
 * oracle (tools/topology_vectors.txt) across several epochs. */
#include "astra/graph.h"
#include "astra/config.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "tools/topology_vectors.txt";
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s (run: python3 tools/oracle_topology.py)\n", path); return 2; }

    static NetworkGraph g;
    static vec3 pos[ASTRA_MAX_NODES];
    static node_id ids[ASTRA_MAX_NODES];

    int epochs = 0, fails = 0;
    double worst_d = 0, worst_lat = 0, worst_bw = 0, worst_loss = 0;
    long total_edges = 0;

    char tag[16];
    while (fscanf(f, "%15s", tag) == 1) {
        if (strcmp(tag, "EPOCH") == 0) {
            int step; if (fscanf(f, "%d", &step) != 1) { fclose(f); return 6; }
            /* read 100 POS lines */
            uint32_t n = 0;
            for (uint32_t i = 0; i < ASTRA_NUM_TOTAL_SATS; ++i) {
                char t2[16]; int id; vec3 r;
                if (fscanf(f, "%15s %d %lf %lf %lf", t2, &id, &r.x, &r.y, &r.z) != 5) { fclose(f); return 3; }
                pos[id] = r; ids[n++] = (node_id)id;
            }
            astra_build_isl_topology(&g, pos, ids, n, ASTRA_EARTH_RADIUS_KM, ASTRA_MAX_LINK_RANGE_KM);

            char t3[16]; int nedges;
            if (fscanf(f, "%15s %d", t3, &nedges) != 2) { fclose(f); return 4; }
            if ((uint32_t)nedges != g.link_count) {
                printf("epoch %d: EDGE COUNT MISMATCH python=%d c=%u\n", step, nedges, g.link_count);
                fails++;
            }
            total_edges += nedges;
            for (int e = 0; e < nedges; ++e) {
                char t4[16]; int a, b; double d, lat, bw, loss;
                if (fscanf(f, "%15s %d %d %lf %lf %lf %lf", t4, &a, &b, &d, &lat, &bw, &loss) != 7) { fclose(f); return 5; }
                const Link *L = astra_get_link(&g, (node_id)a, (node_id)b);
                if (!L) { printf("epoch %d: missing C edge %d-%d\n", step, a, b); fails++; continue; }
                double dd  = fabs(L->distance_km - d);
                double dl  = fabs((double)L->props.latency_s - lat);
                double dbw = fabs((double)L->props.bandwidth_mbps - bw);
                double dls = fabs((double)L->props.loss_prob - loss);
                if (dd  > worst_d)    worst_d = dd;
                if (dl  > worst_lat)  worst_lat = dl;
                if (dbw > worst_bw)   worst_bw = dbw;
                if (dls > worst_loss) worst_loss = dls;
            }
            epochs++;
        }
    }
    fclose(f);

    /* float32 link props -> ~1e-4 relative tolerance is expected; distance is
     * double and should match to ~1e-9. */
    int ok = (fails == 0 && epochs > 0 &&
              worst_d < 1e-6 && worst_lat < 1e-7 && worst_bw < 1e-1 && worst_loss < 1e-6);
    printf("verified %d epochs, %ld total edges\n", epochs, total_edges);
    printf("  max err: dist=%.3e km  lat=%.3e s  bw=%.3e Mbps  loss=%.3e\n",
           worst_d, worst_lat, worst_bw, worst_loss);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
