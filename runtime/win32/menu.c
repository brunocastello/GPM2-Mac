/* Win32 menus: handle table, LoadMenuA from the .rsrc, per-window system menu and the
 * mirror to the host (macOS menu bar).
 *
 * The game does NOT use a Win32 menu bar: "File Options Help", "Next Race" and the money are drawn by
 * the game itself in the frame. It builds the menus in the main window's SYSTEM MENU (GetSystemMenu +
 * InsertMenuA at position 6 + LoadMenuA 0x83/0x81/0x84) and receives the commands via WM_SYSCOMMAND.
 * So by default the mirror shows the system menu popups; if the window has a real menu
 * bar (SetMenu / hMenu), the mirror shows the bar and the commands become WM_COMMAND.
 */
#include "win32.h"
#include "../host/host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MF_GRAYED 0x0001u
#define MF_DISABLED 0x0002u
#define MF_CHECKED 0x0008u
#define MF_POPUP 0x0010u
#define MF_BYPOSITION 0x0400u
#define MF_SEPARATOR 0x0800u
#define MF_BITMAP 0x0004u
#define MF_OWNERDRAW 0x0100u
#define MF_END 0x0080u

#define MENU_BASE 0x00070000u
#define MAX_MENU 256
#define MAX_WND_MENU 256

typedef struct { uint32_t flags, id, sub; char text[96]; } MItem;
typedef struct { int used, n, cap; MItem *it; } Menu;

static Menu menus[MAX_MENU];
static struct { uint32_t bar, sys; } wm[MAX_WND_MENU];
static int dirty;

static uint32_t mh(int i) { return MENU_BASE + 4u * (uint32_t)i; }
static Menu *menu_of(uint32_t h) {
    unsigned i;
    if (h < MENU_BASE || (h - MENU_BASE) % 4u) return 0;
    i = (h - MENU_BASE) / 4u;
    return i < MAX_MENU && menus[i].used ? &menus[i] : 0;
}
static uint32_t new_menu(void) {
    for (int i = 0; i < MAX_MENU; i++)
        if (!menus[i].used) { memset(&menus[i], 0, sizeof menus[i]); menus[i].used = 1; return mh(i); }
    return 0;
}
static void free_menu(uint32_t h) {
    Menu *m = menu_of(h);
    if (!m) return;
    for (int i = 0; i < m->n; i++) if (m->it[i].flags & MF_POPUP) free_menu(m->it[i].sub);
    free(m->it);
    memset(m, 0, sizeof *m);
}
static int wm_idx(uint32_t hwnd) {
    unsigned i;
    if (hwnd < HWND_BASE || (hwnd - HWND_BASE) % 4u) return -1;
    i = (hwnd - HWND_BASE) / 4u;
    return i < MAX_WND_MENU ? (int)i : -1;
}

void menu_window_created(uint32_t hwnd, uint32_t hmenu) {
    int i = wm_idx(hwnd);
    if (i < 0) return;
    wm[i].bar = menu_of(hmenu) ? hmenu : 0; wm[i].sys = 0;
    dirty = 1;
}
int menu_has_bar(uint32_t hwnd) { int i = wm_idx(hwnd); return i >= 0 && wm[i].bar != 0; }
void menu_window_destroyed(uint32_t hwnd) {
    int i = wm_idx(hwnd);
    if (i < 0) return;
    free_menu(wm[i].bar); free_menu(wm[i].sys);
    wm[i].bar = wm[i].sys = 0;
    dirty = 1;
}

/* ---- items ------------------------------------------------------------------------------------- */
static void add_at(Menu *m, int pos, const MItem *src) {
    if (pos < 0 || pos > m->n) pos = m->n;
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 8;
        m->it = (MItem *)realloc(m->it, (size_t)m->cap * sizeof(MItem));
    }
    memmove(&m->it[pos + 1], &m->it[pos], (size_t)(m->n - pos) * sizeof(MItem));
    m->it[pos] = *src;
    m->n++;
}
static void del_at(Menu *m, int pos, int destroy_sub) {
    if (pos < 0 || pos >= m->n) return;
    if (destroy_sub && (m->it[pos].flags & MF_POPUP)) free_menu(m->it[pos].sub);
    memmove(&m->it[pos], &m->it[pos + 1], (size_t)(m->n - pos - 1) * sizeof(MItem));
    m->n--;
}
/* finds the item: MF_BYPOSITION -> index; otherwise the first with that command (recursive into popups) */
static int locate(uint32_t h, uint32_t id, uint32_t flags, Menu **pm) {
    Menu *m = menu_of(h);
    if (!m) return -1;
    if (flags & MF_BYPOSITION) {
        if (id >= (uint32_t)m->n) return -1;
        *pm = m; return (int)id;
    }
    for (int i = 0; i < m->n; i++)
        if (!(m->it[i].flags & MF_POPUP) && m->it[i].id == id && !(m->it[i].flags & MF_SEPARATOR)) { *pm = m; return i; }
    for (int i = 0; i < m->n; i++)
        if (m->it[i].flags & MF_POPUP) {
            int r = locate(m->it[i].sub, id, flags, pm);
            if (r >= 0) return r;
        }
    return -1;
}
static void fill_item(MItem *it, uint32_t flags, uint32_t id, uint32_t lp) {
    memset(it, 0, sizeof *it);
    it->flags = flags & ~(MF_BYPOSITION | MF_END);
    it->id = id;
    if (flags & MF_POPUP) it->sub = id;
    if ((flags & (MF_SEPARATOR | MF_BITMAP | MF_OWNERDRAW)) == 0 && lp) gstr_get(it->text, sizeof it->text, lp);
}

/* ---- resources --------------------------------------------------------------------------------- */
static uint32_t wstr(uint32_t *p, char *out, size_t cap) {
    size_t n = 0;
    for (;;) {
        uint32_t c = R16(*p); *p += 2u;
        if (!c) break;
        if (n + 1 < cap) out[n++] = (char)(c < 256u ? c : '?');
    }
    out[n] = 0;
    return (uint32_t)n;
}
static void parse_items(uint32_t *p, uint32_t h) {
    Menu *m;
    for (;;) {
        MItem it;
        uint32_t fl = R16(*p), id = 0;
        char s[96];
        *p += 2u;
        memset(&it, 0, sizeof it);
        if (fl & MF_POPUP) {
            uint32_t sub;
            wstr(p, s, sizeof s);
            sub = new_menu();
            parse_items(p, sub);
            it.flags = fl & ~MF_END; it.id = sub; it.sub = sub;
        } else {
            id = R16(*p); *p += 2u;
            wstr(p, s, sizeof s);
            it.flags = fl & ~MF_END; it.id = id;
            if (fl == 0 && id == 0 && s[0] == 0) it.flags = MF_SEPARATOR;
        }
        snprintf(it.text, sizeof it.text, "%s", s);
        m = menu_of(h);
        if (m) add_at(m, -1, &it);
        if (fl & MF_END) break;
    }
}

/* ---- imports ---------------------------------------------------------------------------------- */
static uint32_t m_LoadMenuA(void) {
    uint32_t sz, a, p, h;
    if (ARG(1) >= 0x10000u) return 0;                          /* by name: the game only uses ids */
    a = res_find(4, ARG(1), &sz);
    if (!a) return 0;
    p = a + 4u + R16(a + 2u);
    h = new_menu();
    parse_items(&p, h);
    return h;
}
static uint32_t m_CreateMenu(void) { return new_menu(); }
static uint32_t m_DestroyMenu(void) { free_menu(ARG(0)); dirty = 1; return 1; }
static uint32_t m_Append(void) {
    Menu *m = menu_of(ARG(0));
    MItem it;
    if (!m) return 0;
    fill_item(&it, ARG(1), ARG(2), ARG(3));
    add_at(m, -1, &it); dirty = 1;
    return 1;
}
static uint32_t m_Insert(void) {      /* InsertMenuA(hMenu, uPosition, uFlags, uIDNewItem, lpNewItem) */
    Menu *m = menu_of(ARG(0)), *loc = 0;
    MItem it;
    int pos;
    if (!m) return 0;
    if (ARG(1) == 0xFFFFFFFFu) pos = -1;
    else if (ARG(2) & MF_BYPOSITION) pos = ARG(1) > (uint32_t)m->n ? -1 : (int)ARG(1);
    else { pos = locate(ARG(0), ARG(1), 0, &loc); if (pos < 0 || loc != m) { pos = -1; } }
    fill_item(&it, ARG(2), ARG(3), ARG(4));
    add_at(m, pos, &it); dirty = 1;
    return 1;
}
static uint32_t m_Modify(void) {      /* ModifyMenuA(hMenu, uPosition, uFlags, uIDNewItem, lpNewItem) */
    Menu *m = 0;
    MItem it;
    int pos = locate(ARG(0), ARG(1), ARG(2), &m);
    if (pos < 0) return 0;
    fill_item(&it, ARG(2), ARG(3), ARG(4));
    m->it[pos] = it; dirty = 1;
    return 1;
}
static uint32_t m_Delete(void) {
    Menu *m = 0;
    int pos = locate(ARG(0), ARG(1), ARG(2), &m);
    if (pos < 0) return 0;
    del_at(m, pos, 1); dirty = 1;
    return 1;
}
static uint32_t m_Remove(void) {
    Menu *m = 0;
    int pos = locate(ARG(0), ARG(1), ARG(2), &m);
    if (pos < 0) return 0;
    del_at(m, pos, 0); dirty = 1;
    return 1;
}
static uint32_t m_EnableMenuItem(void) {
    Menu *m = 0;
    uint32_t old, f = ARG(2);
    int pos = locate(ARG(0), ARG(1), f, &m);
    if (pos < 0) return 0xFFFFFFFFu;
    old = m->it[pos].flags & (MF_GRAYED | MF_DISABLED);
    m->it[pos].flags = (m->it[pos].flags & ~(MF_GRAYED | MF_DISABLED)) | (f & (MF_GRAYED | MF_DISABLED));
    dirty = 1;
    return old;
}
static uint32_t m_CheckMenuItem(void) {
    Menu *m = 0;
    uint32_t old, f = ARG(2);
    int pos = locate(ARG(0), ARG(1), f, &m);
    if (pos < 0) return 0xFFFFFFFFu;
    old = m->it[pos].flags & MF_CHECKED;
    m->it[pos].flags = (m->it[pos].flags & ~MF_CHECKED) | (f & MF_CHECKED);
    dirty = 1;
    return old;
}
static uint32_t m_GetSubMenu(void) {
    Menu *m = menu_of(ARG(0));
    if (!m || ARG(1) >= (uint32_t)m->n || !(m->it[ARG(1)].flags & MF_POPUP)) return 0;
    return m->it[ARG(1)].sub;
}
static uint32_t m_GetMenuItemCount(void) { Menu *m = menu_of(ARG(0)); return m ? (uint32_t)m->n : 0xFFFFFFFFu; }
static uint32_t m_GetMenuItemID(void) {
    Menu *m = menu_of(ARG(0));
    if (!m || ARG(1) >= (uint32_t)m->n) return 0xFFFFFFFFu;
    return (m->it[ARG(1)].flags & MF_POPUP) ? 0xFFFFFFFFu : m->it[ARG(1)].id;
}
static uint32_t m_GetMenu(void) { int i = wm_idx(ARG(0)); return i < 0 ? 0 : wm[i].bar; }
static uint32_t m_SetMenu(void) {
    int i = wm_idx(ARG(0));
    if (i < 0) return 0;
    wm[i].bar = menu_of(ARG(1)) ? ARG(1) : 0; dirty = 1;
    return 1;
}
static uint32_t m_DrawMenuBar(void) { dirty = 1; return 1; }

static void default_sysmenu(uint32_t h) {
    static const struct { uint32_t id; const char *t; } d[] = {
        {0xF120, "&Restore"}, {0xF010, "&Move"}, {0xF000, "&Size"}, {0xF020, "Mi&nimize"}, {0xF030, "Ma&ximize"},
        {0, 0}, {0xF060, "&Close\tAlt+F4"}};
    Menu *m = menu_of(h);
    for (unsigned i = 0; i < sizeof d / sizeof d[0]; i++) {
        MItem it;
        memset(&it, 0, sizeof it);
        if (d[i].t) { it.id = d[i].id; snprintf(it.text, sizeof it.text, "%s", d[i].t); }
        else it.flags = MF_SEPARATOR;
        add_at(m, -1, &it);
    }
}
/* GetSystemMenu(hwnd, bRevert): revert discards the modified menu; the next request creates the default */
static uint32_t m_GetSystemMenu(void) {
    int i = wm_idx(ARG(0));
    if (i < 0) return 0;
    if (ARG(1)) { free_menu(wm[i].sys); wm[i].sys = 0; dirty = 1; return 0; }
    if (!wm[i].sys) { wm[i].sys = new_menu(); default_sysmenu(wm[i].sys); dirty = 1; }
    return wm[i].sys;
}

/* ---- mirror to the host -------------------------------------------------------------------------- */
static void clean_text(char *dst, size_t cap, const char *s) {
    size_t n = 0;
    for (; *s && *s != '\t' && n + 1 < cap; s++) {
        if (*s == '&') { if (s[1] == '&') { dst[n++] = '&'; s++; } continue; }
        dst[n++] = *s;
    }
    dst[n] = 0;
}
static void conv_item(HostMenuItem *o, const MItem *it) {
    memset(o, 0, sizeof *o);
    clean_text(o->text, sizeof o->text, it->text);
    o->id = it->id;
    o->grayed = (it->flags & (MF_GRAYED | MF_DISABLED)) != 0;
    o->checked = (it->flags & MF_CHECKED) != 0;
    o->separator = (it->flags & MF_SEPARATOR) != 0;
}
static void conv_children(HostMenuItem *o, uint32_t h) {
    Menu *m = menu_of(h);
    o->nchild = 0; o->child = 0;
    if (!m || !m->n) return;
    o->child = (HostMenuItem *)calloc((size_t)m->n, sizeof(HostMenuItem));
    for (int i = 0; i < m->n; i++) {
        conv_item(&o->child[i], &m->it[i]);
        if (m->it[i].flags & MF_POPUP) conv_children(&o->child[i], m->it[i].sub);
    }
    o->nchild = m->n;
}
static void free_conv(HostMenuItem *o) {
    for (int i = 0; i < o->nchild; i++) free_conv(&o->child[i]);
    free(o->child);
}

void menu_sync(void) {
    uint32_t mw;
    int i, n = 0, sys;
    uint32_t src;
    Menu *m;
    HostMenuItem *top;
    if (!dirty) return;
    dirty = 0;
    mw = user_main_window();
    i = wm_idx(mw);
    if (i < 0) return;
    sys = wm[i].bar == 0;
    src = sys ? wm[i].sys : wm[i].bar;
    m = menu_of(src);
    top = (HostMenuItem *)calloc((size_t)((m ? m->n : 0) + 1), sizeof *top);
    for (int k = 0; m && k < m->n; k++) {
        const MItem *it = &m->it[k];
        if (it->flags & MF_SEPARATOR) continue;
        if (sys && !(it->flags & MF_POPUP) && it->id >= 0xF000u) continue;      /* standard system items */
        conv_item(&top[n], it);
        if (it->flags & MF_POPUP) conv_children(&top[n], it->sub);
        else {                                    /* top-level command -> menu with one item */
            HostMenuItem *c = (HostMenuItem *)calloc(1, sizeof *c);
            conv_item(c, it);
            top[n].child = c; top[n].nchild = 1; top[n].id = 0;
        }
        n++;
    }
    host_set_menu(top, n, sys);
    for (int k = 0; k < n; k++) free_conv(&top[k]);
    free(top);
}

void menu_register(void) {
#define R(n, f) win32_register("USER32.dll!" n, f)
    R("LoadMenuA", m_LoadMenuA); R("CreateMenu", m_CreateMenu); R("CreatePopupMenu", m_CreateMenu);
    R("DestroyMenu", m_DestroyMenu);
    R("AppendMenuA", m_Append); R("InsertMenuA", m_Insert); R("ModifyMenuA", m_Modify);
    R("DeleteMenu", m_Delete); R("RemoveMenu", m_Remove);
    R("EnableMenuItem", m_EnableMenuItem); R("CheckMenuItem", m_CheckMenuItem);
    R("GetSubMenu", m_GetSubMenu); R("GetMenuItemCount", m_GetMenuItemCount); R("GetMenuItemID", m_GetMenuItemID);
    R("GetMenu", m_GetMenu); R("SetMenu", m_SetMenu); R("DrawMenuBar", m_DrawMenuBar);
    R("GetSystemMenu", m_GetSystemMenu);
#undef R
}
