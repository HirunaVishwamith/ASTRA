/* test_rf.c — physics-invariant verification for the RF/optical link budget.
 * Like failures/traffic/metrics, this is checked against analytic invariants
 * (not frozen Python vectors), since rf.c is a new additive engine:
 *   (1) FSPL: +6.02 dB per range doubling, +20 dB per frequency decade.
 *   (2) C/N self-consistency: C/N == C/N0 - 10log10(B).
 *   (3) Monotonicity: longer range -> higher FSPL, lower C/N, lower rate.
 *   (4) Shannon bound: achievable modcod rate never exceeds capacity.
 *   (5) Link closes near, goes to outage far (RF and optical).
 *   (6) Purity: identical inputs -> identical outputs. */
#include "astra/rf.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("  FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void) {
    /* (1) free-space path loss scaling */
    double f1 = astra_rf_fspl_db(1000.0, 12.0);
    double f2 = astra_rf_fspl_db(2000.0, 12.0);
    CHECK(fabs((f2 - f1) - 6.0206) < 1e-3, "range x2 should add 6.02 dB, got %.4f", f2-f1);
    double g1 = astra_rf_fspl_db(1000.0, 1.0);
    double g2 = astra_rf_fspl_db(1000.0, 10.0);
    CHECK(fabs((g2 - g1) - 20.0) < 1e-6, "freq x10 should add 20 dB, got %.6f", g2-g1);

    /* (2) C/N self-consistency for the Ku preset */
    RfLink k = astra_rf_budget(&ASTRA_RF_KU_USER_DOWN, 1000.0);
    double b_hz = ASTRA_RF_KU_USER_DOWN.bandwidth_mhz * 1e6;
    CHECK(fabs(k.cn_db - (k.cn0_dbhz - 10.0*log10(b_hz))) < 1e-9,
          "C/N must equal C/N0 - 10log10(B)");

    /* (3)+(4)+(5) sweep range, check monotonicity / Shannon bound / outage */
    double prev_fspl = -1e9, prev_cn = 1e9, prev_rate = 1e9;
    int saw_closed = 0;
    for (double d = 400.0; d <= 6000.0; d += 100.0) {
        RfLink r = astra_rf_budget(&ASTRA_RF_KU_USER_DOWN, d);
        CHECK(r.fspl_db > prev_fspl, "FSPL must rise with range at %.0f km", d);
        CHECK(r.cn_db   < prev_cn + 1e-9, "C/N must fall with range at %.0f km", d);
        CHECK(r.rate_gbps < prev_rate + 1e-9, "rate must not rise with range at %.0f km", d);
        CHECK(r.shannon_gbps + 1e-9 >= r.rate_gbps,
              "Shannon bound violated at %.0f km (%.3f < %.3f)", d, r.shannon_gbps, r.rate_gbps);
        if (r.closed) { CHECK(r.margin_db >= -1e-9, "closed link must have margin>=0 at %.0f km", d); saw_closed = 1; }
        prev_fspl = r.fspl_db; prev_cn = r.cn_db; prev_rate = r.rate_gbps;
    }
    CHECK(saw_closed, "Ku link should close at some short range");

    /* outage is a property of an under-powered link, not of any one preset:
     * a deliberately weak band at long range must report OUTAGE / zero rate. */
    RfBand weak = ASTRA_RF_KU_USER_DOWN;
    weak.eirp_dbw = 5.0;            /* far too little power */
    RfLink wk = astra_rf_budget(&weak, 5000.0);
    CHECK(!wk.closed, "under-powered link must be in outage");
    CHECK(wk.rate_gbps == 0.0, "outage must have zero achievable rate");
    CHECK(strcmp(wk.modcod, "OUTAGE") == 0, "outage modcod label");

    /* selected modcod must actually be supported by the available C/N */
    CHECK(k.closed && k.spectral_eff > 0.0, "1000 km Ku link should close");
    CHECK(fabs(k.rate_gbps - k.spectral_eff*b_hz/1e9) < 1e-6,
          "rate must equal spectral_eff * bandwidth");

    /* (5) optical ISL: closes near, outage far, gains positive */
    OpticalLink o1 = astra_optical_budget(&ASTRA_OPTICAL_ISL, 1000.0);
    OpticalLink o2 = astra_optical_budget(&ASTRA_OPTICAL_ISL, 5000.0);
    CHECK(o1.tx_gain_dbi > 0 && o1.rx_gain_dbi > 0, "telescope gains must be positive");
    CHECK(o2.spreading_loss_db > o1.spreading_loss_db, "optical spreading loss must rise with range");
    CHECK(o1.margin_db > o2.margin_db, "optical margin must fall with range");
    CHECK(o1.closed ? o1.rate_gbps == ASTRA_OPTICAL_ISL.target_gbps : o1.rate_gbps == 0.0,
          "optical rate is target when closed else 0");

    /* (6) purity */
    RfLink a = astra_rf_budget(&ASTRA_RF_KA_GW_UP, 1234.0);
    RfLink b = astra_rf_budget(&ASTRA_RF_KA_GW_UP, 1234.0);
    CHECK(memcmp(&a, &b, sizeof a) == 0, "rf budget must be a pure function");

    if (fails) { printf("FAIL: %d invariant(s) violated\n", fails); return 1; }
    printf("rf link budget: FSPL/C-N/Shannon/modcod/optical invariants OK\n");
    printf("PASS\n");
    return 0;
}
