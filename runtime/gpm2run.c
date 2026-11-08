/* Runtime initialization: runs the recompiled Gpm2.exe from the original entry point.
 * The mains live in headless_main.c (command line) and host/macos/ (GPM2.app). */
#include "gpm2run.h"
#include "win32/win32.h"
#include "host/host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENTRY_VA 0x00539D08u
#define RUN_SENTINEL 0x00F00000u

int opt_fixed_time_set;
uint32_t opt_fixed_time;
int opt_idle_exit = 200;
const char *opt_exe_path;

#ifdef GPM2_OS9
int os9_run_guest(void *(*fn)(void *), unsigned stack_mb);   /* host/os9/host_os9.c */
#endif
static void *run_guest(void *unused) {
    (void)unused;
    g.esp = STACK_TOP - 0x100u;
    W32(g.esp, RUN_SENTINEL);
    shadow_push(RUN_SENTINEL);
    win32_log("[boot] calling the entry point %08x", (unsigned)ENTRY_VA);
    dispatch(ENTRY_VA);
    win32_log("the entry point returned (eax=%08x)", g.eax);
    win32_summary();
    exit((int)g.eax);
    return NULL;
}

int gpm2_start(const GpmConfig *cfg, pthread_t *th) {
    pthread_attr_t at;
    opt_exe_path = cfg->exe;
    machine_init();
    if (image_load(cfg->exe)) { fprintf(stderr, "failed to load %s\n", cfg->exe); return 2; }
    W32(FS_BASE + 0, 0xFFFFFFFFu); W32(FS_BASE + 4, STACK_TOP); W32(FS_BASE + 8, STACK_LOW);
    W32(FS_BASE + 0x18, FS_BASE);
    heap_init();
    vfs_init(cfg->game, cfg->wdir, cfg->cd);
    if (res_load()) { fprintf(stderr, "exe has no .rsrc\n"); return 2; }
    msvcrt_register(); kernel32_register(); user32_register(); menu_register(); gdi32_register();
    wing32_register(); winmm_register(); misc_register(); dplay_register(); dialog_register(); filedlg_register(); audio_init();
    win32_init();
    fprintf(stderr, "[boot] image loaded, imports registered\n");
    if (cfg->script) host_set_script(cfg->script);
    host_init(cfg->out);
#ifdef GPM2_OS9
    (void)at; (void)th;                                        /* Mac OS 9: cooperative Thread Manager thread (or the main one, without it) */
    return os9_run_guest(run_guest, cfg->stack_mb);
#else
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, (size_t)cfg->stack_mb << 20);
    if (pthread_create(th, &at, run_guest, NULL) != 0) { fprintf(stderr, "pthread_create failed\n"); return 2; }
    return 0;
#endif
}
