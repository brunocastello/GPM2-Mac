/* Runtime initialization, shared by the mains (headless_main.c e host/macos/). */
#ifndef GPM2_RUN_H
#define GPM2_RUN_H

#include <pthread.h>

typedef struct {
    const char *game, *exe, *cd, *wdir, *out, *script;
    unsigned stack_mb;
} GpmConfig;

/* Loads the image, registers the imports, starts the host and creates the guest thread (does not wait for it).
 * Returns 0 on success. */
int gpm2_start(const GpmConfig *cfg, pthread_t *th);

#endif
