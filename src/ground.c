/* ground.c — ground stations + ground<->sat links. Port of ground.py and the
 * ground-link loop in main.py:update_simulation. */
#include "astra/ground.h"
#include "astra/orbit.h"
#include "astra/config.h"
#include <string.h>
#include <math.h>

vec3 astra_gs_ecef(const GroundStation *gs, double earth_r) {
    double r = earth_r + gs->alt_km;
    double clat = cos(gs->lat_rad), slat = sin(gs->lat_rad);
    double clon = cos(gs->lon_rad), slon = sin(gs->lon_rad);
    return (vec3){ r*clat*clon, r*clat*slon, r*slat };
}

int astra_gs_sees_sat(vec3 gs_ecef, vec3 sat_ecef, double earth_r, double min_elev_deg) {
    /* Elevation >= mask is sufficient for a spherical Earth: above the local
     * horizon implies no Earth occlusion. (A separate ray-sphere test is
     * redundant and mis-fires for surface stations; see the matching note in
     * physics/orbit.py:is_visible_from_station_ecef.) */
    (void)earth_r;
    double el = astra_elevation_angle(gs_ecef, sat_ecef);
    return el >= min_elev_deg * M_PI / 180.0;
}

uint32_t astra_add_ground_links(NetworkGraph *g,
                                const GroundStation *gs, uint32_t ngs,
                                const vec3 *sat_eci, const node_id *sat_ids, uint32_t nsat,
                                double sim_time_s, double earth_r,
                                double max_range, node_id *best_sat) {
    uint32_t added = 0;
    for (uint32_t i = 0; i < ngs; ++i) {
        vec3 gse = astra_gs_ecef(&gs[i], earth_r);
        vec3 up  = v3_scale(gse, 1.0 / v3_norm(gse));
        node_id best = ASTRA_INVALID;
        double  best_el = -1e9;

        for (uint32_t s = 0; s < nsat; ++s) {
            node_id sid = sat_ids[s];
            vec3 se = astra_eci_to_ecef(sat_eci[sid], sim_time_s, 0.0);
            if (!astra_gs_sees_sat(gse, se, earth_r, gs[i].min_elev_deg)) continue;
            double d = v3_norm(v3_sub(se, gse));
            if (d > max_range) continue;

            vec3 rho = v3_sub(se, gse);
            double rho_norm = v3_norm(rho);
            double el_sin = v3_dot(v3_scale(rho, 1.0 / (rho_norm < 1e-9 ? 1e-9 : rho_norm)), up);
            if (el_sin > best_el) { best_el = el_sin; best = sid; }

            LinkProps p = astra_inverse_square_budget(d, ASTRA_GROUND_BW_MBPS,
                                                      ASTRA_GROUND_BASE_LOSS, ASTRA_GROUND_REF_KM);
            p.latency_s = d / ASTRA_C_LIGHT_KMS + ASTRA_GROUND_EXTRA_LATENCY_S;
            astra_graph_add(g, gs[i].gid, sid, d, p);
            added++;
        }
        if (best_sat) best_sat[i] = best;
    }
    return added;
}

uint32_t astra_default_ground_stations(GroundStation *gs, node_id first_gid) {
    struct { const char *name; double lat, lon; } S[] = {
        {"Sri Lanka",  7.8731,  80.7718},
        {"Singapore",  1.3521, 103.8198},
        {"London",    51.5074,  -0.1278},
        {"New York",  40.7128, -74.0060},
        {"Tokyo",     35.6762, 139.6503},
    };
    uint32_t n = (uint32_t)(sizeof(S) / sizeof(S[0]));
    for (uint32_t i = 0; i < n; ++i) {
        gs[i].gid = first_gid + i;
        strncpy(gs[i].name, S[i].name, sizeof(gs[i].name) - 1);
        gs[i].name[sizeof(gs[i].name) - 1] = '\0';
        gs[i].lat_rad = S[i].lat * M_PI / 180.0;
        gs[i].lon_rad = S[i].lon * M_PI / 180.0;
        gs[i].alt_km = 0.0;
        gs[i].min_elev_deg = ASTRA_GROUND_MIN_ELEV_DEG;
    }
    return n;
}
