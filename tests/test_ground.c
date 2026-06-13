/* test_ground.c — verify ground<->sat link construction + best-sat selection
 * against the Python oracle (tools/ground_vectors.txt). */
#include "astra/ground.h"
#include "astra/graph.h"
#include "astra/config.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static NetworkGraph G;

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "tools/ground_vectors.txt";
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s (run: python3 tools/oracle_ground.py)\n", path); return 2; }

    static vec3 pos[ASTRA_MAX_NODES];
    static node_id sat_ids[ASTRA_MAX_NODES];
    static node_id best[ASTRA_MAX_GROUND];
    GroundStation gs[ASTRA_MAX_GROUND];
    uint32_t ngs = astra_default_ground_stations(gs, ASTRA_NUM_TOTAL_SATS);

    int epochs = 0, fails = 0;
    double worst_d = 0, worst_lat = 0, worst_bw = 0, worst_loss = 0;
    long total_links = 0;

    char tag[16];
    while (fscanf(f, "%15s", tag) == 1) {
        if (strcmp(tag, "EPOCH") != 0) continue;
        int step; if (fscanf(f, "%d", &step) != 1) break;
        uint32_t n = 0;
        for (uint32_t i = 0; i < ASTRA_NUM_TOTAL_SATS; ++i) {
            char t2[16]; int id; vec3 r;
            if (fscanf(f, "%15s %d %lf %lf %lf", t2, &id, &r.x, &r.y, &r.z) != 5) { fclose(f); return 3; }
            pos[id] = r; sat_ids[n++] = (node_id)id;
        }
        astra_graph_clear(&G);
        astra_add_ground_links(&G, gs, ngs, pos, sat_ids, n, (double)step * 5.0,
                               ASTRA_EARTH_RADIUS_KM, ASTRA_GROUND_SAT_MAX_RANGE_KM, best);

        char t3[16]; int nlinks;
        if (fscanf(f, "%15s %d", t3, &nlinks) != 2) { fclose(f); return 4; }
        if ((uint32_t)nlinks != G.link_count) {
            printf("epoch %d: ground link count mismatch python=%d c=%u\n", step, nlinks, G.link_count);
            fails++;
        }
        total_links += nlinks;
        for (int e = 0; e < nlinks; ++e) {
            char t4[16]; int gid, sid; double d, lat, bw, loss;
            if (fscanf(f, "%15s %d %d %lf %lf %lf %lf", t4, &gid, &sid, &d, &lat, &bw, &loss) != 7) { fclose(f); return 5; }
            const Link *L = astra_get_link(&G, (node_id)gid, (node_id)sid);
            if (!L) { printf("epoch %d: missing C ground link %d-%d\n", step, gid, sid); fails++; continue; }
            double dd  = fabs(L->distance_km - d);
            double dl  = fabs(L->props.latency_s - lat);
            double dbw = fabs((double)L->props.bandwidth_mbps - bw);
            double dls = fabs((double)L->props.loss_prob - loss);
            if (dd  > worst_d)    worst_d = dd;
            if (dl  > worst_lat)  worst_lat = dl;
            if (dbw > worst_bw)   worst_bw = dbw;
            if (dls > worst_loss) worst_loss = dls;
        }
        for (uint32_t i = 0; i < ngs; ++i) {
            char t5[16]; int gid, py_best;
            if (fscanf(f, "%15s %d %d", t5, &gid, &py_best) != 3) { fclose(f); return 6; }
            uint32_t slot = (node_id)gid - ASTRA_NUM_TOTAL_SATS;
            int c_best = (best[slot] == ASTRA_INVALID) ? -1 : (int)best[slot];
            if (c_best != py_best) { printf("epoch %d: best-sat mismatch gid=%d py=%d c=%d\n", step, gid, py_best, c_best); fails++; }
        }
        epochs++;
    }
    fclose(f);

    int ok = (fails == 0 && epochs > 0 &&
              worst_d < 1e-6 && worst_lat < 1e-9 && worst_bw < 1e-1 && worst_loss < 1e-6);
    printf("verified %d epochs, %ld ground links\n", epochs, total_links);
    printf("  max err: dist=%.3e km  lat=%.3e s  bw=%.3e Mbps  loss=%.3e\n",
           worst_d, worst_lat, worst_bw, worst_loss);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
