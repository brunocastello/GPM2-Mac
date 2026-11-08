/* Portable core of the Win32 runtime. C99, nothing platform-specific.
 *
 * An import is `uint32_t fn(void)`: it reads the arguments with ARG(i) (stdcall/cdecl: the first
 * is at esp+4) and returns the result (goes to eax; the import itself sets edx and ST0).
 * The dispatcher in core.c pops the `ret` (4 + pop bytes) and checks the shadow stack.
 */
#ifndef GPM2_WIN32_H
#define GPM2_WIN32_H

#include "machine.h"
#include "../host/host.h"

#include <stdarg.h>
#include <stddef.h>

typedef uint32_t (*Win32Fn)(void);

#define ARG(i)   R32(g.esp + 4u + 4u * (uint32_t)(i))

/* ---- core.c ---------------------------------------------------------------------- */
void win32_register(const char *name, Win32Fn fn);       /* "USER32.dll!Name" */
void win32_init(void);                                     /* binds the generated table to the implementations */
NORETURN void win32_die(const char *fmt, ...);
void win32_log(const char *fmt, ...);                      /* stderr, with prefix */
void win32_summary(void);                                  /* import counts */
extern int win32_slow_trace;                               /* [slow] imports > 20 ms in the log */
extern int win32_trace;                                    /* GPM2_TRACE=1 */
extern int win32_stub_log;                                 /* logs the 1st call of each stub */
void win32_stub_note(const char *name);                    /* records stub use */
const char *win32_cur_import(void);
unsigned win32_cur_idx(void);

/* host C function callable by the guest (COM vtable): returns the guest address; `pop` = argument bytes (stdcall) */
uint32_t win32_thunk(Win32Fn fn, unsigned pop);

/* host -> guest call (WndProc, DlgProc, TimerProc, qsort, _initterm) */
uint32_t guest_call(uint32_t addr, unsigned n, ...);        /* stdcall: the callee pops the args */
uint32_t guest_call_cdecl(uint32_t addr, unsigned n, ...);  /* cdecl: the caller pops */

/* guest strings and buffers */
uint8_t *guest_ptr(uint32_t addr, uint32_t len);            /* checked; aborts outside memory */
uint32_t gstr_len(uint32_t addr);
void gstr_get(char *dst, size_t cap, uint32_t addr);        /* copies (truncates, 0-terminated) */
void gstr_put(uint32_t addr, const char *s);                /* writes with NUL */
void gstr_putn(uint32_t addr, uint32_t cap, const char *s); /* truncates at cap-1; returns nothing */
void gmem_get(void *dst, uint32_t addr, uint32_t n);
void gmem_put(uint32_t addr, const void *src, uint32_t n);
int gstr_cmp_ci(const char *a, const char *b);

int gformat(char *out, size_t cap, const char *fmt, uint32_t argp);   /* msvcrt.c */
void win32_exit(int code);

/* ---- heap.c ------------------------------------------------------------------------ */
#define HEAP_BASE 0x01000000u
void heap_init(void);
uint32_t heap_alloc(uint32_t n);
void heap_free(uint32_t p);
uint32_t heap_realloc(uint32_t p, uint32_t n);
uint32_t heap_size(uint32_t p);
void heap_stats(uint32_t *used, uint32_t *nused, uint32_t *maxfree);
void gdi_stats(unsigned cnt[8], unsigned long long *bm_bytes);
void win32_mem_report(const char *why);

/* ---- vfs.c ---------------------------------------------------------------------------- */
void vfs_init(const char *game_dir, const char *write_dir, const char *cd_dir);
/* DOS/Win32 path -> host path; `write` = open for writing (writable layer).
 * Returns 0 if it fits in `out`. */
int vfs_resolve(const char *dos, int write, char *out, size_t cap);
int vfs_host_protected(const char *host);                  /* inside original/ or the game folder: never write */
int vfs_host_to_dos(const char *host, char *dos, size_t cap);   /* C:\\GPM2\\... or Z:\\... (synthetic drive) */
int vfs_cwd_host(char *out, size_t cap);                   /* cwd folder in the writable layer (host) */
const char *vfs_cwd(void);                                  /* current directory (DOS style) */
void vfs_set_cwd(const char *dos);
#include <stdio.h>
size_t fread_yield(void *buf, size_t n, FILE *f);          /* fread in chunks, yielding the CPU to the host (OS 9) */
int vfs_stat_exists(const char *dos);
int vfs_list(const char *dos_pattern, char names[][64], int max);

/* ---- resources.c ------------------------------------------------------------------------ */
int res_load(void);                                         /* reads the exe's .rsrc into guest memory */
/* returns the guest address and size of the resource (numeric type/id); 0 = not found */
uint32_t res_find(uint32_t type, uint32_t id, uint32_t *size);
uint32_t res_find_name(uint32_t type, const char *name, uint32_t *size);   /* named resource */
int res_string(uint32_t id, char *out, size_t cap);         /* string table; -1 if it does not exist */

/* ---- input.c (host -> core queue) -------------------------------------------------------------- */
int win32_input_pop(HostEvent *e);
void win32_input_wait(uint32_t ms);                        /* sleeps up to `ms` real ms or until there is an event */

/* ---- menu.c ----------------------------------------------------------------------- */
void menu_register(void);
void menu_window_created(uint32_t hwnd, uint32_t hmenu);
void menu_window_destroyed(uint32_t hwnd);
int menu_has_bar(uint32_t hwnd);                           /* the window has a Win32 menu bar (subtracts 19 px) */
void menu_sync(void);                                      /* resends the menus to the host if they changed */

/* ---- user32.c --------------------------------------------------------------------------------- */
uint32_t user_main_window(void);
void user_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
/* Internal API for controls.c / dialog.c */
typedef struct { uint32_t style, exstyle, parent, id, proc; int x, y, w, h, cw, ch, visible, enabled, top; } UWin;
int user_wininfo(uint32_t h, UWin *o);                      /* 0 if the window does not exist */
const char *user_wintitle(uint32_t h);
void user_set_wintitle(uint32_t h, const char *t);
uint32_t user_send(uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp);
uint32_t user_create(const char *cls, const char *title, uint32_t style, uint32_t exstyle, int x, int y, int w, int h,
                     uint32_t parent, uint32_t id, uint32_t hinst);
void user_destroy(uint32_t h);
int user_children(uint32_t parent, uint32_t *out, int max);  /* in creation order */
uint32_t user_find_child(uint32_t parent, uint32_t id);
void user_set_enabled(uint32_t h, int en);
void user_set_visible(uint32_t h, int v);                     /* state only; no messages */
void user_move(uint32_t h, int x, int y);
uint32_t user_focus(void);
void user_set_focus(uint32_t h);                              /* WM_KILLFOCUS/WM_SETFOCUS */
void user_activate(uint32_t h);
uint32_t user_defproc(uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp);
int user_keydown(uint32_t vk);                                /* the key is pressed (GetKeyState) */
void user_modal_step(uint32_t dlg);                           /* one step of a modal dialog's loop */

/* ---- controls.c: EDIT, LISTBOX, COMBOBOX, BUTTON, STATIC in the core ---------------------------- */
enum { CK_BUTTON = 0, CK_EDIT, CK_STATIC, CK_LISTBOX, CK_COMBO };
uint32_t ctl_proc(int kind, uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp);
int ctl_list_add_text(uint32_t h, const char *s);             /* LB_ADDSTRING with host text */
int dlg_chat_capture(uint32_t h, uint32_t msg, uint32_t lp, uint32_t *ret);   /* BUG-025 */
void dlg_chat_command(int cmd);                               /* Chat menu */
void dlg_chat_center(uint32_t h);                             /* chat panel: center */
int dlg_bring_front(uint32_t h);                              /* chat panel: BringWindowToTop */
void ctl_touch(uint32_t h);                                   /* marks for resending to the host (visible/enabled/text...) */
int ctl_fill_host(uint32_t h, HostCtl *out, const char ***items_storage);   /* control state for the host; 0 if it is not a control */
uint32_t ctl_next_dirty(uint32_t after);                      /* next dirty control (HWND) or 0 */
void ctl_clear_dirty(uint32_t h);
/* user actions coming from the host (apply to the model and notify the parent) */
void ctl_user_click(uint32_t h);
void ctl_user_text(uint32_t h, const char *text);
void ctl_user_select(uint32_t h, int index, int dbl);
int ctl_kind(uint32_t h);                                     /* CK_* or -1 */
void ctl_draw_children(uint32_t parent, uint32_t hbm);       /* draws the children of a window that is not a dialog */
int ctl_wants_enter(uint32_t h);                              /* multiline EDIT: Enter belongs to the control */
int ctl_is_button(uint32_t h, int *defpush);

/* ---- dialog.c ----------------------------------------------------------------------------- */
void dialog_register(void);
uint32_t dlg_debug_open(const char *name);                    /* test: opens a template without a DlgProc (script dlg-open) */
uint32_t dlg_wndproc(uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp);
void dlg_host_event(const HostEvent *e);                      /* HOST_EV_DLG */
const char *user_title(uint32_t h);
void user_invalidate(uint32_t h);
#define HWND_BASE 0x00002000u /* fits in 16 bits: the game compares LOWORD(lParam) with the HWND (chat Send button) */
void dlg_window_destroying(uint32_t h);
void dlg_adopt(uint32_t h, const char *title, int cw, int ch);
int ctl_is_edit(uint32_t h);
void dlg_window_destroyed(uint32_t h);
void dlg_paint_text(uint32_t h, const char *t, int x, int y, int w, int he); /* DrawTextA in a native dialog becomes a STATIC of the panel */
void dlg_sync(void);                                          /* resends dirty controls to the host panels */
int dlg_key(uint32_t dlg, uint32_t target, uint32_t vk);      /* IsDialogMessage: 1 = handled (Tab/Enter/Esc) */
void dlg_abort(uint32_t dlg);

void ini_flush_due(uint32_t now_ms);                          /* kernel32.c: writes the cached .INI after 1 s */

/* ---- filedlg.c ------------------------------------------------------------------------------ */
void filedlg_register(void);

/* ---- audio.c: portable mixer 44100 Hz/16-bit/stereo + sndPlaySound/MCI ------------------------ */
typedef struct AudioStream AudioStream;
typedef struct { int16_t *pcm; uint32_t frames; AudioStream *st; } Pcm;   /* interleaved stereo, 44100 Hz; st != 0: read on demand from the file */
#define AUDIO_RATE 44100
int wav_to_pcm(const uint8_t *d, size_t n, Pcm *out);         /* RIFF/WAVE PCM -> 44100/16/stereo; 0 = ok */
int audio_stream_open(FILE *f, long size, Pcm *out);          /* long WAV by streaming; 0 = ok and takes ownership of f */
void audio_init(void);
void audio_pump(uint32_t now_ms);                             /* guest thread: virtual clock without a backend */
uint32_t audio_ms_until_event(void);                          /* ms until the next end of a sound (0xFFFFFFFF = none) */
int audio_voice_load(int slot, Pcm pcm, const char *name);     /* takes the PCM; stops and replaces the slot's sound */
int audio_voice_play(int slot, uint32_t from_ms, uint32_t to_ms, int loop);   /* from 0xFFFFFFFF = continue; to 0 = to the end */
void audio_voice_release(int slot);
void audio_set_dump(const char *path);                         /* headless: writes what would be played (WAV) */
void mci_poll(void);                                           /* winmm.c: posts MM_MCINOTIFY for the sounds that finished */
void audio_voice_stop(int slot);
void audio_voice_pause(int slot, int pause);
int audio_voice_state(int slot);                              /* 0 stopped, 1 playing, 2 paused */
int audio_voice_take_done(int slot);                          /* 1 if it finished on its own since the last call */
uint32_t audio_voice_pos_ms(int slot);
uint32_t audio_voice_len_ms(int slot);
void audio_voice_seek(int slot, uint32_t ms);
void audio_mix_int(int16_t *out, int nframes);                /* mixes (shared by the backend and the virtual clock) */
enum { AU_SND = 0, AU_MCI0 = 1, AU_NMCI = 8, AU_NSLOT = 9 };
int snd_play(const char *name, uint32_t flags);               /* winmm.c */

/* ---- host/host.h ------------------------------------------------------------------------- */
/* (included separately) */

/* ---- start of each module --------------------------------------------------------------- */
void msvcrt_register(void);
void kernel32_register(void);
void user32_register(void);
void gdi32_register(void);
void wing32_register(void);
void winmm_register(void);
void misc_register(void);
void dplay_register(void);

/* global options (gpm2run.c) */
extern int opt_fixed_time_set;
extern uint32_t opt_fixed_time;
extern int opt_idle_exit;
extern const char *opt_exe_path;

/* virtual time (ms since start); advanced by idle polls when --fixed-time */
uint32_t win32_now_ms(void);
void win32_advance_ms(uint32_t ms);

#endif

/* ---- hlp.c: GPMHELP.HLP -> HTML in <wdir>/Help (cache); returns 1 and the path of index.html in the host's format ---- */
int hlp_prepare(const char *game_dir, const char *wdir, char *index, size_t cap);
