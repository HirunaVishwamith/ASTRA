/* sim_thread.h — runs the simulation tick loop on its own thread.
 * realtime=1 paces to ~60 Hz wall-clock (GUI); realtime=0 free-runs as fast as
 * possible (headless / profiling). The render thread only ever touches the sim
 * through astra_snapshot_acquire() and astra_cmd_push(). */
#ifndef ASTRA_SIM_THREAD_H
#define ASTRA_SIM_THREAD_H

#include "astra/sim.h"
#include <pthread.h>
#include <stdatomic.h>

typedef struct {
    SimState       *sim;
    pthread_t       thread;
    _Atomic int     running;
    int             realtime;
} SimThread;

int  astra_sim_thread_start(SimThread *t, SimState *s, int realtime);
void astra_sim_thread_stop(SimThread *t);   /* signals + joins */

#endif /* ASTRA_SIM_THREAD_H */
