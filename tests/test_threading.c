/* test_threading.c — concurrency + strike-safety of the sim<->render boundary.
 * Runs the sim on its own free-running thread while this (render) thread pulls
 * snapshots and fires commands. Asserts the invariants the lock-free design is
 * supposed to guarantee:
 *   (A) frame_id is monotonically non-decreasing across acquired snapshots;
 *   (B) NO snapshot ever references a dead satellite as a link endpoint
 *       (proves strikes are atomic w.r.t. publish — no half-dead topology);
 *   (C) a struck satellite eventually reads alive==0 in a snapshot, and after
 *       CMD_REBOOT_ALL eventually reads alive==1 again;
 *   (D) the sim keeps advancing (step_count grows) — thread is actually live. */
#include "astra/sim_thread.h"
#include <stdio.h>
#include <time.h>

static SimState SIM;       /* large; keep off the stack */
static SimThread TH;

static void nap_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Spin until a snapshot newer than the last is available, or give up. */
static const RenderSnapshot *next_snapshot(SimState *s, int max_spins) {
    for (int i = 0; i < max_spins; ++i) {
        const RenderSnapshot *snap = astra_snapshot_acquire(s);
        if (snap) return snap;
        nap_ms(1);
    }
    return NULL;
}

int main(void) {
    int fails = 0;
    astra_sim_init(&SIM, 0xA57121u);
    if (!astra_sim_thread_start(&TH, &SIM, 0 /* free-run */)) {
        printf("thread start failed\n"); return 1;
    }
    const uint32_t num_sats = SIM.num_sats;
    const uint32_t victim   = 42;

    uint64_t last_frame = 0;
    uint32_t first_step = 0, last_step = 0;
    int got_first = 0;

    /* Phase 1: observe a healthy running sim. Check (A) and (B). */
    for (int k = 0; k < 200; ++k) {
        const RenderSnapshot *snap = next_snapshot(&SIM, 1000);
        if (!snap) { printf("no snapshot in phase 1\n"); fails++; break; }

        if (snap->frame_id < last_frame) {            /* (A) */
            printf("frame_id went backwards: %llu < %llu\n",
                   (unsigned long long)snap->frame_id,
                   (unsigned long long)last_frame); fails++;
        }
        last_frame = snap->frame_id;

        for (uint32_t e = 0; e < snap->link_count; ++e) {  /* (B) */
            node_id u = snap->link[e].u, v = snap->link[e].v;
            if (u < num_sats && !snap->sat[u].alive) {
                printf("link %u endpoint u=%u dead in snapshot\n", e, u); fails++; break;
            }
            if (v < num_sats && !snap->sat[v].alive) {
                printf("link %u endpoint v=%u dead in snapshot\n", e, v); fails++; break;
            }
        }
        if (!got_first) { first_step = snap->step_count; got_first = 1; }
        last_step = snap->step_count;
    }
    if (last_step <= first_step) {                    /* (D) */
        printf("sim did not advance: first=%u last=%u\n", first_step, last_step); fails++;
    }

    /* Phase 2: strike a satellite. Keep checking (B) throughout. */
    Command strike = { CMD_STRIKE, victim, 0.0 };
    while (!astra_cmd_push(&SIM, strike)) nap_ms(1);

    int saw_dead = 0;
    for (int k = 0; k < 500 && !saw_dead; ++k) {
        const RenderSnapshot *snap = next_snapshot(&SIM, 1000);
        if (!snap) break;
        for (uint32_t e = 0; e < snap->link_count; ++e) {   /* (B) still holds */
            node_id u = snap->link[e].u, v = snap->link[e].v;
            if ((u < num_sats && !snap->sat[u].alive) ||
                (v < num_sats && !snap->sat[v].alive)) {
                printf("dead endpoint after strike, link %u\n", e); fails++; break;
            }
        }
        if (!snap->sat[victim].alive) saw_dead = 1;          /* (C) */
        if (snap->frame_id < last_frame) { printf("frame_id backwards (p2)\n"); fails++; }
        last_frame = snap->frame_id;
    }
    if (!saw_dead) { printf("struck sat %u never observed dead\n", victim); fails++; }

    /* Phase 3: reboot. Victim must come back alive. */
    Command reboot = { CMD_REBOOT_ALL, 0, 0.0 };
    while (!astra_cmd_push(&SIM, reboot)) nap_ms(1);

    int saw_alive = 0;
    for (int k = 0; k < 500 && !saw_alive; ++k) {
        const RenderSnapshot *snap = next_snapshot(&SIM, 1000);
        if (!snap) break;
        if (snap->sat[victim].alive) saw_alive = 1;          /* (C) */
        if (snap->frame_id < last_frame) { printf("frame_id backwards (p3)\n"); fails++; }
        last_frame = snap->frame_id;
    }
    if (!saw_alive) { printf("struck sat %u never recovered after reboot\n", victim); fails++; }

    astra_sim_thread_stop(&TH);

    printf("frames_seen_upto=%llu final_step=%u saw_dead=%d saw_alive=%d\n",
           (unsigned long long)last_frame, last_step, saw_dead, saw_alive);
    printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
