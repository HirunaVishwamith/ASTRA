/* orbit.c — faithful C port of physics/orbit.py.
 *
 * Algorithms and constants match the Python reference exactly; verified
 * numerically by tests/test_orbit against a Python oracle dump. */
#include "astra/orbit.h"
#include "astra/config.h"
#include <math.h>

double astra_j2_node_rate(double mu, double body_r_km, double a, double e, double i_rad) {
    if (a <= 0.0) return 0.0;
    double n  = sqrt(mu / (a*a*a));        /* mean motion (rad/s)        */
    double p  = a * (1.0 - e*e);           /* semi-latus rectum (km)     */
    double rp = body_r_km / p;
    return -1.5 * n * ASTRA_J2 * rp*rp * cos(i_rad);
}

/* ---- Stumpff functions -------------------------------------------------- */
static double stumpff_C(double z) {
    if (z > 0.0) { double sz = sqrt(z);  return (1.0 - cos(sz)) / z; }
    if (z < 0.0) { double sz = sqrt(-z); return (cosh(sz) - 1.0) / (-z); }
    return 0.5;
}

static double stumpff_S(double z) {
    if (z > 0.0) { double sz = sqrt(z);  return (sz - sin(sz))  / (sz*sz*sz); }
    if (z < 0.0) { double sz = sqrt(-z); return (sinh(sz) - sz) / (sz*sz*sz); }
    return 1.0 / 6.0;
}

/* ---- COE -> RV ----------------------------------------------------------- */
StateRV astra_coe_to_rv(double mu, OrbitElements c) {
    double a = c.a, e = c.e, inc = c.i, raan = c.raan, argp = c.argp, nu = c.nu;
    double p = a * (1.0 - e*e);
    double cnu = cos(nu), snu = sin(nu);
    double denom = 1.0 + e * cnu;

    vec3 r_pf = { p * cnu / denom, p * snu / denom, 0.0 };
    double sp = sqrt(mu / p);
    vec3 v_pf = { -sp * snu, sp * (e + cnu), 0.0 };

    double cO = cos(raan), sO = sin(raan);
    double ci = cos(inc),  si = sin(inc);
    double cw = cos(argp), sw = sin(argp);

    /* Perifocal -> ECI rotation R3(raan) R1(i) R3(argp) */
    double R00 = cO*cw - sO*sw*ci, R01 = -cO*sw - sO*cw*ci, R02 =  sO*si;
    double R10 = sO*cw + cO*sw*ci, R11 = -sO*sw + cO*cw*ci, R12 = -cO*si;
    double R20 = sw*si,            R21 =  cw*si,            R22 =  ci;

    StateRV s;
    s.r.x = R00*r_pf.x + R01*r_pf.y + R02*r_pf.z;
    s.r.y = R10*r_pf.x + R11*r_pf.y + R12*r_pf.z;
    s.r.z = R20*r_pf.x + R21*r_pf.y + R22*r_pf.z;
    s.v.x = R00*v_pf.x + R01*v_pf.y + R02*v_pf.z;
    s.v.y = R10*v_pf.x + R11*v_pf.y + R12*v_pf.z;
    s.v.z = R20*v_pf.x + R21*v_pf.y + R22*v_pf.z;
    return s;
}

/* ---- RV -> COE (best-effort, matches Python clamping) -------------------- */
static double clamp1(double x) { return x < -1.0 ? -1.0 : (x > 1.0 ? 1.0 : x); }
static double wrap2pi(double a) { double t = fmod(a, 2.0*M_PI); return t < 0 ? t + 2.0*M_PI : t; }

OrbitElements astra_rv_to_coe(double mu, vec3 r, vec3 v) {
    OrbitElements c = {0};
    double R = v3_norm(r), V = v3_norm(v);
    vec3 h = v3_cross(r, v);
    double hmag = v3_norm(h);
    vec3 k = {0.0, 0.0, 1.0};
    vec3 n = v3_cross(k, h);
    double nmag = v3_norm(n);

    vec3 e_vec = v3_sub(v3_scale(v3_cross(v, h), 1.0/mu), v3_scale(r, 1.0/R));
    double e = v3_norm(e_vec);

    double energy = V*V/2.0 - mu/R;
    c.a = (fabs(energy) > 1e-12) ? -mu/(2.0*energy) : INFINITY;
    c.e = e;
    c.i = acos(clamp1(h.z / hmag));
    c.raan = (nmag > 1e-12) ? wrap2pi(atan2(n.y, n.x)) : 0.0;

    if (nmag > 1e-12 && e > 1e-10) {
        double argp = acos(clamp1(v3_dot(n, e_vec) / (nmag * e)));
        if (e_vec.z < 0) argp = wrap2pi(2.0*M_PI - argp);
        c.argp = argp;
    } else c.argp = 0.0;

    if (e > 1e-10) {
        double nu = acos(clamp1(v3_dot(e_vec, r) / (e * R)));
        if (v3_dot(r, v) < 0) nu = wrap2pi(2.0*M_PI - nu);
        c.nu = nu;
    } else if (nmag > 1e-12) {
        double u = acos(clamp1(v3_dot(n, r) / (nmag * R)));
        if (r.z < 0) u = wrap2pi(2.0*M_PI - u);
        c.nu = u;
    } else {
        c.nu = wrap2pi(atan2(r.y, r.x));
    }
    return c;
}

/* ---- Universal-variable propagation ------------------------------------- */
StateRV astra_propagate_kepler(double mu, vec3 r0, vec3 v0, double dt_s) {
    double r0mag = v3_norm(r0);
    double vr0 = v3_dot(r0, v0) / r0mag;
    double alpha = 2.0/r0mag - v3_dot(v0, v0)/mu;
    double sqrt_mu = sqrt(mu);

    double chi = (fabs(alpha) > 1e-12)
        ? sqrt_mu * fabs(alpha) * dt_s
        : sqrt_mu * dt_s / r0mag;

    for (int it = 0; it < 50; ++it) {
        double z = alpha * chi * chi;
        double C = stumpff_C(z), S = stumpff_S(z);
        double F = (r0mag*vr0/sqrt_mu) * chi*chi * C
                 + (1.0 - alpha*r0mag) * chi*chi*chi * S
                 + r0mag*chi - sqrt_mu*dt_s;
        double dF = (r0mag*vr0/sqrt_mu) * chi * (1.0 - z*S)
                  + (1.0 - alpha*r0mag) * chi*chi * C
                  + r0mag;
        if (fabs(dF) < 1e-14) break;
        double delta = F / dF;
        chi -= delta;
        if (fabs(delta) < 1e-10) break;
    }

    double z = alpha * chi * chi;
    double C = stumpff_C(z), S = stumpff_S(z);

    double f = 1.0 - (chi*chi/r0mag) * C;
    double g = dt_s - (chi*chi*chi/sqrt_mu) * S;
    vec3 r = v3_add(v3_scale(r0, f), v3_scale(v0, g));
    double rmag = v3_norm(r);

    double fdot = (sqrt_mu/(rmag*r0mag)) * (z*S - 1.0) * chi;
    double gdot = 1.0 - (chi*chi/rmag) * C;
    vec3 v = v3_add(v3_scale(r0, fdot), v3_scale(v0, gdot));

    StateRV out = { r, v };
    return out;
}

/* ---- Frame transforms ---------------------------------------------------- */
vec3 astra_eci_to_ecef(vec3 r, double t, double theta0) {
    double th = theta0 + ASTRA_OMEGA_EARTH * t;
    double c = cos(th), s = sin(th);
    return (vec3){ c*r.x + s*r.y, -s*r.x + c*r.y, r.z };
}

vec3 astra_ecef_to_eci(vec3 r, double t, double theta0) {
    double th = theta0 + ASTRA_OMEGA_EARTH * t;
    double c = cos(th), s = sin(th);
    return (vec3){ c*r.x - s*r.y, s*r.x + c*r.y, r.z };
}

void astra_ecef_to_geodetic(vec3 r, double *lat, double *lon, double *rad) {
    *lon = atan2(r.y, r.x);
    double rho = hypot(r.x, r.y);
    *lat = atan2(r.z, rho);
    *rad = sqrt(r.x*r.x + r.y*r.y + r.z*r.z);
}

/* ---- Geometry: occlusion & elevation ------------------------------------- */
int astra_has_line_of_sight(vec3 a, vec3 b, double body_r, double clearance) {
    vec3 d = v3_sub(b, a);
    double dd = v3_dot(d, d);
    if (dd == 0.0) return v3_norm(a) > (body_r + clearance);
    double t = -v3_dot(a, d) / dd;
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    vec3 closest = v3_add(a, v3_scale(d, t));
    return v3_norm(closest) > (body_r + clearance);
}

double astra_elevation_angle(vec3 station, vec3 sat) {
    vec3 rho = v3_sub(sat, station);
    double rho_norm = v3_norm(rho);
    if (rho_norm == 0.0) return -M_PI/2.0;
    vec3 up = v3_scale(station, 1.0/v3_norm(station));
    double sin_el = clamp1(v3_dot(v3_scale(rho, 1.0/rho_norm), up));
    return asin(sin_el);
}
