/* test_j2.c — invariant verification for opt-in J2 nodal precession.
 *   (1) node rate for the default Starlink orbit ~ -4.5 deg/day (real value).
 *   (2) sign physics: prograde<0, retrograde>0, polar~0.
 *   (3) higher altitude -> smaller magnitude.
 *   (4) sim integration: a J2-enabled run equals a two-body run with each
 *       plane rigidly rotated about ECI z by rate*t (two-body propagation is
 *       rotation-equivariant, so the per-step rotations must compose exactly).
 * The two-body parity path is the j2_enabled==0 default and is covered by the
 * orbit/topology/routing parity tests; this only exercises the opt-in branch. */
#include "astra/sim.h"
#include "astra/orbit.h"
#include <stdio.h>
#include <math.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static SimState A, B;

int main(void) {
    const double mu = ASTRA_MU_EARTH, Re = ASTRA_EARTH_RADIUS_KM;
    double a = ASTRA_SAT_SMA_KM, e = ASTRA_SAT_ECC, inc = ASTRA_INCLINATION_DEG*M_PI/180.0;

    /* (1) magnitude + (2) sign */
    double rate = astra_j2_node_rate(mu, Re, a, e, inc);
    double deg_day = rate * 86400.0 * 180.0/M_PI;
    CHECK(rate < 0.0, "prograde node rate must be negative");
    CHECK(fabs(deg_day - (-4.49)) < 0.3, "Starlink regression ~ -4.5 deg/day, got %.3f", deg_day);
    CHECK(astra_j2_node_rate(mu, Re, a, e, 100.0*M_PI/180.0) > 0.0, "retrograde must be positive");
    CHECK(fabs(astra_j2_node_rate(mu, Re, a, e, M_PI/2.0)) < 1e-12, "polar orbit node rate ~ 0");

    /* (3) altitude dependence */
    CHECK(fabs(astra_j2_node_rate(mu, Re, a+500.0, e, inc)) < fabs(rate),
          "node rate magnitude must fall with altitude");

    /* (4) sim integration matches an analytic rigid rotation */
    astra_sim_init_cfg(&A, 1u, ASTRA_NUM_PLANES, ASTRA_NUM_SATS_PER_PLANE, 5000.0);
    astra_sim_init_cfg(&B, 1u, ASTRA_NUM_PLANES, ASTRA_NUM_SATS_PER_PLANE, 5000.0);
    B.j2_enabled = 1;
    for (int k = 0; k < 1000; ++k) { astra_sim_tick(&A); astra_sim_tick(&B); }

    double t = A.sim_time_s, maxerr = 0.0;
    for (uint32_t s = 0; s < A.num_sats; ++s) {
        double ang = B.j2_node_rate[s]*t, ca = cos(ang), sa = sin(ang);
        vec3 p = A.field.state[s].r;                 /* two-body reference  */
        double ex = p.x*ca - p.y*sa, ey = p.x*sa + p.y*ca, ez = p.z;
        vec3 q = B.field.state[s].r;                 /* J2-precessed run    */
        double d = fabs(q.x-ex) + fabs(q.y-ey) + fabs(q.z-ez);
        if (d > maxerr) maxerr = d;
    }
    CHECK(maxerr < 1e-3, "J2-on must equal J2-off rotated by rate*t (maxerr %.3e km)", maxerr);

    if (fails) { printf("FAIL: %d invariant(s) violated\n", fails); return 1; }
    printf("J2 nodal precession: %.3f deg/day; integration matches rotation (err %.2e km)\n",
           deg_day, maxerr);
    printf("PASS\n");
    return 0;
}
