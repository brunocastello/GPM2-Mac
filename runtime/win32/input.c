/* Host -> core event queue. Thread-safe: the host (AppKit main thread)
 * pushes; the guest thread pops in user32.c and sleeps here when there is nothing to do. */
#include "win32.h"
#include "../host/host.h"

#include <pthread.h>
#include <string.h>
#ifndef GPM2_OS9
#include <sys/time.h>
#endif
#include <errno.h>

#define MAX_EV 512
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static HostEvent evq[MAX_EV];
static unsigned eh, et;

void host_push_event(const HostEvent *e) {
    pthread_mutex_lock(&mu);
    if (e->type == HOST_EV_MOUSEMOVE && eh != et && evq[(et + MAX_EV - 1u) % MAX_EV].type == HOST_EV_MOUSEMOVE)
        evq[(et + MAX_EV - 1u) % MAX_EV] = *e;                    /* coalesce consecutive moves */
    else if ((et + 1u) % MAX_EV != eh) { evq[et] = *e; et = (et + 1u) % MAX_EV; }
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}

int win32_input_pop(HostEvent *e) {
    int r = 0;
    pthread_mutex_lock(&mu);
    if (eh != et) { *e = evq[eh]; eh = (eh + 1u) % MAX_EV; r = 1; }
    pthread_mutex_unlock(&mu);
    return r;
}

#ifdef GPM2_OS9
/* Mac OS 9: no threads; waiting = pumping the host event loop for up to `ms` */
void win32_input_wait(uint32_t ms) { os9_pump(ms); }
#else
/* waits up to `ms` real milliseconds, or until an event arrives */
void win32_input_wait(uint32_t ms) {
    struct timespec ts;
    struct timeval tv;
    pthread_mutex_lock(&mu);
    if (eh == et && ms) {
        gettimeofday(&tv, NULL);
        ts.tv_sec = tv.tv_sec + (time_t)(ms / 1000u);
        ts.tv_nsec = (long)tv.tv_usec * 1000L + (long)(ms % 1000u) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&cv, &mu, &ts);
    }
    pthread_mutex_unlock(&mu);
}
#endif
