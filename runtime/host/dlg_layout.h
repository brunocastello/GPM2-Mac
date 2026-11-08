/* Layout of the Link Play dialogs in the Mac backends: the frame is native, so the MS Sans Serif coordinates of the
 * template (and those the chat window computes in WM_SIZE) give way to a form following each system's convention.
 * Each backend defines its own tables (macos/dlg_layout_modern.h, carbon/dlg_layout_tiger.h) before including
 * this file. Presentation only: ids, events and control state remain the core's. */
#ifndef DLG_LAYOUT_H
#define DLG_LAYOUT_H
#include "host.h"
#include <stdio.h>
#include <string.h>
static int dlg_layout_has(const char *name, int chat) {
    if (chat) return 1;
    for (size_t i = 0; i < sizeof dlg_sizes / sizeof dlg_sizes[0]; i++)
        if (!strcmp(dlg_sizes[i].dlg, name)) return 1;
    return 0;
}
static void dlg_layout_size(const char *name, int *w, int *h) {
    for (size_t i = 0; i < sizeof dlg_sizes / sizeof dlg_sizes[0]; i++)
        if (!strcmp(dlg_sizes[i].dlg, name)) { *w = dlg_sizes[i].w; *h = dlg_sizes[i].h; return; }
}
static void dlg_layout_ctl(const char *name, int chat, int cw, int ch, HostCtl *c) {
    if (chat) {                                  /* list on top; field and Send on the same row */
        if (c->kind == HC_LIST) { c->x = DLG_CHAT_M; c->y = DLG_CHAT_M; c->w = cw - 2 * DLG_CHAT_M; c->h = ch - DLG_CHAT_M - DLG_CHAT_MB - DLG_CHAT_RH - DLG_CHAT_GAP; }
        else if (c->kind == HC_EDIT) { c->x = DLG_CHAT_M; c->y = ch - DLG_CHAT_MB - DLG_CHAT_RH; c->w = cw - 2 * DLG_CHAT_M - DLG_CHAT_BW - DLG_CHAT_GAP; c->h = DLG_CHAT_RH; }
        else if (c->kind == HC_PUSH || c->kind == HC_DEFPUSH) { c->x = cw - DLG_CHAT_M - DLG_CHAT_BW; c->y = ch - DLG_CHAT_MB - DLG_CHAT_RH + DLG_CHAT_BTN_DY; c->w = DLG_CHAT_BW; c->h = DLG_CHAT_RH - 2 * DLG_CHAT_BTN_DY; }
        return;
    }
    for (size_t i = 0; i < sizeof dlg_rows / sizeof dlg_rows[0]; i++) {
        const DlgRow *r = &dlg_rows[i];
        if (strcmp(r->dlg, name) || r->id != c->id || (r->text && strcmp(r->text, c->text))) continue;
        if (r->hide) { c->visible = 0; return; }
        c->x = r->x; c->y = r->y; c->w = r->w; c->h = r->h;
        if (r->label) {                          /* template frame or caption becomes the form label */
            c->kind = HC_STATIC; c->align = 2;
            snprintf(c->text, sizeof c->text, "%s", r->label);
        }
        return;
    }
}
#endif
