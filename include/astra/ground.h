/* ground.h — ground stations and ground<->satellite link construction.
 * Port of ground.py + the ground-link section of main.py:update_simulation. */
#ifndef ASTRA_GROUND_H
#define ASTRA_GROUND_H

#include "astra/graph.h"
#include "astra/vec3.h"

typedef struct {
    node_id gid;
    char    name[24];
    double  lat_rad, lon_rad, alt_km;
    double  min_elev_deg;
} GroundStation;

/* Fixed ECEF position of a station (spherical Earth). */
vec3 astra_gs_ecef(const GroundStation *gs, double earth_r_km);

/* Station-to-satellite visibility: elevation >= mask AND clear LOS. */
int astra_gs_sees_sat(vec3 gs_ecef, vec3 sat_ecef, double earth_r_km, double min_elev_deg);

/* Add ground<->satellite links into the graph for the current epoch.
 *  sat_eci[]  indexed by sat id; sat_ids[] lists active sats; nsat its length.
 *  best_sat[] (len ngs, may be NULL) receives the highest-elevation sat per
 *  station, or ASTRA_INVALID. Returns number of links added. */
uint32_t astra_add_ground_links(NetworkGraph *g,
                                 const GroundStation *gs, uint32_t ngs,
                                 const vec3 *sat_eci, const node_id *sat_ids, uint32_t nsat,
                                 double sim_time_s, double earth_r_km,
                                 double max_range_km, node_id *best_sat);

/* The default 5-station set from main.py (Sri Lanka, Singapore, London,
 * New York, Tokyo). Fills gs[0..4]; returns 5. gid = first_gid + i. */
uint32_t astra_default_ground_stations(GroundStation *gs, node_id first_gid);

#endif /* ASTRA_GROUND_H */
