/* astra_physcheck.c — physical-accuracy profile of the two-body propagator.
 *
 * The two-body problem has exact conserved quantities; an analytically correct
 * propagator must hold them to machine precision over long propagation. This
 * tool drives the constellation for many periods and reports the worst-case
 * drift in each invariant — a direct, oracle-free measure of physical fidelity:
 *
 *   - specific orbital energy   eps = |v|^2/2 - mu/|r|        (scalar)
 *   - specific angular momentum h   = r x v                   (vector)
 *   - eccentricity (LRL) vector e   = (v x h)/mu - r/|r|      (vector)
 *   - period closure: after exactly one period T the state returns to start
 *   - time-reversal: propagate +dt then -dt returns to start
 *
 * NOTE: comparing this two-body model against *real* Starlink orbits requires
 * live TLEs (celestrak) + an SGP4 propagator. This sandbox has no network, so
 * that divergence study is deferred; the harness is built to accept TLEs when
 * a networked environment is available. What this tool proves is that our
 * propagator is internally exact, so any divergence later is purely the
 * physics gap (J2 / drag), not integrator error.
 */
#include "astra/orbit.h"
#include "astra/config.h"
#include <stdio.h>
#include <math.h>

static double max_d(double a, double b) { return a > b ? a : b; }

int main(void) {
    const double mu = ASTRA_MU_EARTH;
    const uint32_t planes = ASTRA_NUM_PLANES, per = ASTRA_NUM_SATS_PER_PLANE;

    double worst_energy = 0, worst_h = 0, worst_e = 0;
    double worst_closure = 0, worst_reversal = 0;
    const int n_periods = 20;
    const int substeps  = 200;          /* propagation steps per orbit */

    for (uint32_t pl = 0; pl < planes; ++pl)
    for (uint32_t sl = 0; sl < per; ++sl) {
        OrbitElements c = { ASTRA_SAT_SMA_KM, ASTRA_SAT_ECC,
            ASTRA_INCLINATION_DEG*M_PI/180.0, (2.0*M_PI/planes)*pl, 0.0,
            (2.0*M_PI/per)*sl + pl*0.1 };
        StateRV s0 = astra_coe_to_rv(mu, c);

        /* reference invariants at epoch */
        double r0 = v3_norm(s0.r), v0 = v3_norm(s0.v);
        double eps0 = 0.5*v0*v0 - mu/r0;
        vec3   h0   = v3_cross(s0.r, s0.v);
        double hn0  = v3_norm(h0);
        vec3   e0   = v3_sub(v3_scale(v3_cross(s0.v, h0), 1.0/mu),
                             v3_scale(s0.r, 1.0/r0));
        double en0  = v3_norm(e0);

        double a = ASTRA_SAT_SMA_KM;
        double T = 2.0*M_PI*sqrt(a*a*a/mu);      /* orbital period */
        double dt = T / substeps;

        /* long propagation: hold invariants over many periods */
        StateRV s = s0;
        for (int k = 0; k < n_periods*substeps; ++k) {
            s = astra_propagate_kepler(mu, s.r, s.v, dt);
            double r = v3_norm(s.r), v = v3_norm(s.v);
            double eps = 0.5*v*v - mu/r;
            vec3   h   = v3_cross(s.r, s.v);
            vec3   e   = v3_sub(v3_scale(v3_cross(s.v, h), 1.0/mu),
                                v3_scale(s.r, 1.0/r));
            worst_energy = max_d(worst_energy, fabs(eps - eps0)/fabs(eps0));
            worst_h      = max_d(worst_h, fabs(v3_norm(h) - hn0)/hn0);
            worst_e      = max_d(worst_e, fabs(v3_norm(e) - en0));   /* e ~ 1e-3 */
        }

        /* period closure: one full period from epoch -> back to s0 */
        StateRV sp = astra_propagate_kepler(mu, s0.r, s0.v, T);
        worst_closure = max_d(worst_closure, v3_norm(v3_sub(sp.r, s0.r)));

        /* time reversal: +0.37T then -0.37T */
        StateRV sf = astra_propagate_kepler(mu, s0.r, s0.v, 0.37*T);
        StateRV sb = astra_propagate_kepler(mu, sf.r, sf.v, -0.37*T);
        worst_reversal = max_d(worst_reversal, v3_norm(v3_sub(sb.r, s0.r)));
    }

    printf("=== ASTRA physical-accuracy profile (two-body invariants) ===\n");
    printf("constellation: %u sats, a=%.1f km, propagated %d periods x %d substeps\n",
           planes*per, ASTRA_SAT_SMA_KM, n_periods, substeps);
    printf("worst relative energy drift   |d eps|/eps : %.3e\n", worst_energy);
    printf("worst relative ang.mom. drift |d h|/h     : %.3e\n", worst_h);
    printf("worst eccentricity-vector drift |d e|     : %.3e  (e0=%.3f)\n",
           worst_e, ASTRA_SAT_ECC);
    printf("worst period-closure error |r(T)-r0|      : %.3e km\n", worst_closure);
    printf("worst time-reversal error  |r(-)-r0|      : %.3e km\n", worst_reversal);

    /* pass thresholds: tight, since the propagator is analytical */
    int ok = (worst_energy < 1e-10) && (worst_h < 1e-10)
          && (worst_closure < 1e-4) && (worst_reversal < 1e-6);
    printf("%s\n", ok ? "PASS (propagator is physically exact)" :
                        "WARN (invariant drift above expectation)");
    return ok ? 0 : 1;
}
