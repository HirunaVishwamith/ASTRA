/* sim_thread.c — pthread driver for the simulation tick loop.
 * The sim thread is the sole writer of all simulation truth; it publishes
 * RenderSnapshots through the triple buffer and consumes commands from the SPSC
 * ring. realtime=1 paces to ~60 Hz wall-clock; realtime=0 free-runs (headless,
 * profiling). Stopping signals the atomic flag and joins. */
#include "astra/sim_thread.h"
#include <time.h>

#define ASTRA_FRAME_NS 16666667L   /* ~60 Hz */

static void sleep_ns(long ns) {
    struct timespec ts = { ns / 1000000000L, ns % 1000000000L };
    nanosleep(&ts, NULL);
}

static void *thread_main(void *arg) {
    SimThread *t = (SimThread *)arg;
    while (atomic_load_explicit(&t->running, memory_order_acquire)) {
        struct timespec t0;
        if (t->realtime) clock_gettime(CLOCK_MONOTONIC, &t0);

        astra_sim_tick(t->sim);

        if (t->realtime) {
            struct timespec t1;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long elapsed = (t1.tv_sec - t0.tv_sec) * 1000000000L
                         + (t1.tv_nsec - t0.tv_nsec);
            long rem = ASTRA_FRAME_NS - elapsed;
            if (rem > 0) sleep_ns(rem);
        }
    }
    return NULL;
}

int astra_sim_thread_start(SimThread *t, SimState *s, int realtime) {
    t->sim = s;
    t->realtime = realtime;
    atomic_store_explicit(&t->running, 1, memory_order_release);
    if (pthread_create(&t->thread, NULL, thread_main, t) != 0) {
        atomic_store_explicit(&t->running, 0, memory_order_release);
        return 0;
    }
    return 1;
}

void astra_sim_thread_stop(SimThread *t) {
    atomic_store_explicit(&t->running, 0, memory_order_release);
    pthread_join(t->thread, NULL);
}
