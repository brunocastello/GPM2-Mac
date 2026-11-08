/* Win32 controls built into the core: BUTTON, EDIT, STATIC, LISTBOX, COMBOBOX.
 *
 * Full model, no drawing: each control is a child window (user32.c) whose WndProc is `ctl_proc`. The state
 * (text, selection, items, check) lives here; drawing is the backend's responsibility, which mirrors a dialog's
 * controls in native widgets (dialog.c / host.h). Model changes mark the control as "dirty" and the dialog
 * resends them to the host; user actions come back through ctl_user_*, which apply them to the model and notify the parent via
 * WM_COMMAND, as Win32 does.
 */
#include "gdi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WM_SETTEXT 0x000C
#define WM_GETTEXT 0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ENABLE 0x000A
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_KEYDOWN 0x0100
#define WM_CHAR 0x0102
#define WM_COMMAND 0x0111

#define VK_SHIFT 0x10
#define VK_LEFT 0x25
#define VK_RIGHT 0x27
#define VK_HOME 0x24
#define VK_END 0x23
#define VK_DELETE 0x2E
#define VK_SPACE 0x20

/* BS_* */
#define BS_TYPEMASK 0x0Fu
#define BS_DEFPUSHBUTTON 1
#define BS_CHECKBOX 2
#define BS_AUTOCHECKBOX 3
#define BS_RADIOBUTTON 4
#define BS_3STATE 5
#define BS_AUTO3STATE 6
#define BS_GROUPBOX 7
#define BS_AUTORADIOBUTTON 9
#define BN_CLICKED 0
/* ES_* */
#define ES_MULTILINE 0x0004u
#define ES_UPPERCASE 0x0008u
#define ES_LOWERCASE 0x0010u
#define ES_PASSWORD 0x0020u
#define ES_READONLY 0x0800u
#define ES_NUMBER 0x2000u
#define EN_SETFOCUS 0x0100
#define EN_KILLFOCUS 0x0200
#define EN_CHANGE 0x0300
/* LBS_* / CBS_* */
#define LBS_NOTIFY 0x0001u
#define LBS_SORT 0x0002u
#define LBS_MULTIPLESEL 0x0008u
#define LBS_OWNERDRAWFIXED 0x0010u
#define LBS_OWNERDRAWVARIABLE 0x0020u
#define LBS_HASSTRINGS 0x0040u
#define LBS_EXTENDEDSEL 0x0800u
#define CBS_DROPDOWN 2u
#define CBS_DROPDOWNLIST 3u
#define CBS_OWNERDRAWFIXED 0x0010u
#define CBS_OWNERDRAWVARIABLE 0x0020u
#define CBS_SORT 0x0100u
#define CBS_HASSTRINGS 0x0200u
#define LBN_SELCHANGE 1
#define LBN_DBLCLK 2
#define CBN_SELCHANGE 1
#define CBN_DBLCLK 2
#define CBN_SETFOCUS 3
#define CBN_KILLFOCUS 4
#define CBN_EDITCHANGE 5
#define LB_ERR 0xFFFFFFFFu

typedef struct { char *s; uint32_t data; int sel; } Item;
typedef struct {
    int used, kind, dirty;
    uint32_t h, style;
    char *text; size_t tcap;           /* text: button/static/edit; in the DROPDOWN combo, the edit field */
    int check;                         /* buttons: 0, 1, 2 (indeterminate) */
    int s0, s1;                        /* EDIT: selection (s1 = caret) */
    uint32_t limit;
    int ro, pwchar, pushed;
    Item *it; int n, cap;              /* LISTBOX / COMBOBOX */
    int cursel, top;
    uint32_t icon;
} Ctl;

#define MAX_CTL 512
static Ctl cs[MAX_CTL];

static Ctl *find(uint32_t h) {
    for (int i = 0; i < MAX_CTL; i++) if (cs[i].used && cs[i].h == h) return &cs[i];
    return 0;
}
static void set_text(Ctl *c, const char *t) {
    size_t n = strlen(t);
    if (n + 1 > c->tcap) { c->tcap = n + 64; c->text = (char *)realloc(c->text, c->tcap); }
    memcpy(c->text, t, n + 1);
}
static Ctl *get(int kind, uint32_t h) {
    Ctl *c = find(h);
    UWin u;
    int i;
    if (c) return c;
    if (!user_wininfo(h, &u)) return 0;
    for (i = 0; i < MAX_CTL; i++) if (!cs[i].used) break;
    if (i == MAX_CTL) return 0;
    c = &cs[i];
    memset(c, 0, sizeof *c);
    c->used = 1; c->kind = kind; c->h = h; c->style = u.style; c->cursel = -1; c->limit = 0x7FFFFFFEu; c->dirty = 1;
    set_text(c, "");
    if (kind != CK_LISTBOX && kind != CK_COMBO) set_text(c, user_wintitle(h));
    return c;
}
static void drop(Ctl *c) {
    for (int i = 0; i < c->n; i++) free(c->it[i].s);
    free(c->it); free(c->text);
    memset(c, 0, sizeof *c);
}
void ctl_touch(uint32_t h) { Ctl *c = find(h); if (c) c->dirty = 1; }
static int lc_insert(Ctl *c, int idx, const char *s, uint32_t data);
int ctl_list_add_text(uint32_t h, const char *s) {
    Ctl *c = find(h);
    int i;
    if (!c) return -1;
    i = lc_insert(c, -1, s, 0);
    c->dirty = 1;
    return i;
}
uint32_t ctl_next_dirty(uint32_t after) {
    for (int i = 0; i < MAX_CTL; i++) if (cs[i].used && cs[i].dirty && cs[i].h > after) return cs[i].h;   /* HWNDs grow in creation order */
    return 0;
}
void ctl_clear_dirty(uint32_t h) { Ctl *c = find(h); if (c) c->dirty = 0; }
int ctl_kind(uint32_t h) { Ctl *c = find(h); return c ? c->kind : -1; }
int ctl_is_edit(uint32_t h) { Ctl *c = find(h); return c && c->kind == CK_EDIT; }
int ctl_wants_enter(uint32_t h) { Ctl *c = find(h); return c && c->kind == CK_EDIT && (c->style & ES_MULTILINE); }
int ctl_is_button(uint32_t h, int *defpush) {
    Ctl *c = find(h);
    if (!c || c->kind != CK_BUTTON) return 0;
    if (defpush) *defpush = (c->style & BS_TYPEMASK) == BS_DEFPUSHBUTTON;
    return (c->style & BS_TYPEMASK) == 0 || (c->style & BS_TYPEMASK) == BS_DEFPUSHBUTTON;
}

static void notify(Ctl *c, uint32_t code) {
    UWin u;
    if (!user_wininfo(c->h, &u) || !u.parent) return;
    user_send(u.parent, WM_COMMAND, (code << 16) | (u.id & 0xFFFFu), c->h);
}

/* ---- text: WM_SETTEXT / WM_GETTEXT ------------------------------------------------------------- */
static uint32_t put_text(uint32_t buf, uint32_t max, const char *t) {
    uint32_t n = (uint32_t)strlen(t);
    if (max == 0) return 0;
    if (n > max - 1u) n = max - 1u;
    gmem_put(buf, t, n);
    W8(buf + n, 0);
    return n;
}
static char *guest_dup(uint32_t p) {
    uint32_t n = p ? gstr_len(p) : 0;
    char *s = (char *)malloc((size_t)n + 1u);
    if (n) gmem_get(s, p, n);
    s[n] = 0;
    return s;
}
static int ci_cmp(const char *a, const char *b) { return gstr_cmp_ci(a, b); }
static int ci_cmp_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

/* ---- EDIT ---------------------------------------------------------------------------------------- */
static void edit_changed(Ctl *c) {
    c->dirty = 1;
    notify(c, c->kind == CK_COMBO ? CBN_EDITCHANGE : EN_CHANGE);
}
static void edit_replace(Ctl *c, int a, int b, const char *ins) {
    size_t len = strlen(c->text), n = strlen(ins);
    char *t;
    if (a > b) { int x = a; a = b; b = x; }
    if (a < 0) a = 0;
    if ((size_t)b > len) b = (int)len;
    if ((size_t)a > len) a = (int)len;
    if (len - (size_t)(b - a) + n > c->limit) n = c->limit > len - (size_t)(b - a) ? c->limit - (len - (size_t)(b - a)) : 0;
    t = (char *)malloc(len - (size_t)(b - a) + n + 1u);
    memcpy(t, c->text, (size_t)a);
    memcpy(t + a, ins, n);
    memcpy(t + a + n, c->text + b, len - (size_t)b + 1u);
    set_text(c, t);
    free(t);
    c->s0 = c->s1 = a + (int)n;
    edit_changed(c);
}
static uint32_t edit_proc(Ctl *c, uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) {
    int len = (int)strlen(c->text);
    switch (msg) {
    case WM_SETTEXT: { char *s = guest_dup(lp); set_text(c, s); free(s); c->s0 = c->s1 = 0; edit_changed(c); return 1; }
    case WM_GETTEXT: return put_text(lp, wp, c->text);
    case WM_GETTEXTLENGTH: return (uint32_t)len;
    case 0x00B0: {                                           /* EM_GETSEL */
        int a = c->s0 < c->s1 ? c->s0 : c->s1, b = c->s0 < c->s1 ? c->s1 : c->s0;
        if (wp) W32(wp, (uint32_t)a);
        if (lp) W32(lp, (uint32_t)b);
        return (uint32_t)a | ((uint32_t)b << 16);
    }
    case 0x00B1: {                                           /* EM_SETSEL: (start, end); end -1 = to the end; start -1 = none */
        int a = (int)wp, b = (int)lp;
        if (a == -1) { c->s0 = c->s1 = c->s1; return 1; }
        if (b == -1 || b > len) b = len;
        if (a > len) a = len;
        c->s0 = a; c->s1 = b; c->dirty = 1;
        return 1;
    }
    case 0x00B8: return 0;                                   /* EM_GETMODIFY */
    case 0x00B9: return 0;                                   /* EM_SETMODIFY */
    case 0x00BA: return 1;                                   /* EM_GETLINECOUNT */
    case 0x00C4: {                                           /* EM_GETLINE: the 1st WORD of the buffer holds the capacity; no trailing NUL */
        uint32_t cap = R16(lp), n = (uint32_t)strlen(c->text);
        if (n > cap) n = cap;
        if (n) gmem_put(lp, c->text, n);
        return n;
    }
    case 0x00C2: { char *s = guest_dup(lp); edit_replace(c, c->s0, c->s1, s); free(s); return 0; }   /* EM_REPLACESEL */
    case 0x00C5: c->limit = wp ? wp : 0x7FFFFFFEu; return 0; /* EM_LIMITTEXT */
    case 0x00D5: return c->limit;                            /* EM_GETLIMITTEXT */
    case 0x00CC: c->pwchar = (int)wp; return 0;              /* EM_SETPASSWORDCHAR */
    case 0x00CF: c->ro = wp != 0; c->dirty = 1; return 1;    /* EM_SETREADONLY */
    case WM_SETFOCUS:
        if (!(c->style & ES_MULTILINE)) { c->s0 = 0; c->s1 = len; }   /* focus by Tab: selects all */
        notify(c, c->kind == CK_COMBO ? CBN_SETFOCUS : EN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS: notify(c, c->kind == CK_COMBO ? CBN_KILLFOCUS : EN_KILLFOCUS); return 0;
    case WM_CHAR: {
        int ch = (int)(wp & 0xFFu);
        char ins[2] = {0, 0};
        if (c->ro || (c->style & ES_READONLY)) return 0;
        if (ch == 8) {
            if (c->s0 != c->s1) edit_replace(c, c->s0, c->s1, "");
            else if (c->s1 > 0) edit_replace(c, c->s1 - 1, c->s1, "");
        } else if (ch == 13) {
            if (c->style & ES_MULTILINE) edit_replace(c, c->s0, c->s1, "\r\n");
        } else if (ch >= 32 && ch != 127) {
            if ((c->style & ES_NUMBER) && (ch < '0' || ch > '9')) return 0;
            if ((c->style & ES_UPPERCASE) && ch >= 'a' && ch <= 'z') ch -= 32;
            if ((c->style & ES_LOWERCASE) && ch >= 'A' && ch <= 'Z') ch += 32;
            ins[0] = (char)ch;
            edit_replace(c, c->s0, c->s1, ins);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        int shift = user_keydown(VK_SHIFT);
        switch (wp) {
        case VK_LEFT:
            if (shift) { if (c->s1 > 0) c->s1--; }
            else if (c->s0 != c->s1) c->s0 = c->s1 = c->s0 < c->s1 ? c->s0 : c->s1;
            else { if (c->s1 > 0) c->s1--; c->s0 = c->s1; }
            c->dirty = 1; return 0;
        case VK_RIGHT:
            if (shift) { if (c->s1 < len) c->s1++; }
            else if (c->s0 != c->s1) c->s0 = c->s1 = c->s0 > c->s1 ? c->s0 : c->s1;
            else { if (c->s1 < len) c->s1++; c->s0 = c->s1; }
            c->dirty = 1; return 0;
        case VK_HOME: c->s1 = 0; if (!shift) c->s0 = 0; c->dirty = 1; return 0;
        case VK_END: c->s1 = len; if (!shift) c->s0 = len; c->dirty = 1; return 0;
        case VK_DELETE:
            if (c->ro) return 0;
            if (c->s0 != c->s1) edit_replace(c, c->s0, c->s1, "");
            else if (c->s1 < len) edit_replace(c, c->s1, c->s1 + 1, "");
            return 0;
        default: return 0;
        }
    }
    default: return user_defproc(h, msg, wp, lp);
    }
}

/* ---- LISTBOX / COMBOBOX: items -------------------------------------------------------------------- */
static int is_sorted(Ctl *c) { return c->kind == CK_LISTBOX ? (c->style & LBS_SORT) != 0 : (c->style & CBS_SORT) != 0; }
static int no_strings(Ctl *c) {
    uint32_t od = c->kind == CK_LISTBOX ? (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE) : (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE);
    uint32_t hs = c->kind == CK_LISTBOX ? LBS_HASSTRINGS : CBS_HASSTRINGS;
    return (c->style & od) && !(c->style & hs);
}
static int multi(Ctl *c) { return c->kind == CK_LISTBOX && (c->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)); }
static int lc_find(Ctl *c, int start, const char *pre, int exact) {
    size_t pl = strlen(pre);
    if (c->n == 0) return -1;
    for (int k = 1; k <= c->n; k++) {
        int i = ((start + k) % c->n + c->n) % c->n;
        if (exact ? ci_cmp(c->it[i].s, pre) == 0 : (strlen(c->it[i].s) >= pl && ci_cmp_n(c->it[i].s, pre, pl) == 0)) return i;
    }
    return -1;
}
static int lc_insert(Ctl *c, int idx, const char *s, uint32_t data) {
    if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 16; c->it = (Item *)realloc(c->it, (size_t)c->cap * sizeof(Item)); }
    if (idx < 0 || idx > c->n) idx = c->n;
    memmove(c->it + idx + 1, c->it + idx, (size_t)(c->n - idx) * sizeof(Item));
    c->it[idx].s = strdup(s); c->it[idx].data = data; c->it[idx].sel = 0;
    c->n++;
    if (c->cursel >= idx) c->cursel++;
    c->dirty = 1;
    return idx;
}
static void lc_set_cursel(Ctl *c, int i) {
    if (i < -1 || i >= c->n) i = -1;
    c->cursel = i;
    if (!multi(c)) for (int k = 0; k < c->n; k++) c->it[k].sel = k == i;
    if (c->kind == CK_COMBO && (c->style & 3u) == CBS_DROPDOWN && i >= 0) { set_text(c, c->it[i].s); }
    c->dirty = 1;
}
/* common operation; `op` is the equivalent LB_* code (CB_* messages are mapped beforehand) */
static uint32_t lc_op(Ctl *c, uint32_t op, uint32_t wp, uint32_t lp) {
    switch (op) {
    case 0x180: case 0x181: {                                /* ADDSTRING / INSERTSTRING */
        char *s = no_strings(c) ? strdup("") : guest_dup(lp);
        uint32_t data = no_strings(c) ? lp : 0;
        int idx = op == 0x181 ? (int)wp : -1;
        if (op == 0x180 && is_sorted(c)) {
            idx = 0;
            while (idx < c->n && ci_cmp(c->it[idx].s, s) <= 0) idx++;
        }
        idx = lc_insert(c, idx, s, data);
        free(s);
        return (uint32_t)idx;
    }
    case 0x182:                                              /* DELETESTRING */
        if ((int)wp < 0 || (int)wp >= c->n) return LB_ERR;
        free(c->it[wp].s);
        memmove(c->it + wp, c->it + wp + 1, (size_t)(c->n - (int)wp - 1) * sizeof(Item));
        c->n--;
        if (c->cursel == (int)wp) c->cursel = -1; else if (c->cursel > (int)wp) c->cursel--;
        c->dirty = 1;
        return (uint32_t)c->n;
    case 0x184:                                              /* RESETCONTENT */
        for (int i = 0; i < c->n; i++) free(c->it[i].s);
        c->n = 0; c->cursel = -1; c->top = 0; c->dirty = 1;
        return 0;
    case 0x186: lc_set_cursel(c, (int)wp); return (int)wp >= -1 && (int)wp < c->n ? (wp == 0xFFFFFFFFu ? LB_ERR : wp) : LB_ERR;   /* SETCURSEL */
    case 0x188: return (uint32_t)c->cursel;                  /* GETCURSEL (-1 = LB_ERR) */
    case 0x18B: return (uint32_t)c->n;                       /* GETCOUNT */
    case 0x189:                                              /* GETTEXT */
        if ((int)wp < 0 || (int)wp >= c->n) return LB_ERR;
        return put_text(lp, 0x7FFFFFFFu, c->it[wp].s);
    case 0x18A:                                              /* GETTEXTLEN */
        if ((int)wp < 0 || (int)wp >= c->n) return LB_ERR;
        return (uint32_t)strlen(c->it[wp].s);
    case 0x199: return (int)wp >= 0 && (int)wp < c->n ? c->it[wp].data : LB_ERR;          /* GETITEMDATA */
    case 0x19A: if ((int)wp < 0 || (int)wp >= c->n) return LB_ERR; c->it[wp].data = lp; return 1;   /* SETITEMDATA */
    case 0x18F: case 0x1A2: case 0x18C: {                    /* FINDSTRING / FINDSTRINGEXACT / SELECTSTRING */
        char *s = guest_dup(lp);
        int i = lc_find(c, (int)wp, s, op == 0x1A2);
        free(s);
        if (i < 0) return LB_ERR;
        if (op == 0x18C) lc_set_cursel(c, i);
        return (uint32_t)i;
    }
    case 0x185:                                              /* SETSEL (multiple) */
        if ((int)lp == -1) { for (int i = 0; i < c->n; i++) c->it[i].sel = wp != 0; }
        else if ((int)lp >= 0 && (int)lp < c->n) { c->it[lp].sel = wp != 0; c->cursel = (int)lp; }
        else return LB_ERR;
        c->dirty = 1;
        return 0;
    case 0x187: return (int)wp >= 0 && (int)wp < c->n ? (uint32_t)c->it[wp].sel : LB_ERR;   /* GETSEL */
    case 0x190: { uint32_t k = 0; for (int i = 0; i < c->n; i++) k += (uint32_t)c->it[i].sel; return k; }   /* GETSELCOUNT */
    case 0x191: {                                            /* GETSELITEMS */
        uint32_t k = 0;
        for (int i = 0; i < c->n && k < wp; i++) if (c->it[i].sel) W32(lp + 4u * k++, (uint32_t)i);
        return k;
    }
    case 0x18E: return (uint32_t)c->top;                     /* GETTOPINDEX */
    case 0x197: if ((int)wp >= 0 && (int)wp < c->n) c->top = (int)wp; return 0;   /* SETTOPINDEX */
    case 0x1A0: return 0;                                    /* SETITEMHEIGHT */
    case 0x1A1: return 13;                                   /* GETITEMHEIGHT */
    default: return 0xFFFFFFFEu;                             /* not handled */
    }
}
static uint32_t cb_map(uint32_t m) {
    switch (m) {
    case 0x143: return 0x180; case 0x144: return 0x182; case 0x146: return 0x18B; case 0x147: return 0x188;
    case 0x148: return 0x189; case 0x149: return 0x18A; case 0x14A: return 0x181; case 0x14B: return 0x184;
    case 0x14C: return 0x18F; case 0x14D: return 0x18C; case 0x14E: return 0x186; case 0x150: return 0x199;
    case 0x151: return 0x19A; case 0x153: return 0x1A0; case 0x154: return 0x1A1; case 0x158: return 0x1A2;
    default: return 0;
    }
}

static uint32_t list_proc(Ctl *c, uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) {
    uint32_t r;
    switch (msg) {
    case WM_SETTEXT: return 0;
    case WM_GETTEXT: return put_text(lp, wp, "");
    case WM_GETTEXTLENGTH: return 0;
    default:
        if (msg >= 0x180 && msg <= 0x1B0) { r = lc_op(c, msg, wp, lp); if (r != 0xFFFFFFFEu) return r; }
        return user_defproc(h, msg, wp, lp);
    }
}
static uint32_t combo_proc(Ctl *c, uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) {
    int list = (c->style & 3u) == CBS_DROPDOWNLIST;
    uint32_t op;
    switch (msg) {
    case WM_SETTEXT:
        if (list) return 0xFFFFFFFFu;                        /* CB_ERR */
        return edit_proc(c, h, msg, wp, lp);
    case WM_GETTEXT:
        if (list) return put_text(lp, wp, c->cursel >= 0 ? c->it[c->cursel].s : "");
        return put_text(lp, wp, c->text);
    case WM_GETTEXTLENGTH:
        if (list) return c->cursel >= 0 ? (uint32_t)strlen(c->it[c->cursel].s) : 0;
        return (uint32_t)strlen(c->text);
    case 0x0141: c->limit = wp ? wp : 0x7FFFFFFEu; return 1;      /* CB_LIMITTEXT */
    case 0x0140: case 0x0142: return list ? 0xFFFFFFFFu : edit_proc(c, h, msg == 0x140 ? 0xB0 : 0xB1, wp, lp);
    case 0x014F: return 0;                                   /* CB_SHOWDROPDOWN */
    case 0x0157: return 0;                                   /* CB_GETDROPPEDSTATE */
    case 0x0155: case 0x0156: return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_CHAR: case WM_KEYDOWN:
        return list ? 0 : edit_proc(c, h, msg, wp, lp);
    default:
        op = cb_map(msg);
        if (op) {
            uint32_t r = lc_op(c, op, wp, lp);
            return r == 0xFFFFFFFEu ? 0 : r;
        }
        return user_defproc(h, msg, wp, lp);
    }
}

/* ---- BUTTON ---------------------------------------------------------------------------------------- */
static void radio_group(Ctl *c, int set_this) {
    UWin u;
    uint32_t sib[128];
    int n, me = -1, lo, hi;
    if (!user_wininfo(c->h, &u)) return;
    n = user_children(u.parent, sib, 128);
    for (int i = 0; i < n; i++) if (sib[i] == c->h) me = i;
    if (me < 0) return;
    lo = me;                                                 /* the group goes from the previous WS_GROUP (inclusive) to the next */
    while (lo > 0) { UWin w; user_wininfo(sib[lo], &w); if (w.style & 0x20000u) break; lo--; }
    hi = me + 1;
    while (hi < n) { UWin w; user_wininfo(sib[hi], &w); if (w.style & 0x20000u) break; hi++; }
    for (int i = lo; i < hi; i++) {
        Ctl *o = find(sib[i]);
        if (o && o->kind == CK_BUTTON && ((o->style & BS_TYPEMASK) == BS_AUTORADIOBUTTON || (o->style & BS_TYPEMASK) == BS_RADIOBUTTON)) {
            int v = o == c ? set_this : 0;
            if (o->check != v) { o->check = v; o->dirty = 1; }
        }
    }
}
static void btn_click(Ctl *c) {
    uint32_t t = c->style & BS_TYPEMASK;
    if (t == BS_AUTOCHECKBOX) { c->check = !c->check; c->dirty = 1; }
    else if (t == BS_AUTO3STATE) { c->check = (c->check + 1) % 3; c->dirty = 1; }
    else if (t == BS_AUTORADIOBUTTON) radio_group(c, 1);
    notify(c, BN_CLICKED);
}
static uint32_t button_proc(Ctl *c, uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) {
    uint32_t t = c->style & BS_TYPEMASK;
    switch (msg) {
    case WM_SETTEXT: { char *s = guest_dup(lp); set_text(c, s); free(s); c->dirty = 1; return 1; }
    case WM_GETTEXT: return put_text(lp, wp, c->text);
    case WM_GETTEXTLENGTH: return (uint32_t)strlen(c->text);
    case 0x00F0: return (uint32_t)c->check;                  /* BM_GETCHECK */
    case 0x00F1:                                             /* BM_SETCHECK */
        if (t == BS_AUTORADIOBUTTON || t == BS_RADIOBUTTON) radio_group(c, wp != 0);
        else { c->check = (int)wp; c->dirty = 1; }
        return 0;
    case 0x00F2: return (uint32_t)(c->check ? 3 : 0);        /* BM_GETSTATE */
    case 0x00F3: return 0;                                   /* BM_SETSTATE */
    case 0x00F4: c->style = (c->style & ~BS_TYPEMASK) | (wp & BS_TYPEMASK); c->dirty = 1; return 0;   /* BM_SETSTYLE */
    case 0x00F5: btn_click(c); return 0;                     /* BM_CLICK */
    case WM_KEYDOWN: if ((wp == VK_SPACE || wp == 13) && t != BS_GROUPBOX) btn_click(c); return 0;
    case 0x0201: if (t != BS_GROUPBOX) { c->pushed = 1; user_set_focus(h); } return 0;             /* WM_LBUTTONDOWN */
    case 0x0202: {                                                                                /* WM_LBUTTONUP */
        UWin u;
        int x = (int)(int16_t)(lp & 0xFFFFu), y = (int)(int16_t)(lp >> 16), was = c->pushed;
        c->pushed = 0;
        if (was && user_wininfo(h, &u) && x >= 0 && y >= 0 && x < u.w && y < u.h) btn_click(c);
        return 0;
    }
    default: return user_defproc(h, msg, wp, lp);
    }
}
static uint32_t static_proc(Ctl *c, uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) {
    switch (msg) {
    case WM_SETTEXT: { char *s = guest_dup(lp); set_text(c, s); free(s); c->dirty = 1; return 1; }
    case WM_GETTEXT: return put_text(lp, wp, c->text);
    case WM_GETTEXTLENGTH: return (uint32_t)strlen(c->text);
    case 0x0170: { uint32_t o = c->icon; c->icon = wp; c->dirty = 1; return o; }     /* STM_SETICON */
    case 0x0171: return c->icon;                                                    /* STM_GETICON */
    default: return user_defproc(h, msg, wp, lp);
    }
}

uint32_t ctl_proc(int kind, uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) {
    Ctl *c;
    if (msg == WM_NCDESTROY) { c = find(h); if (c) drop(c); return 0; }
    if (msg == WM_NCCREATE) { get(kind, h); return 1; }
    c = get(kind, h);
    if (!c) return user_defproc(h, msg, wp, lp);
    if (msg == WM_SETFONT) return 0;
    if (msg == WM_GETFONT) return 0;
    switch (kind) {
    case CK_EDIT: return edit_proc(c, h, msg, wp, lp);
    case CK_LISTBOX: return list_proc(c, h, msg, wp, lp);
    case CK_COMBO: return combo_proc(c, h, msg, wp, lp);
    case CK_BUTTON: return button_proc(c, h, msg, wp, lp);
    default: return static_proc(c, h, msg, wp, lp);
    }
}

/* ---- user actions (host -> model -> parent) -------------------------------------------------------- */
void ctl_user_click(uint32_t h) {
    Ctl *c = find(h);
    UWin u;
    if (!c || !user_wininfo(h, &u) || !u.enabled) return;
    if (c->kind == CK_BUTTON) btn_click(c);
}
void ctl_user_text(uint32_t h, const char *text) {
    Ctl *c = find(h);
    if (!c || (c->kind != CK_EDIT && c->kind != CK_COMBO)) return;
    set_text(c, text);
    c->s0 = c->s1 = (int)strlen(text);
    c->dirty = 1;                                            /* the host compares before replacing (does not touch the cursor) */
    notify(c, c->kind == CK_COMBO ? CBN_EDITCHANGE : EN_CHANGE);
}
void ctl_user_select(uint32_t h, int index, int dbl) {
    Ctl *c = find(h);
    if (!c || (c->kind != CK_LISTBOX && c->kind != CK_COMBO)) return;
    if (dbl) { notify(c, c->kind == CK_COMBO ? CBN_DBLCLK : LBN_DBLCLK); return; }
    if (index < 0 || index >= c->n) return;
    lc_set_cursel(c, index);
    if (c->kind == CK_COMBO) notify(c, CBN_SELCHANGE);
    else if (c->style & LBS_NOTIFY) notify(c, LBN_SELCHANGE);
}

/* ---- mirror to the host -------------------------------------------------------------------------------- */
static void strip_amp(char *dst, size_t cap, const char *s) {
    size_t o = 0;
    for (; *s && o + 1 < cap; s++) {
        if (*s == '&') { if (s[1] == '&') { dst[o++] = '&'; s++; } continue; }
        dst[o++] = *s;
    }
    dst[o] = 0;
}
int ctl_fill_host(uint32_t h, HostCtl *o, const char ***items) {
    Ctl *c = find(h);
    UWin u;
    uint32_t t;
    if (!c || !user_wininfo(h, &u)) return 0;
    memset(o, 0, sizeof *o);
    o->hwnd = h; o->id = u.id ? u.id : h; o->x = u.x;   /* children without id (chat): the HWND itself identifies them */
    o->x = u.x; o->y = u.y; o->w = u.w; o->h = u.h;
    o->enabled = u.enabled; o->visible = u.visible; o->sel = -1;
    t = c->style & BS_TYPEMASK;
    switch (c->kind) {
    case CK_BUTTON:
        o->kind = t == BS_DEFPUSHBUTTON ? HC_DEFPUSH : t == BS_CHECKBOX || t == BS_AUTOCHECKBOX || t == BS_3STATE || t == BS_AUTO3STATE ? HC_CHECK :
                  t == BS_RADIOBUTTON || t == BS_AUTORADIOBUTTON ? HC_RADIO : t == BS_GROUPBOX ? HC_GROUP : HC_PUSH;
        o->checked = c->check;
        strip_amp(o->text, sizeof o->text, c->text);
        break;
    case CK_EDIT:
        o->kind = HC_EDIT;
        o->multiline = (c->style & ES_MULTILINE) != 0; o->password = (c->style & ES_PASSWORD) != 0;
        o->number = (c->style & ES_NUMBER) != 0; o->readonly = c->ro || (c->style & ES_READONLY);
        snprintf(o->text, sizeof o->text, "%s", c->text);
        break;
    case CK_STATIC:
        o->kind = (c->style & 0x1Fu) == 3 ? HC_ICON : HC_STATIC;
        o->align = (c->style & 3u) == 1 ? 1 : (c->style & 3u) == 2 ? 2 : 0;
        strip_amp(o->text, sizeof o->text, c->text);
        break;
    case CK_LISTBOX:
    case CK_COMBO:
        o->kind = c->kind == CK_LISTBOX ? HC_LIST : (c->style & 3u) == CBS_DROPDOWN ? HC_COMBO_DROP : HC_COMBO_LIST;
        o->multisel = multi(c);
        if (o->kind == HC_COMBO_DROP) snprintf(o->text, sizeof o->text, "%s", c->text);
        break;
    default: return 0;
    }
    if (c->kind == CK_LISTBOX || c->kind == CK_COMBO) {
        *items = (const char **)realloc((void *)*items, (size_t)(c->n + 1) * sizeof(char *));
        for (int i = 0; i < c->n; i++) (*items)[i] = c->it[i].s;
        o->nitems = c->n; o->items = (const char *const *)*items; o->sel = c->cursel;
    }
    return 1;
}

/* ---- drawing of the child controls of windows that are not dialogs ----------------------------------------------------
 * (Dialogs become native panels; controls loose in a game window, like the OK button of the About window,
 * are drawn by the core on the window surface, in Windows 95 style, when the frame is presented.) */
void ctl_draw_children(uint32_t parent, uint32_t hbm) {
    uint32_t kids[64];
    int n = user_children(parent, kids, 64);
    for (int i = 0; i < n; i++) {
        Ctl *c = find(kids[i]);
        UWin u;
        const char *t;
        char buf[256];
        if (!c || !user_wininfo(kids[i], &u) || !u.visible) continue;
        t = c->text;
        { size_t o = 0; for (const char *q = t; *q && o + 1 < sizeof buf; q++) { if (*q == '&' && q[1] != '&') continue; buf[o++] = *q; } buf[o] = 0; t = buf; }
        switch (c->kind) {
        case CK_BUTTON: {
            uint32_t bt = c->style & BS_TYPEMASK;
            if (bt == 0 || bt == BS_DEFPUSHBUTTON) {
                gdi_ui_edge(hbm, u.x, u.y, u.w, u.h, c->pushed ? 1 : bt == BS_DEFPUSHBUTTON ? 2 : 0);
                gdi_ui_text(hbm, u.x + c->pushed, u.y + c->pushed, u.w, u.h, t, 1, u.enabled ? 18 : 17);
            } else if (bt == BS_GROUPBOX) {
                gdi_ui_edge(hbm, u.x, u.y + 6, u.w, u.h - 6, 4);
                gdi_ui_text(hbm, u.x + 8, u.y, gdi_ui_text_w(t) + 4, 14, t, 0, 18);
            } else {
                gdi_ui_edge(hbm, u.x, u.y + (u.h - 13) / 2, 13, 13, 3);
                if (c->check) gdi_ui_rect(hbm, u.x + 4, u.y + (u.h - 13) / 2 + 4, 5, 5, 18);
                gdi_ui_text(hbm, u.x + 18, u.y, u.w - 18, u.h, t, 0, u.enabled ? 18 : 17);
            }
            break;
        }
        case CK_STATIC:
            gdi_ui_text(hbm, u.x, u.y, u.w, u.h, t, (c->style & 3u) == 1 ? 1 : (c->style & 3u) == 2 ? 2 : 0, 18);
            break;
        case CK_EDIT:
            gdi_ui_edge(hbm, u.x, u.y, u.w, u.h, 3);
            gdi_ui_text(hbm, u.x + 3, u.y, u.w - 6, u.h, c->text, 0, 8);
            break;
        case CK_LISTBOX:
            gdi_ui_edge(hbm, u.x, u.y, u.w, u.h, 3);
            for (int k = 0; k < c->n && 2 + (k + 1) * 14 < u.h; k++) {
                if (c->it[k].sel) gdi_ui_rect(hbm, u.x + 2, u.y + 2 + k * 14, u.w - 4, 14, 13);
                gdi_ui_text(hbm, u.x + 4, u.y + 2 + k * 14, u.w - 8, 14, c->it[k].s, 0, c->it[k].sel ? 14 : 8);
            }
            break;
        default: break;
        }
    }
}
