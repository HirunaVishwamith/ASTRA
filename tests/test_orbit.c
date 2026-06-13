/* test_orbit.c — verify the C orbit port against the Python oracle dump
 * (tools/orbit_vectors.txt). Fails (nonzero exit) if any error exceeds tol. */
#include "astra/orbit.h"
#include "astra/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static double max_abs(double a, double b) { return a > b ? a : b; }

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "tools/orbit_vectors.txt";
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s (run: python3 tools/oracle_orbit.py)\n", path); return 2; }

    /* Position tolerance in km, velocity in km/s. Two-body + identical
     * algorithm should agree to round-off scaled by step count. */
    const double POS_TOL = 1e-6, VEL_TOL = 1e-9;
    double worst_rv_pos = 0, worst_rv_vel = 0, worst_prop_pos = 0, worst_prop_vel = 0;
    int sats = 0, fails = 0;

    char tag[16];
    OrbitElements coe;
    while (fscanf(f, "%15s", tag) == 1) {
        if (strcmp(tag, "COE") == 0) {
            if (fscanf(f, "%lf %lf %lf %lf %lf %lf",
                       &coe.a, &coe.e, &coe.i, &coe.raan, &coe.argp, &coe.nu) != 6) break;
        } else if (strcmp(tag, "RV") == 0) {
            vec3 r, v;
            if (fscanf(f, "%lf %lf %lf %lf %lf %lf",
                       &r.x, &r.y, &r.z, &v.x, &v.y, &v.z) != 6) break;
            StateRV s = astra_coe_to_rv(ASTRA_MU_EARTH, coe);
            worst_rv_pos = max_abs(worst_rv_pos, v3_norm(v3_sub(s.r, r)));
            worst_rv_vel = max_abs(worst_rv_vel, v3_norm(v3_sub(s.v, v)));
        } else if (strcmp(tag, "PROP") == 0) {
            vec3 r, v;
            if (fscanf(f, "%lf %lf %lf %lf %lf %lf",
                       &r.x, &r.y, &r.z, &v.x, &v.y, &v.z) != 6) break;
            StateRV s = astra_coe_to_rv(ASTRA_MU_EARTH, coe);
            for (int k = 0; k < 100; ++k)
                s = astra_propagate_kepler(ASTRA_MU_EARTH, s.r, s.v, 5.0);
            double dp = v3_norm(v3_sub(s.r, r));
            double dv = v3_norm(v3_sub(s.v, v));
            worst_prop_pos = max_abs(worst_prop_pos, dp);
            worst_prop_vel = max_abs(worst_prop_vel, dv);
            if (dp > POS_TOL || dv > VEL_TOL) fails++;
            sats++;
        }
    }
    fclose(f);

    printf("verified %d satellites\n", sats);
    printf("  coe_to_rv : max pos err = %.3e km   max vel err = %.3e km/s\n", worst_rv_pos, worst_rv_vel);
    printf("  propagate : max pos err = %.3e km   max vel err = %.3e km/s  (100 x 5s)\n", worst_prop_pos, worst_prop_vel);

    int ok = (worst_rv_pos < POS_TOL && worst_rv_vel < VEL_TOL &&
              worst_prop_pos < POS_TOL && worst_prop_vel < VEL_TOL && fails == 0 && sats > 0);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
