/* failures.c — port of network/failures.py FailureModel.apply. */
#include "astra/failures.h"
#include <math.h>

void astra_failures_apply(NetworkGraph *g, const LinkImpairments *imp,
                          double dt_s, Rng *rng) {
    double dt = dt_s < 0.0 ? 0.0 : dt_s;
    double p_black = 1.0 - pow(1.0 - imp->blackout_prob_per_s, dt);
    double p_spike = 1.0 - pow(1.0 - imp->latency_spike_prob_per_s, dt);

    for (uint32_t e = 0; e < g->link_count; ++e) {
        Link *L = &g->links[e];
        if (!L->up) continue;

        if (imp->blackout_prob_per_s > 0.0 && rng_double(rng) < p_black) {
            L->up = 0;                                   /* blackout: drop edge */
            continue;
        }
        double add_lat = (imp->latency_spike_prob_per_s > 0.0 &&
                          rng_double(rng) < p_spike) ? imp->latency_spike_add_s : 0.0;
        double loss = L->props.loss_prob * imp->loss_multiplier;
        if (loss < 0.0) loss = 0.0; else if (loss > 1.0) loss = 1.0;
        L->props.latency_s += add_lat;
        L->props.loss_prob  = (float)loss;
    }
}
