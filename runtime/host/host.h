/* Platform backend interface. Backends: host/headless.c (tests, BMP) and
 * host/macos/ (AppKit). The core (win32/) only knows this header; nothing from Apple leaks into it.
 * All host_* functions below are called by the guest thread, except `host_push_event`
 * (called by any host thread) and the lifecycle ones. */
#ifndef GPM2_HOST_H
#define GPM2_HOST_H

#include <stddef.h>
#include <stdint.h>

/* ---- output --------------------------------------------------------------------------------- */
/* Top-level windows. The core reports each change (create, move, show, hide, z-order,
 * title) with host_window_update; the backend decides how to show it: macOS creates an NSPanel child of the
 * main window for each owned top-level window; headless composes them all on a 640x480 desktop.
 * Coordinates on the Win32 virtual screen (640x480). Called by the guest thread. */
typedef struct {
    uint32_t id;                      /* guest HWND */
    uint32_t owner;                   /* owner HWND (0 = none) */
    int x, y, w, h;                   /* window rectangle (with the non-client area) */
    int cx, cy, cw, ch;               /* client area: origin relative to (x, y) and size */
    int bw, cap;                      /* border and caption height (px) */
    int visible, topmost, main;       /* main = the game's main window (top-level, no owner) */
    int caption, sysmenu;             /* has caption; has WS_SYSMENU (close button) */
    int z;                            /* z-order among top-level windows: higher = on top (within topmost) */
    char title[128];
} HostWindow;
void host_window_update(const HostWindow *w);        /* creates or updates */
void host_window_destroy(uint32_t id);

/* Presents the 8 bpp frame (w x h, stride = w, top row first) of the client area of window `win` with the
 * RGBX palette (256 entries, bytes r,g,b,0). `who` = import that produced the frame (for the log). */
void host_present(uint32_t win, const uint8_t *pix, int w, int h, const uint8_t pal[256][4], const char *who);
void host_computer_name(char *out, size_t cap);   /* machine name: Link Play session and players */
void host_set_title(const char *title);
/* system cursor: `idc` is the Win32 IDC_* (0x7F00 arrow, 0x7F02 wait...); 0 = no cursor */
void host_set_cursor(uint32_t idc);
void host_show_cursor(int visible);

/* ---- menus ---------------------------------------------------------------------------------- */
typedef struct HostMenuItem {
    char text[96];                    /* without '&' and without the text after '\t' */
    uint32_t id;
    int grayed, checked, separator;
    int nchild;
    struct HostMenuItem *child;
} HostMenuItem;
/* Replaces the game's menus in the app bar (the host copies; the caller frees). Each top-level
 * item is a menu; `sys` = 1: the items fire WM_SYSCOMMAND (system menu), 0: WM_COMMAND. */
void host_set_menu(const HostMenuItem *top, int n, int sys);

/* MessageBoxA: blocks; returns IDOK(1)/IDCANCEL(2)/IDABORT(3)/IDRETRY(4)/IDIGNORE(5)/IDYES(6)/IDNO(7) */
int host_message_box(const char *title, const char *text, uint32_t flags);

/* Link Play TCP/IP: asks for the host address ("ip" or "ip:port"; empty = LAN discovery only); 0 = cancelled.
 * Headless: GPM2_NET_HOST variable. */
int host_net_ask_address(char *out, int cap);

/* WinHelpA: opens the game's Help on the host; context < 0 = index/contents, otherwise anchor "ctx<context>" (HELP_CONTEXT) */
void host_help(int context);
/* hlp.c (core): converts GPMHELP.HLP from the game folder into HTML in <wdir>/Help (cache); 1 = ok, index = path of index.html */
int hlp_prepare(const char *game_dir, const char *wdir, char *index, size_t cap);

/* ---- input (host -> core, thread-safe) ------------------------------------------------------ */
enum { HOST_EV_MOUSEMOVE = 1, HOST_EV_BUTTON, HOST_EV_KEY, HOST_EV_MENU, HOST_EV_CLOSE, HOST_EV_DLG, HOST_EV_CHAT };   /* HOST_EV_CHAT: a = 0 shows the chat, 1 opens Chat Window Settings */
/* HOST_EV_DLG: win = dialog HWND, a = control id, b = DLG_*, c = index, text = new text */
enum { DLG_CLICK = 1, DLG_TEXT, DLG_SEL, DLG_DBLCLK, DLG_FOCUS };
enum { HOST_MOD_SHIFT = 1, HOST_MOD_CTRL = 2, HOST_MOD_ALT = 4 };
typedef struct {
    int type;
    uint32_t win;        /* MOUSEMOVE, BUTTON and CLOSE: HWND of the source window (0 = main) */
    int x, y;            /* client coordinates of that window; may fall outside it with capture */
    int a;               /* BUTTON: 0 left, 1 right, 2 middle; KEY: virtual key; MENU: id */
    int b;               /* BUTTON/KEY: 1 = pressed; MENU: 1 = WM_SYSCOMMAND, 0 = WM_COMMAND */
    int c;               /* KEY: 1 = auto-repeat */
    int ch;              /* KEY: generated Windows-1252 character (0 = none) */
    unsigned mods;       /* HOST_MOD_* */
    char text[256];      /* DLG_TEXT: control text */
} HostEvent;
void host_push_event(const HostEvent *e);        /* implemented in the core (win32/input.c) */

/* ---- native dialogs ---------------------------------------------------------------------
 * The core is the source of truth (Win32 control model); the backend mirrors each dialog in a native panel.
 * host_dlg_open creates the panel (modal or not; the core handles the modal loop); host_dlg_ctl creates or updates a
 * control (called again on each model change); user actions come back as HOST_EV_DLG.
 * Positions in Win32 pixels relative to the dialog client. Called by the guest thread. */
enum { HC_PUSH = 1, HC_DEFPUSH, HC_CHECK, HC_RADIO, HC_GROUP, HC_EDIT, HC_STATIC, HC_LIST, HC_COMBO_LIST, HC_COMBO_DROP, HC_ICON, HC_OTHER };
#define HOST_DLG_MARGIN 16            /* margin of the native windows in the Mac backends */
typedef struct {
    uint32_t id;                      /* dialog HWND */
    uint32_t owner;
    char name[64];                    /* template name ("EDITAMOUNT") or "#103" */
    char title[128];
    int w, h;                         /* client area in pixels */
    int modal;
    int chat;                         /* Link Play chat window: in the corner of the main window, focus on the text field */
    int mw, mh, ox, oy;               /* native margin in the Mac backends: client area size and control offset (mw = 0: no adjustment) */
} HostDlg;
typedef struct {
    uint32_t hwnd;                    /* control HWND */
    uint32_t id;                      /* control id (dlg item) */
    int kind;                         /* HC_* */
    int x, y, w, h;
    char text[256];
    int checked, enabled, visible;
    int align;                        /* STATIC/EDIT: 0 left, 1 center, 2 right */
    int multiline, password, number, readonly, multisel;
    int nitems;
    const char *const *items;
    int sel;                          /* LISTBOX/COMBOBOX: index (-1 = none) */
} HostCtl;
void host_dlg_open(const HostDlg *d);
void host_dlg_ctl(uint32_t dlg, const HostCtl *c);
void host_dlg_close(uint32_t dlg);
void host_dlg_front(uint32_t dlg);    /* brings the panel to the front */
void host_dlg_back(uint32_t dlg);     /* sends the panel behind the main window, which becomes active */
void host_dlg_center(uint32_t dlg);   /* centers the panel on the main window */
void host_beep(void);                 /* MessageBeep */
void host_chat_menu_enable(int on);   /* Chat menu: enabled once Link Play is established */

/* ---- Open/Save: host paths; the core converts to/from DOS paths ------------------------- */
typedef struct {
    int save;
    char title[128];
    char dir[1024];                   /* initial folder (host) */
    char name[260];                   /* initial file name (no folder) */
    int nfilt;
    char fdesc[8][96], fpat[8][128];  /* description and pattern ("*.gps;*.gcf") of each filter */
    int fsel;                         /* initial filter (0-based) */
    char defext[16];                  /* default extension without dot */
    int overwrite_prompt;
} HostFileReq;
/* 1 = chose (host path in out), 0 = cancelled. Blocks the guest thread. */
int host_file_dialog(const HostFileReq *rq, char *out, size_t cap);

/* ---- audio -------------------------------------------------------------------------------------
 * host_audio_open: the backend starts the output and begins pulling `audio_render` (core) on its audio thread;
 * returns 1 if there is real output (0 = headless: the core advances the mixer by the guest clock). */
int host_audio_open(void);
void audio_render(int16_t *out, int nframes);      /* core: stereo 44100 Hz, called by the audio thread */
void audio_stream_service(void);                   /* core: reads the next blocks of long WAVs; the backend without threads calls it while pumping */
uint32_t dlg_debug_open(const char *name);        /* core: opens a DIALOG without a DlgProc (script `dlg-open`) */
void audio_log_write(const char *path);            /* core: list of the sounds played (name, start, end) */

/* ---- time and lifecycle ---------------------------------------------------------------------- */
uint32_t host_now_ms(void);
void host_sleep_ms(uint32_t ms);
void host_init(const char *out_dir);
unsigned host_frame_count(void);
const char *host_last_frame_path(void);
/* Called by the core on each poll with no message. `idle` = consecutive idle polls on the new frame.
 * The headless backend runs the script (--script) here; returns 1 to end the program. */
int host_poll_events(unsigned idle);
int host_script_active(void);
void host_set_script(const char *path);

#endif
