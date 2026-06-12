/* orbit.h — two-body orbital mechanics (ECI). Direct port of physics/orbit.py.
 *
 * Units: distance km, velocity km/s, time s, angles rad. */
#ifndef ASTRA_ORBIT_H
#define ASTRA_ORBIT_H

#include "astra/vec3.h"

/* Classical orbital elements. */
typedef struct {
    double a;     /* semi-major axis (km)      */
    double e;     /* eccentricity              */
    double i;     /* inclination (rad)         */
    double raan;  /* RAAN (rad)                */
    double argp;  /* argument of perigee (rad) */
    double nu;    /* true anomaly (rad)        */
} OrbitElements;

typedef struct { vec3 r; vec3 v; } StateRV;

/* COE <-> Cartesian (perifocal -> ECI via R3(raan) R1(i) R3(argp)). */
StateRV       astra_coe_to_rv(double mu, OrbitElements coe);
OrbitElements astra_rv_to_coe(double mu, vec3 r, vec3 v);

/* Two-body universal-variable propagation (Stumpff C/S, Newton iteration). */
StateRV astra_propagate_kepler(double mu, vec3 r0, vec3 v0, double dt_s);

/* ECI <-> ECEF simple R3 rotation by theta0 + omega_earth * t. */
vec3 astra_eci_to_ecef(vec3 r_eci, double t_since_epoch_s, double theta0_rad);
vec3 astra_ecef_to_eci(vec3 r_ecef, double t_since_epoch_s, double theta0_rad);

/* Spherical geodetic from ECEF: returns lat, lon (rad) and radius (km). */
void astra_ecef_to_geodetic(vec3 r_ecef, double *lat_rad, double *lon_rad, double *r_km);

/* Segment a->b clears a sphere of radius body_r (+clearance)?  Earth occlusion. */
int astra_has_line_of_sight(vec3 a, vec3 b, double body_r_km, double clearance_km);

/* Elevation angle (rad) of sat above station's local horizon (spherical). */
double astra_elevation_angle(vec3 station_ecef, vec3 sat_ecef);

#endif /* ASTRA_ORBIT_H */
