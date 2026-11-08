/* DIALOG dialogs from the .rsrc: DialogBoxParamA (modal, nested loop on the guest thread),
 * CreateDialogParamA (modeless), EndDialog, DefDlgProcA and the GetDlgItem/CheckDlgButton/... family.
 *
 * Each dialog is a window of class "#32770" (native proc `dlg_wndproc`, which calls the guest DlgProc and then
 * the default handling) with the controls as children (controls.c). The backend mirrors the dialog in a native panel
 * (host_dlg_*); user actions come back as HOST_EV_DLG and become WM_COMMAND through the control model.
 * Dialog units of the MS Sans Serif 8 font (Win95): cxChar 6, cyChar 13 -> x = dlu*6/4, y = dlu*13/8.
 */
#include "hooks/hooks.h"
#include "win32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_CLOSE 0x0010
#define WM_NCDESTROY 0x0082
#define DM_GETDEFID 0x0400
#define DM_SETDEFID 0x0401
#define WS_CHILD 0x40000000u
#define WS_VISIBLE 0x10000000u
#define WS_TABSTOP 0x00010000u
#define WS_DISABLED 0x08000000u
#define IDOK 1
#define IDCANCEL 2
#define MAX_DLG 16

typedef struct {
    int used, modal, ended, shown, chat;
    uint32_t h, owner, proc, param, result, defid, prev_focus;
    char name[64];
} Dlg;
static Dlg dlgs[MAX_DLG];
static int nshown;

static Dlg *dfind(uint32_t h) {
    for (int i = 0; i < MAX_DLG; i++) if (dlgs[i].used && dlgs[i].h == h) return &dlgs[i];
    return 0;
}
/* the dialog (from the table) that contains window `h` (itself or a descendant) */
static Dlg *dlg_of(uint32_t h) {
    UWin u;
    for (int d = 0; d < 4 && user_wininfo(h, &u); d++) {
        Dlg *k = dfind(h);
        if (k) return k;
        h = u.parent;
    }
    return 0;
}

/* ---- reading the template -------------------------------------------------------------------------- */
static uint32_t skip_sz(uint32_t p) { while (R16(p)) p += 2; return p + 2; }
static void get_sz(uint32_t p, char *out, size_t cap) {
    size_t i = 0;
    for (; R16(p) && i + 1 < cap; p += 2) out[i++] = (char)R16(p);
    out[i] = 0;
}
static uint32_t skip_ord_or_sz(uint32_t p) {            /* 0x0000 | 0xFFFF id | sz */
    uint16_t w = (uint16_t)R16(p);
    if (w == 0) return p + 2;
    if (w == 0xFFFF) return p + 4;
    return skip_sz(p);
}
static const char *class_of_atom(uint32_t a) {
    switch (a) {
    case 0x80: return "BUTTON"; case 0x81: return "EDIT"; case 0x82: return "STATIC";
    case 0x83: return "LISTBOX"; case 0x84: return "SCROLLBAR"; case 0x85: return "COMBOBOX";
    default: return "STATIC";
    }
}
static int dpx(int v) { return v * 6 / 4; }
static int dpy(int v) { return v * 13 / 8; }

static void host_open(Dlg *d, const char *title, int cw, int ch) {
    HostDlg hd;
    uint32_t kids[128];
    int n = user_children(d->h, kids, 128);
    memset(&hd, 0, sizeof hd);
    hd.id = d->h; hd.owner = d->owner; hd.modal = d->modal; hd.w = cw; hd.h = ch;
    hd.chat = d->chat;
    snprintf(hd.name, sizeof hd.name, "%s", d->name);
    snprintf(hd.title, sizeof hd.title, "%s", title);
    {   /* rectangle of the visible controls, for the backend to place the system margin; with fewer than 2 controls the game draws the rest (dlg_paint_text) */
        int x0 = 1 << 20, y0 = 1 << 20, x1 = 0, y1 = 0, cnt = 0;
        for (int i = 0; i < n && !d->chat; i++) {
            HostCtl c;
            const char **it = 0;
            int ch2;
            memset(&c, 0, sizeof c);
            if (!ctl_fill_host(kids[i], &c, &it) || !c.visible) continue;
            ch2 = (c.kind == HC_COMBO_LIST || c.kind == HC_COMBO_DROP) && c.h > 22 ? 22 : c.h;   /* the template height includes the open list */
            if (c.x < x0) x0 = c.x;
            if (c.y < y0) y0 = c.y;
            if (c.x + c.w > x1) x1 = c.x + c.w;
            if (c.y + ch2 > y1) y1 = c.y + ch2;
            cnt++;
        }
        if (cnt >= 2) { hd.ox = HOST_DLG_MARGIN - x0; hd.oy = HOST_DLG_MARGIN - y0; hd.mw = x1 - x0 + 2 * HOST_DLG_MARGIN; hd.mh = y1 - y0 + 2 * HOST_DLG_MARGIN; }
    }
    host_dlg_open(&hd);
    d->shown = 1; nshown++;
    for (int i = 0; i < n; i++) ctl_touch(kids[i]);     /* all controls stay "dirty" until dlg_sync */
}

/* Creates the dialog and the controls, sends WM_INITDIALOG and shows it. 0 if the template does not exist. */
static uint32_t dlg_open_named(const char *name, uint32_t owner, uint32_t proc, uint32_t param, int modal) {
    uint32_t a, p, h, first = 0, kids[128];
    char title[128], cls[64], text[256], face[64];
    uint32_t style, exstyle;
    int cdit, x, y, cx, cy, n, cw, ch;
    Dlg *d = 0;
    uint32_t rsz;
    if (name[0] == '#') a = res_find(5, (uint32_t)atoi(name + 1), &rsz); else a = res_find_name(5, name, &rsz);
    if (!a) { win32_log("dialog '%s' does not exist in .rsrc", name); return 0; }
    style = R32(a); exstyle = R32(a + 4); cdit = (int)R16(a + 8);
    x = (int)R16(a + 10); y = (int)R16(a + 12); cx = (int)R16(a + 14); cy = (int)R16(a + 16);
    (void)x; (void)y;
    p = a + 18;
    p = skip_ord_or_sz(p);                               /* menu */
    p = skip_ord_or_sz(p);                               /* class */
    get_sz(p, title, sizeof title); p = skip_sz(p);
    if (style & 0x40u) { p += 2; get_sz(p, face, sizeof face); p = skip_sz(p); }    /* DS_SETFONT */
    for (int i = 0; i < MAX_DLG; i++) if (!dlgs[i].used) { d = &dlgs[i]; break; }
    if (!d) return 0;
    memset(d, 0, sizeof *d);
    d->used = 1; d->modal = modal; d->owner = owner; d->proc = proc; d->param = param; d->defid = IDOK;
    d->prev_focus = user_focus();
    snprintf(d->name, sizeof d->name, "%s", name);
    cw = dpx(cx); ch = dpy(cy);
    {   /* 3 px border and 19 px caption; centered on the 640x480 virtual screen */
        int w = cw + 6, hgt = ch + 6 + 19;
        h = user_create("#32770", title, style, exstyle, (640 - w) / 2, (480 - hgt) / 2, w, hgt, owner, 0, 0x400000u);
    }
    if (!h) { d->used = 0; return 0; }
    d->h = h;
    for (int i = 0; i < cdit; i++) {
        uint32_t istyle, iex, id, cp;
        int ix, iy, icx, icy;
        p = (p + 3u) & ~3u;
        istyle = R32(p); iex = R32(p + 4);
        ix = (int)R16(p + 8); iy = (int)R16(p + 10); icx = (int)R16(p + 12); icy = (int)R16(p + 14);
        id = R16(p + 16);
        p += 18;
        if (R16(p) == 0xFFFF) { snprintf(cls, sizeof cls, "%s", class_of_atom(R16(p + 2))); p += 4; }
        else { get_sz(p, cls, sizeof cls); p = skip_sz(p); }
        if (R16(p) == 0xFFFF) { snprintf(text, sizeof text, "#%u", R16(p + 2)); p += 4; }
        else { get_sz(p, text, sizeof text); p = skip_sz(p); }
        p += 2u + R16(p);                                /* creation data */
        cp = user_create(cls, text, istyle | WS_CHILD, iex, dpx(ix), dpy(iy), dpx(icx), dpy(icy), h, id, 0x400000u);
        if (!first && cp && (istyle & WS_TABSTOP) && (istyle & WS_VISIBLE) && !(istyle & WS_DISABLED)) first = cp;
        if (cp && (istyle & 0xFu) == 1 && !gstr_cmp_ci(cls, "BUTTON")) d->defid = id;
    }
    n = user_children(h, kids, 128);
    if (!first && n) first = kids[0];
    {   /* WM_INITDIALOG: TRUE = the system puts the focus on the first tabstop */
        uint32_t r = user_send(h, WM_INITDIALOG, first, param);
        if (r && first) user_set_focus(first);
    }
    if (dfind(h) && (style & WS_VISIBLE)) { const char *tt = user_title(h); user_set_visible(h, 1); host_open(d, tt[0] ? tt : title[0] ? title : "Grand Prix Manager 2", cw, ch); user_invalidate(h); }   /* title from WM_INITDIALOG; dialog WM_PAINT (network status text) */
    return h;
}

static uint32_t dlg_create(uint32_t hinst, uint32_t tmpl, uint32_t owner, uint32_t proc, uint32_t param, int modal) {
    char name[64];
    (void)hinst;
    if (tmpl < 0x10000u) snprintf(name, sizeof name, "#%u", tmpl); else gstr_get(name, sizeof name, tmpl);
    return dlg_open_named(name, owner, proc, param, modal);
}
/* ---- Link Play chat: Chat menu and BUG-025 ------------------------------------------------------------
 * Game addresses: DAT_0057b2ee (network game), DAT_0057b2d2 (chat window, 0 = closed), DAT_0057b2d6
 * (chat list, not zeroed on close) and FUN_005050f0 (creates the window). BUG-025: with the chat closed, the game sends
 * messages to the destroyed list and they are lost; here they are kept and enter the list on reopen. */
#define GA_NET_MODE  0x57B2EEu
#define GA_CHAT_WIN  0x57B2D2u
#define GA_CHAT_LIST 0x57B2D6u
#define GA_CHAT_NEW  0x5050F0u
#define GA_SEND_BACK 0x71B964u                          /* After Sending: Send Window To Back */
#define GA_NEW_TOP   0x71B95Cu                          /* On New Messages: Bring Window To Top */
#define GA_HOT_CENTRE 0x71B96Cu                         /* Hot Key: Centre Chat Window */
enum { CHAT_KEEP = 256 };
static char chat_keep[CHAT_KEEP][256];
static int chat_nkeep, chat_seen, chat_menu_on;

int dlg_chat_capture(uint32_t h, uint32_t msg, uint32_t lp, uint32_t *ret) {
    if ((msg != 0x180u && msg != 0x186u) || !h || !chat_seen || !fix_on(FIX_BUG025)) return 0;   /* LB_ADDSTRING, LB_SETCURSEL */
    if (h != R32(GA_CHAT_LIST) || R32(GA_CHAT_WIN)) return 0;
    if (msg == 0x180u) {
        if (chat_nkeep == CHAT_KEEP) { memmove(chat_keep[0], chat_keep[1], (CHAT_KEEP - 1) * sizeof chat_keep[0]); chat_nkeep--; }
        gstr_get(chat_keep[chat_nkeep++], sizeof chat_keep[0], lp);
        if (R32(GA_NEW_TOP)) {                                   /* reopens (dlg_adopt restores the messages); the game continues and brings it to the front */
            guest_call(GA_CHAT_NEW, 0);
            *ret = user_send(R32(GA_CHAT_LIST), 0x18Bu, 0, 0) - 1u;   /* LB_GETCOUNT: the game's LB_SETCURSEL goes to the last row */
            return 1;
        }
    }
    *ret = 0;
    return 1;
}
static void chat_replay(void) {
    uint32_t list = R32(GA_CHAT_LIST);
    int last = -1;
    for (int i = 0; i < chat_nkeep; i++) last = ctl_list_add_text(list, chat_keep[i]);
    chat_nkeep = 0;
    if (last >= 0) user_send(list, 0x186u, (uint32_t)last, 0);   /* scrolls to the last one, as the game does */
}
void dlg_chat_command(int cmd) {
    uint32_t h;
    if (!chat_seen || !R32(GA_NET_MODE)) return;
    if (!R32(GA_CHAT_WIN)) guest_call(GA_CHAT_NEW, 0);
    h = R32(GA_CHAT_WIN);
    if (!h) return;
    if (cmd == 0) {                                              /* like the hot key */
        if (R32(GA_HOT_CENTRE)) dlg_chat_center(h);
        dlg_bring_front(h);
    }
    else user_post(h, 0x0112u, 1, 0);                      /* Settings item of the chat system menu */
}
int dlg_bring_front(uint32_t h) {
    Dlg *d = dfind(h);
    if (!d || !d->chat || !d->shown) return 0;
    host_dlg_front(h);
    return 1;
}
void dlg_chat_center(uint32_t h) {
    Dlg *d = dfind(h);
    if (d && d->chat && d->shown) host_dlg_center(h);
}
static void chat_menu_sync(void) {
    int net = R32(GA_NET_MODE) != 0;
    if (!net) { chat_seen = 0; chat_nkeep = 0; }
    if ((net && chat_seen) != chat_menu_on) { chat_menu_on = net && chat_seen; host_chat_menu_enable(chat_menu_on); }
}

/* Game window that does not come from a template (Link Play chat) shown as a native panel; the WndProc remains the game's */
void dlg_adopt(uint32_t h, const char *title, int cw, int ch) {
    Dlg *d = 0;
    if (dfind(h)) return;
    for (int i = 0; i < MAX_DLG; i++) if (!dlgs[i].used) { d = &dlgs[i]; break; }
    if (!d) return;
    memset(d, 0, sizeof *d);
    d->used = 1; d->chat = 1; d->h = h; d->defid = IDOK;
    snprintf(d->name, sizeof d->name, "ChatClass");
    host_open(d, title[0] ? title : "Grand Prix Manager 2", cw, ch);
    chat_seen = 1;
    chat_replay();
}
/* test hook (script `dlg-open`): opens a template without a guest DlgProc */
uint32_t dlg_debug_open(const char *name) { return dlg_open_named(name, user_main_window(), 0, 0, 0); }

/* ---- Native WndProc of class #32770 ------------------------------------------------------------------ */
static uint32_t dlg_default(Dlg *d, uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) {
    switch (msg) {
    case WM_CLOSE: {
        uint32_t c = user_find_child(h, IDCANCEL);
        UWin u;
        if (c && user_wininfo(c, &u) && u.enabled) user_send(h, WM_COMMAND, IDCANCEL, c);
        else if (!c) user_send(h, WM_COMMAND, IDCANCEL, 0);
        return 0;
    }
    case DM_GETDEFID: return d ? (0x534Bu << 16) | d->defid : 0;
    case DM_SETDEFID: if (d) d->defid = wp; return 1;
    default: return user_defproc(h, msg, wp, lp);
    }
}
uint32_t dlg_wndproc(uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) {
    Dlg *d = dfind(h);
    uint32_t r = 0;
    if (!d) return user_defproc(h, msg, wp, lp);
    if (d->proc) r = guest_call(d->proc, 4, h, msg, wp, lp);
    if (msg == WM_INITDIALOG) return r;
    if (r) return r;
    return dlg_default(d, h, msg, wp, lp);
}
static uint32_t u_DefDlgProcA(void) { return dlg_default(dfind(ARG(0)), ARG(0), ARG(1), ARG(2), ARG(3)); }

/* ---- end of the dialog ------------------------------------------------------------------------------------- */
/* Called by destroy_window: the game closes modeless dialogs with DestroyWindow, not EndDialog */
void dlg_window_destroying(uint32_t h) {
    Dlg *d = dfind(h);
    if (d && d->shown) { host_dlg_close(h); d->shown = 0; nshown--; }
}
void dlg_window_destroyed(uint32_t h) {
    Dlg *d = dfind(h);
    if (d) d->used = 0;
}
static void dlg_destroy(Dlg *d) { user_destroy(d->h); }
void dlg_abort(uint32_t dlg) { Dlg *d = dfind(dlg); if (d) { d->ended = 1; d->result = IDCANCEL; } }
static uint32_t u_EndDialog(void) {
    Dlg *d = dfind(ARG(0));
    if (!d) return 0;
    d->ended = 1; d->result = ARG(1);
    return 1;
}

static uint32_t u_DialogBoxParamA(void) {
    uint32_t owner = ARG(2), h, res = 0xFFFFFFFFu;
    Dlg *d;
    UWin ou;
    int owner_was = 0;
    h = dlg_create(ARG(0), ARG(1), owner, ARG(3), ARG(4), 1);
    if (!h) return 0xFFFFFFFFu;
    d = dfind(h);
    if (owner && user_wininfo(owner, &ou)) { owner_was = ou.enabled; user_set_enabled(owner, 0); }
    while (d->used && !d->ended) user_modal_step(h);
    res = d->result;
    if (owner_was) user_set_enabled(owner, 1);
    {   uint32_t pf = d->prev_focus;
        dlg_destroy(d);
        if (pf) user_set_focus(pf);
    }
    return res;
}
static uint32_t u_CreateDialogParamA(void) {
    return dlg_create(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), 0);
}

/* ---- Item API ------------------------------------------------------------------------------------------ */
static uint32_t u_GetDlgItem(void) { return user_find_child(ARG(0), ARG(1)); }
static uint32_t u_SendDlgItemMessageA(void) {
    uint32_t c = user_find_child(ARG(0), ARG(1));
    return c ? user_send(c, ARG(2), ARG(3), ARG(4)) : 0;
}
static uint32_t u_CheckDlgButton(void) {
    uint32_t c = user_find_child(ARG(0), ARG(1));
    if (!c) return 0;
    user_send(c, 0xF1u, ARG(2), 0);                      /* BM_SETCHECK */
    return 1;
}
static uint32_t u_IsDlgButtonChecked(void) {
    uint32_t c = user_find_child(ARG(0), ARG(1));
    return c ? user_send(c, 0xF0u, 0, 0) : 0;
}
static uint32_t u_CheckRadioButton(void) {
    for (uint32_t id = ARG(1); id <= ARG(2); id++) {
        uint32_t c = user_find_child(ARG(0), id);
        if (c) user_send(c, 0xF1u, id == ARG(3), 0);
    }
    return 1;
}

/* ---- host: events, synchronization, keyboard ------------------------------------------------------------------ */
void dlg_host_event(const HostEvent *e) {
    uint32_t c;
    Dlg *d = dfind(e->win);
    UWin u;
    if (!d) return;
    if ((uint32_t)e->a == 0xF137u) {                     /* Hot Key popup: checks the chosen radio (BM_SETCHECK) */
        static const uint32_t ids[9] = {1035, 1036, 1037, 1038, 1039, 1040, 1041, 1010, 1014};
        if (e->b == DLG_SEL && e->c >= 0 && e->c < 9) {  /* BM_SETCHECK 1 clears the group; BM_SETCHECK 0 on another radio would clear the chosen one */
            uint32_t r = user_find_child(e->win, ids[e->c]);
            if (r) user_send(r, 0xF1u, 1u, 0);
        }
        return;
    }
    c = user_find_child(e->win, (uint32_t)e->a);
    if (!c && user_wininfo((uint32_t)e->a, &u) && u.parent == e->win) c = (uint32_t)e->a;   /* child without id: see ctl_fill_host */
    if (!c) return;
    switch (e->b) {
    case DLG_CLICK:
        ctl_user_click(c);
        ctl_touch(c);                                    /* the native control already changed by itself: if the game refused (Send To Players), go back to the core state */
        if (d->chat && dfind(e->win)) {                  /* sent: the field is ready for the next message */
            uint32_t kids[8];
            int n = user_children(e->win, kids, 8);
            for (int i = 0; i < n; i++) if (ctl_is_edit(kids[i])) { ctl_user_text(kids[i], ""); ctl_touch(kids[i]); }
            if (R32(GA_SEND_BACK)) host_dlg_back(e->win);   /* FUN_004f50e0: the game window returns to the front */
        }
        break;
    case DLG_TEXT: ctl_user_text(c, e->text); break;
    case DLG_SEL: ctl_user_select(c, e->c, 0); break;
    case DLG_DBLCLK: ctl_user_select(c, e->c, 1); break;
    case DLG_FOCUS: user_set_focus(c); break;
    default: break;
    }
}
void dlg_paint_text(uint32_t h, const char *t, int x, int y, int w, int he) {
    Dlg *d = dfind(h);
    HostCtl hc;
    if (!d || !d->shown) return;
    memset(&hc, 0, sizeof hc);
    hc.hwnd = 0xDD000000u | (h & 0xFFFFFFu);                  /* synthetic control, one per dialog: no event comes back from it */
    hc.kind = HC_STATIC; hc.x = x; hc.y = y; hc.w = w; hc.h = he;
    hc.enabled = 1; hc.visible = 1; hc.sel = -1;
    snprintf(hc.text, sizeof hc.text, "%s", t);
    host_dlg_ctl(d->h, &hc);
}
/* Chat Window Settings (#137): the 9 Hot Key radios appear as a popup. The core keeps the radios, which the game reads on OK. */
#define HOTKEY_POPUP 0xF137u
static const uint32_t hotkey_ids[9] = {1035, 1036, 1037, 1038, 1039, 1040, 1041, 1010, 1014};
static int is_hotkey_radio(const Dlg *d, uint32_t id) {
    if (strcmp(d->name, "#137")) return 0;
    for (int i = 0; i < 9; i++) if (hotkey_ids[i] == id) return 1;
    return 0;
}
static void hotkey_send(Dlg *d) {
    static char txt[9][64];
    const char *ptr[9];
    const char **it = 0;
    HostCtl hc, r;
    int n = 0;
    memset(&hc, 0, sizeof hc);
    hc.hwnd = 0xDC000000u | (d->h & 0xFFFFFFu); hc.id = HOTKEY_POPUP; hc.kind = HC_COMBO_LIST;
    hc.enabled = 1; hc.visible = 1; hc.sel = -1; hc.w = 150; hc.h = 22;
    for (int i = 0; i < 9; i++) {
        uint32_t c = user_find_child(d->h, hotkey_ids[i]);
        if (!c || !ctl_fill_host(c, &r, &it)) continue;
        if (!n) { hc.x = r.x; hc.y = r.y; }
        snprintf(txt[n], sizeof txt[n], "%s", r.text);
        ptr[n] = txt[n];
        if (r.checked) hc.sel = n;
        n++;
    }
    free((void *)it);
    hc.nitems = n; hc.items = ptr;
    host_dlg_ctl(d->h, &hc);
}
void dlg_sync(void) {
    uint32_t h = 0;
    chat_menu_sync();
    const char **items = 0;
    Dlg *hk = 0;
    if (!nshown) return;
    while ((h = ctl_next_dirty(h)) != 0) {
        UWin u;
        HostCtl hc;
        ctl_clear_dirty(h);
        if (!user_wininfo(h, &u)) continue;
        { Dlg *d = dfind(u.parent);
          if (d && d->shown && is_hotkey_radio(d, u.id)) hk = d;
          else if (d && d->shown && ctl_fill_host(h, &hc, &items)) host_dlg_ctl(d->h, &hc); }
    }
    free((void *)items);
    if (hk) hotkey_send(hk);
}

/* IsDialogMessage: Tab/Shift-Tab, Enter (default button) and Esc (IDCANCEL). 1 = consumed the key */
static int tabstop_ok(uint32_t c) {
    UWin u;
    return user_wininfo(c, &u) && (u.style & WS_TABSTOP) && u.visible && u.enabled && ctl_kind(c) >= 0;
}
int dlg_key(uint32_t dlg, uint32_t target, uint32_t vk) {
    Dlg *d = dfind(dlg);
    if (!d || dlg_of(target) != d) return 0;
    if (vk == 9) {                                       /* Tab */
        uint32_t kids[128];
        int n = user_children(dlg, kids, 128), cur = -1, back = user_keydown(0x10);
        for (int i = 0; i < n; i++) if (kids[i] == target) cur = i;
        for (int k = 1; k <= n; k++) {
            int i = ((cur + (back ? -k : k)) % n + n) % n;
            if (tabstop_ok(kids[i])) { user_set_focus(kids[i]); break; }
        }
        return 1;
    }
    if (vk == 13) {                                      /* Enter */
        uint32_t c;
        UWin u;
        if (ctl_wants_enter(target)) return 0;
        if (ctl_is_button(target, 0)) { user_send(target, 0xF5u, 0, 0); return 1; }
        c = user_find_child(dlg, d->defid);
        if (c && user_wininfo(c, &u) && u.enabled) user_send(c, 0xF5u, 0, 0);
        else user_send(dlg, WM_COMMAND, d->defid, 0);
        return 1;
    }
    if (vk == 27) {                                      /* Esc */
        user_send(dlg, WM_COMMAND, IDCANCEL, user_find_child(dlg, IDCANCEL));
        return 1;
    }
    return 0;
}

void dialog_register(void) {
#define R(n, f) win32_register("USER32.dll!" n, f)
    R("DialogBoxParamA", u_DialogBoxParamA); R("CreateDialogParamA", u_CreateDialogParamA);
    R("EndDialog", u_EndDialog); R("DefDlgProcA", u_DefDlgProcA); R("GetDlgItem", u_GetDlgItem);
    R("SendDlgItemMessageA", u_SendDlgItemMessageA); R("CheckDlgButton", u_CheckDlgButton);
    R("IsDlgButtonChecked", u_IsDlgButtonChecked); R("CheckRadioButton", u_CheckRadioButton);
#undef R
}
