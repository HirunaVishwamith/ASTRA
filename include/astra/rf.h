/* rf.h — physical RF / optical link-budget engine.
 *
 * An additive analysis layer: it does NOT touch the parity-verified
 * inverse-square topology budget in graph.c. Instead it characterises any link
 * the way a satcom RF systems engineer would on a datasheet — Friis free-space
 * path loss, kTB thermal noise, Shannon-Hartley capacity, and a DVB-S2-style
 * modcod ladder that picks the highest-order modulation+coding the link can
 * actually close, yielding a real achievable rate (Gbps) and link margin (dB).
 *
 * Two physical regimes:
 *   - RF      (Ku user downlink, Ka gateway uplink): EIRP / G-over-T / kTB.
 *   - Optical (inter-satellite laser links): telescope gains + spreading loss
 *             against a receiver sensitivity.
 *
 * All powers are dBW unless a field name says dBm; all ratios are dB. Numbers
 * in the presets are representative of a Starlink-class system but are
 * illustrative engineering figures, not official specifications. */
#ifndef ASTRA_RF_H
#define ASTRA_RF_H

#include <stdint.h>

/* ---- RF link (Friis + kTB + Shannon + modcod) --------------------------- */
typedef struct {
    double range_km;
    double fspl_db;        /* free-space path loss                            */
    double cn0_dbhz;       /* carrier-to-noise-density (C/N0)                  */
    double cn_db;          /* carrier-to-noise in the occupied bandwidth      */
    double ebn0_db;        /* energy-per-bit to noise density at the rate     */
    double shannon_gbps;   /* Shannon-Hartley capacity upper bound            */
    double rate_gbps;      /* achievable rate at the selected modcod          */
    double spectral_eff;   /* bits/s/Hz of the selected modcod                */
    double margin_db;      /* C/N above the modcod threshold (link margin)    */
    const char *modcod;    /* selected modulation+coding, or "OUTAGE"         */
    int    closed;         /* 1 if the link closes (a modcod is achievable)   */
} RfLink;

/* A directed RF link's transmission parameters (one band preset). */
typedef struct {
    const char *name;
    double freq_ghz;       /* carrier frequency                               */
    double eirp_dbw;       /* transmitter effective isotropic radiated power  */
    double gt_dbk;         /* receiver figure of merit G/T (dB/K)             */
    double bandwidth_mhz;  /* occupied channel bandwidth                      */
    double atmos_loss_db;  /* clear-sky gaseous/atmospheric absorption        */
    double rain_margin_db; /* rain + scintillation fade margin allocated      */
    double impl_loss_db;   /* antenna pointing + implementation losses        */
} RfBand;

/* ---- Optical inter-satellite laser link --------------------------------- */
typedef struct {
    double range_km;
    double tx_gain_dbi;        /* transmit telescope gain                     */
    double rx_gain_dbi;        /* receive telescope gain                      */
    double spreading_loss_db;  /* 20 log10(4 pi d / lambda)                   */
    double rx_power_dbm;       /* power at the detector                       */
    double margin_db;          /* rx_power - sensitivity                      */
    double rate_gbps;          /* target rate if the link closes, else 0      */
    int    closed;
} OpticalLink;

typedef struct {
    const char *name;
    double tx_power_w;         /* laser output power (W)                      */
    double wavelength_nm;      /* optical wavelength                          */
    double tx_aperture_m;      /* transmit telescope diameter                 */
    double rx_aperture_m;      /* receive telescope diameter                  */
    double optics_loss_db;     /* combined tx+rx optics efficiency loss       */
    double pointing_loss_db;   /* residual pointing/jitter loss               */
    double rx_sensitivity_dbm; /* required detector power for target_gbps     */
    double target_gbps;        /* design data rate                            */
} OpticalParams;

/* Free-space path loss in dB for a slant range (km) at a frequency (GHz). */
double astra_rf_fspl_db(double range_km, double freq_ghz);

/* Full RF / optical budgets for a given slant range. */
RfLink      astra_rf_budget(const RfBand *b, double range_km);
OpticalLink astra_optical_budget(const OpticalParams *o, double range_km);

/* Representative Starlink-class presets (illustrative, not official specs). */
extern const RfBand       ASTRA_RF_KU_USER_DOWN; /* sat -> user terminal (Ku) */
extern const RfBand       ASTRA_RF_KA_GW_UP;     /* gateway -> sat (Ka)       */
extern const OpticalParams ASTRA_OPTICAL_ISL;    /* sat <-> sat laser ISL     */

#endif /* ASTRA_RF_H */
