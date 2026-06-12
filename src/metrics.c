/* metrics.c — port of metrics/collector.py. */
#include "astra/metrics.h"
#include <string.h>

void astra_metrics_init(MetricsCollector *m, uint32_t sample_pairs, uint64_t seed) {
    memset(m, 0, sizeof(*m));
    m->sample_pairs = sample_pairs;
    rng_seed(&m->rng, seed, 0x1234u);
    m->failure_set = 0; m->converged_set = 0; m->has_prev = 0;
}

void astra_metrics_notify_failure(MetricsCollector *m, double now_s) {
    m->failure_set = 1; m->failure_time_s = now_s;
    m->converged_set = 0;
}

/* count next-hop entries that changed since last step (route churn) */
static uint32_t route_update_count(MetricsCollector *m, const Router *r, uint32_t n) {
    uint32_t changed = 0;
    for (uint32_t src = 0; src < n; ++src)
        for (uint32_t dst = 0; dst < n; ++dst) {
            node_id nh = astra_next_hop(r, src, dst);
            size_t idx = (size_t)src * n + dst;
            if (m->has_prev && m->prev_nh[idx] != nh) changed++;
            m->prev_nh[idx] = nh;
        }
    int first = !m->has_prev;
    m->has_prev = 1;
    return first ? 0u : changed;
}

/* mean hop count over sampled src/dst pairs, following next-hop pointers */
static double avg_path_length(MetricsCollector *m, const Router *r,
                              const node_id *act, uint32_t n) {
    if (n < 2) return 0.0;
    uint32_t pairs = m->sample_pairs;
    uint32_t maxp = n * (n - 1) / 2;
    if (pairs > maxp) pairs = maxp;
    if (pairs == 0) return 0.0;
    uint64_t total = 0; uint32_t count = 0;
    for (uint32_t p = 0; p < pairs; ++p) {
        uint32_t ia = rng_below(&m->rng, n), ib = rng_below(&m->rng, n);
        while (ib == ia) ib = rng_below(&m->rng, n);
        node_id a = act[ia], b = act[ib];
        node_id cur = a; uint32_t hops = 0; int ok = 0;
        for (uint32_t h = 0; h < n; ++h) {
            if (cur == b) { ok = 1; break; }
            node_id nh = astra_next_hop(r, cur, b);
            if (nh == ASTRA_INVALID) break;
            cur = nh; hops++;
        }
        if (ok) { total += hops; count++; }
    }
    return count ? (double)total / (double)count : 0.0;
}

StepMetrics astra_metrics_step(MetricsCollector *m, const Router *r,
                               const node_id *act, uint32_t n,
                               const TrafficStats *ts, double now_s) {
    StepMetrics sm;
    sm.time_s = now_s;
    sm.route_updates = route_update_count(m, r, n);
    sm.avg_path_len_hops = avg_path_length(m, r, act, n);
    sm.delivery_ratio = astra_delivery_ratio(ts);
    sm.avg_delay_s = ts->delivered ? ts->sum_delay_s / (double)ts->delivered : 0.0;
    sm.avg_hops    = ts->delivered ? ts->sum_hops    / (double)ts->delivered : 0.0;
    sm.avg_link_util = ts->last_link_util;

    /* convergence: first step with zero route churn after a failure event */
    sm.convergence_s = -1.0;
    if (m->failure_set) {
        if (!m->converged_set && sm.route_updates == 0) {
            m->converged_set = 1; m->converged_at_s = now_s;
        }
        if (m->converged_set) sm.convergence_s = m->converged_at_s - m->failure_time_s;
    }
    return sm;
}
