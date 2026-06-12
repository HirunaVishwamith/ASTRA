/* failures.h — link impairment / failure model. Port of network/failures.py.
 * Operates in place on the graph's edge list: blacked-out links get up=0
 * (skipped by CSR + treated as broken by traffic), survivors get latency
 * spikes and a loss multiplier. */
#ifndef ASTRA_FAILURES_H
#define ASTRA_FAILURES_H

#include "astra/graph.h"
#include "astra/rng.h"

typedef struct {
    double blackout_prob_per_s;
    double latency_spike_prob_per_s;
    double latency_spike_add_s;
    double loss_multiplier;
} LinkImpairments;

/* Apply one step of impairments. dt_s converts per-second probabilities to
 * per-step Bernoulli trials, exactly as the Python model. */
void astra_failures_apply(NetworkGraph *g, const LinkImpairments *imp,
                          double dt_s, Rng *rng);

static inline LinkImpairments astra_impair_none(void) {
    LinkImpairments i = {0.0, 0.0, 0.05, 1.0};
    return i;
}

#endif /* ASTRA_FAILURES_H */
