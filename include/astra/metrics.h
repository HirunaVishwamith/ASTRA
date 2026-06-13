/* metrics.h — per-step metrics. Port of metrics/collector.py. Route-update
 * counting uses a full next-hop diff (cleaner than Python's random sampling)
 * to drive convergence-time detection after a failure event. */
#ifndef ASTRA_METRICS_H
#define ASTRA_METRICS_H

#include "astra/routing.h"
#include "astra/traffic.h"
#include "astra/rng.h"

typedef struct {
    double time_s;
    double avg_path_len_hops;
    double delivery_ratio;
    double avg_delay_s;
    double avg_hops;
    double avg_link_util;
    uint32_t route_updates;
    double convergence_s;     /* <0 when not applicable */
} StepMetrics;

typedef struct {
    uint32_t sample_pairs;
    Rng      rng;
    int      has_prev;
    node_id  prev_nh[ASTRA_MAX_NODES * ASTRA_MAX_NODES];
    int      failure_set;  double failure_time_s;
    int      converged_set; double converged_at_s;
} MetricsCollector;

void astra_metrics_init(MetricsCollector *m, uint32_t sample_pairs, uint64_t seed);
void astra_metrics_notify_failure(MetricsCollector *m, double now_s);

StepMetrics astra_metrics_step(MetricsCollector *m, const Router *r,
                               const node_id *active_ids, uint32_t n,
                               const TrafficStats *ts, double now_s);

#endif /* ASTRA_METRICS_H */
