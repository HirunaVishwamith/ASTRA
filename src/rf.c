/* rf.c — physical RF / optical link-budget engine (see rf.h).
 *
 * Standard satcom link-budget arithmetic:
 *   FSPL(dB)  = 92.45 + 20 log10(d_km) + 20 log10(f_GHz)
 *   C/N0(dBHz)= EIRP - FSPL - L_atm - L_rain - L_impl + G/T - 10log10(k)
 *   C/N(dB)   = C/N0 - 10 log10(B_Hz)
 *   Shannon   = B * log2(1 + 10^(C/N / 10))            (capacity bound)
 * then pick the highest DVB-S2-style modcod whose required C/N is met to get
 * the achievable rate and the link margin.  Boltzmann's constant in log form is
 * 10 log10(1.380649e-23 J/K) = -228.60 dBW/K/Hz. */
#include "astra/rf.h"
#include <math.h>
#include <stddef.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BOLTZMANN_DBW_K_HZ (-228.5991)   /* 10*log10(1.380649e-23) */

double astra_rf_fspl_db(double range_km, double freq_ghz) {
    if (range_km < 1e-6) range_km = 1e-6;
    if (freq_ghz  < 1e-9) freq_ghz = 1e-9;
    return 92.45 + 20.0*log10(range_km) + 20.0*log10(freq_ghz);
}

/* DVB-S2-style modcod ladder: spectral efficiency (b/s/Hz) and the C/N (dB)
 * required to close it, ascending in efficiency. Thresholds are representative
 * of DVB-S2 AWGN performance. */
typedef struct { const char *name; double eta; double req_cn_db; } ModCod;
static const ModCod MODCODS[] = {
    { "QPSK 1/4",    0.490, -2.35 },
    { "QPSK 1/2",    0.989,  1.00 },
    { "QPSK 3/4",    1.485,  4.03 },
    { "8PSK 2/3",    1.980,  6.62 },
    { "8PSK 3/4",    2.228,  7.91 },
    { "16APSK 3/4",  2.967, 10.21 },
    { "16APSK 5/6",  3.297, 11.61 },
    { "32APSK 3/4",  3.703, 12.73 },
    { "32APSK 5/6",  4.119, 14.28 },
    { "32APSK 9/10", 4.453, 16.05 },
};
#define NMODCOD (sizeof(MODCODS)/sizeof(MODCODS[0]))

RfLink astra_rf_budget(const RfBand *b, double range_km) {
    RfLink r;
    double b_hz = b->bandwidth_mhz * 1e6;
    double fspl = astra_rf_fspl_db(range_km, b->freq_ghz);
    double losses = fspl + b->atmos_loss_db + b->rain_margin_db + b->impl_loss_db;

    double cn0 = b->eirp_dbw - losses + b->gt_dbk - BOLTZMANN_DBW_K_HZ;
    double cn  = cn0 - 10.0*log10(b_hz);
    double snr_lin = pow(10.0, cn/10.0);
    double shannon_bps = b_hz * log2(1.0 + snr_lin);

    /* highest modcod that closes */
    const ModCod *sel = NULL;
    for (size_t i = 0; i < NMODCOD; ++i)
        if (cn >= MODCODS[i].req_cn_db) sel = &MODCODS[i];

    double eta      = sel ? sel->eta : 0.0;
    double rate_bps = eta * b_hz;
    /* Eb/N0 at the achieved rate (fall back to bandwidth if in outage) */
    double rate_for_eb = rate_bps > 1.0 ? rate_bps : b_hz;

    r.range_km     = range_km;
    r.fspl_db      = fspl;
    r.cn0_dbhz     = cn0;
    r.cn_db        = cn;
    r.ebn0_db      = cn0 - 10.0*log10(rate_for_eb);
    r.shannon_gbps = shannon_bps / 1e9;
    r.rate_gbps    = rate_bps / 1e9;
    r.spectral_eff = eta;
    r.margin_db    = cn - (sel ? sel->req_cn_db : MODCODS[0].req_cn_db);
    r.modcod       = sel ? sel->name : "OUTAGE";
    r.closed       = sel ? 1 : 0;
    return r;
}

OpticalLink astra_optical_budget(const OpticalParams *o, double range_km) {
    OpticalLink r;
    double d_m    = (range_km < 1e-6 ? 1e-6 : range_km) * 1000.0;
    double lambda = o->wavelength_nm * 1e-9;

    /* circular-aperture telescope gain: G = (pi D / lambda)^2 */
    double gt = pow(M_PI * o->tx_aperture_m / lambda, 2.0);
    double gr = pow(M_PI * o->rx_aperture_m / lambda, 2.0);
    double gt_db = 10.0*log10(gt);
    double gr_db = 10.0*log10(gr);
    double spreading = 20.0*log10(4.0*M_PI*d_m / lambda);

    double ptx_dbw = 10.0*log10(o->tx_power_w);
    double prx_dbw = ptx_dbw + gt_db + gr_db - spreading
                   - o->optics_loss_db - o->pointing_loss_db;
    double prx_dbm = prx_dbw + 30.0;
    double margin  = prx_dbm - o->rx_sensitivity_dbm;

    r.range_km          = range_km;
    r.tx_gain_dbi       = gt_db;
    r.rx_gain_dbi       = gr_db;
    r.spreading_loss_db = spreading;
    r.rx_power_dbm      = prx_dbm;
    r.margin_db         = margin;
    r.closed            = margin >= 0.0;
    r.rate_gbps         = (margin >= 0.0) ? o->target_gbps : 0.0;
    return r;
}

/* ---- representative Starlink-class presets (illustrative) --------------- */

/* Ku-band sat -> user-terminal downlink: a high-gain phased-array spot beam to
 * a ~0.5 m class user terminal over ~240 MHz. */
const RfBand ASTRA_RF_KU_USER_DOWN = {
    .name          = "Ku user downlink",
    .freq_ghz      = 11.7,
    .eirp_dbw      = 38.0,
    .gt_dbk        = 13.0,
    .bandwidth_mhz = 240.0,
    .atmos_loss_db = 0.3,
    .rain_margin_db= 3.0,
    .impl_loss_db  = 2.0,
};

/* Ka-band gateway -> sat uplink: a large gateway antenna to the satellite
 * receive G/T over ~500 MHz. */
const RfBand ASTRA_RF_KA_GW_UP = {
    .name          = "Ka gateway uplink",
    .freq_ghz      = 28.5,
    .eirp_dbw      = 62.0,
    .gt_dbk        = 9.0,
    .bandwidth_mhz = 500.0,
    .atmos_loss_db = 0.8,
    .rain_margin_db= 6.0,
    .impl_loss_db  = 2.5,
};

/* Optical inter-satellite laser link: ~1550 nm, small telescopes, ~100 Gbps. */
const OpticalParams ASTRA_OPTICAL_ISL = {
    .name               = "Laser ISL",
    .tx_power_w         = 2.0,
    .wavelength_nm      = 1550.0,
    .tx_aperture_m      = 0.08,
    .rx_aperture_m      = 0.08,
    .optics_loss_db     = 5.0,
    .pointing_loss_db   = 3.0,
    .rx_sensitivity_dbm = -38.0,
    .target_gbps        = 100.0,
};
