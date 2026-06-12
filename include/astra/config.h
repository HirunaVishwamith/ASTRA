/* config.h — compile-time limits and physical constants for ASTRA-C.
 *
 * Values mirror the Python reference (main.py constants) exactly so the C port
 * can be verified numerically against it before the Python is removed.
 */
#ifndef ASTRA_CONFIG_H
#define ASTRA_CONFIG_H

#include <stdint.h>

/* ---- Capacity (drives every static buffer; tune for larger shells) ------- */
#define ASTRA_MAX_SATS        1024u
#define ASTRA_MAX_GROUND        64u
#define ASTRA_MAX_NODES       (ASTRA_MAX_SATS + ASTRA_MAX_GROUND)
#define ASTRA_MAX_DEGREE        32u    /* bounded fan-out per node            */
#define ASTRA_MAX_LINKS       (ASTRA_MAX_NODES * ASTRA_MAX_DEGREE)
#define ASTRA_MAX_PACKETS     65536u   /* packet pool size (power of two)     */
#define ASTRA_MAX_QUEUE        4096u    /* per-link outgoing queue cap         */
#define ASTRA_INVALID         0xFFFFFFFFu

/* ---- Default constellation (Starlink-like Walker-Delta) ------------------ */
#define ASTRA_NUM_PLANES         10u
#define ASTRA_NUM_SATS_PER_PLANE 10u
#define ASTRA_NUM_TOTAL_SATS     (ASTRA_NUM_PLANES * ASTRA_NUM_SATS_PER_PLANE)

/* ---- Physical constants (match Python reference) ------------------------- */
#define ASTRA_EARTH_RADIUS_KM   6378.137
#define ASTRA_MU_EARTH          398600.4418        /* km^3/s^2               */
#define ASTRA_OMEGA_EARTH       7.2921150e-5       /* rad/s, WGS-84          */
#define ASTRA_C_LIGHT_KMS       299792.458         /* km/s                   */
#define ASTRA_LEO_ALTITUDE_KM   550.0
#define ASTRA_SAT_SMA_KM        (ASTRA_EARTH_RADIUS_KM + ASTRA_LEO_ALTITUDE_KM)
#define ASTRA_SAT_ECC           0.001
#define ASTRA_INCLINATION_DEG   53.0

/* ---- Link / topology defaults ------------------------------------------- */
#define ASTRA_MAX_LINK_RANGE_KM   2500.0
#define ASTRA_BASE_BW_MBPS        2000.0
#define ASTRA_BASE_LOSS           0.0005
#define ASTRA_EXTRA_LATENCY_S     0.001

/* ---- Ground stations ---------------------------------------------------- */
#define ASTRA_GROUND_MIN_ELEV_DEG     10.0
#define ASTRA_GROUND_SAT_MAX_RANGE_KM 3000.0
#define ASTRA_GROUND_BW_MBPS          300.0
#define ASTRA_GROUND_BASE_LOSS        0.002
#define ASTRA_GROUND_REF_KM           800.0
#define ASTRA_ISL_REF_KM              500.0
#define ASTRA_GROUND_EXTRA_LATENCY_S  0.003

/* ---- Cache / alignment --------------------------------------------------- */
#define ASTRA_CACHELINE       64u
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  #define ASTRA_ALIGN(n) _Alignas(n)
#else
  #define ASTRA_ALIGN(n)
#endif

typedef uint32_t node_id;
typedef uint32_t link_id;
typedef uint32_t pkt_id;

#endif /* ASTRA_CONFIG_H */
