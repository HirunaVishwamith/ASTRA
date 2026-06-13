/* astra_linkbudget — print a satcom link-budget datasheet for the ASTRA link
 * presets (Ku user downlink, Ka gateway uplink, optical laser ISL).
 *
 *   astra_linkbudget                 full datasheet + range sweep
 *   astra_linkbudget --range 1200    detailed budget at a single slant range
 *
 * Uses the physical RF/optical engine in src/rf.c (Friis + kTB + Shannon +
 * DVB-S2 modcod selection). Numbers are representative, not official specs. */
#include "astra/rf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void rf_detail(const RfBand *b, double d) {
    RfLink r = astra_rf_budget(b, d);
    printf("  %-20s  f=%.1f GHz  B=%.0f MHz\n", b->name, b->freq_ghz, b->bandwidth_mhz);
    printf("    EIRP            %8.2f dBW\n", b->eirp_dbw);
    printf("    G/T             %8.2f dB/K\n", b->gt_dbk);
    printf("    FSPL @ %.0f km   %8.2f dB\n", d, r.fspl_db);
    printf("    atmos+rain+impl %8.2f dB\n", b->atmos_loss_db + b->rain_margin_db + b->impl_loss_db);
    printf("    C/N0            %8.2f dB-Hz\n", r.cn0_dbhz);
    printf("    C/N             %8.2f dB\n", r.cn_db);
    printf("    Eb/N0           %8.2f dB\n", r.ebn0_db);
    printf("    Shannon cap.    %8.2f Gbps\n", r.shannon_gbps);
    printf("    modcod          %8s  (%.2f b/s/Hz)\n", r.modcod, r.spectral_eff);
    printf("    achievable rate %8.2f Gbps\n", r.rate_gbps);
    printf("    link margin     %8.2f dB   [%s]\n\n", r.margin_db, r.closed ? "CLOSED" : "OUTAGE");
}

static void opt_detail(const OpticalParams *o, double d) {
    OpticalLink r = astra_optical_budget(o, d);
    printf("  %-20s  lambda=%.0f nm  Ptx=%.1f W\n", o->name, o->wavelength_nm, o->tx_power_w);
    printf("    Tx telescope    %8.2f dBi\n", r.tx_gain_dbi);
    printf("    Rx telescope    %8.2f dBi\n", r.rx_gain_dbi);
    printf("    spreading loss  %8.2f dB\n", r.spreading_loss_db);
    printf("    optics+pointing %8.2f dB\n", o->optics_loss_db + o->pointing_loss_db);
    printf("    Rx power        %8.2f dBm  (sens %.1f dBm)\n", r.rx_power_dbm, o->rx_sensitivity_dbm);
    printf("    link margin     %8.2f dB   [%s]\n", r.margin_db, r.closed ? "CLOSED" : "OUTAGE");
    printf("    data rate       %8.2f Gbps\n\n", r.rate_gbps);
}

int main(int argc, char **argv) {
    double range = -1.0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--range") && i+1 < argc) range = strtod(argv[++i], 0);
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }

    printf("=== ASTRA link-budget datasheet (representative figures) ===\n\n");

    if (range > 0.0) {
        rf_detail(&ASTRA_RF_KU_USER_DOWN, range);
        rf_detail(&ASTRA_RF_KA_GW_UP, range);
        opt_detail(&ASTRA_OPTICAL_ISL, range);
        return 0;
    }

    /* default reference ranges per link type */
    rf_detail(&ASTRA_RF_KU_USER_DOWN, 700.0);
    rf_detail(&ASTRA_RF_KA_GW_UP, 900.0);
    opt_detail(&ASTRA_OPTICAL_ISL, 2500.0);

    printf("Range sweep (achievable rate, Gbps / link margin, dB):\n");
    printf("  %-9s  %-22s  %-22s  %-22s\n", "range_km",
           "Ku user down", "Ka gateway up", "optical ISL");
    for (double d = 500.0; d <= 5500.0; d += 500.0) {
        RfLink ku = astra_rf_budget(&ASTRA_RF_KU_USER_DOWN, d);
        RfLink ka = astra_rf_budget(&ASTRA_RF_KA_GW_UP, d);
        OpticalLink op = astra_optical_budget(&ASTRA_OPTICAL_ISL, d);
        printf("  %-9.0f  %7.2f / %-12.1f  %7.2f / %-12.1f  %7.2f / %-12.1f\n", d,
               ku.rate_gbps, ku.margin_db,
               ka.rate_gbps, ka.margin_db,
               op.rate_gbps, op.margin_db);
    }
    return 0;
}
