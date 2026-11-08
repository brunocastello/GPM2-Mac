/* current macOS (Apple HIG): normal-size font and controls, 16 margin, right-aligned labels,
 * default button on the right, Cancel to its left and secondary action on the left margin. */
#ifndef DLG_LAYOUT_MODERN_H
#define DLG_LAYOUT_MODERN_H
#include <stdint.h>
#include "../host.h"
typedef struct { const char *dlg; uint32_t id; const char *text; int x, y, w, h; const char *label; int hide; } DlgRow;
typedef struct { const char *dlg; int w, h; } DlgSize;
enum { DLG_CHAT_M = HOST_DLG_MARGIN, DLG_CHAT_MB = HOST_DLG_MARGIN, DLG_CHAT_BW = 72, DLG_CHAT_RH = 22, DLG_CHAT_GAP = 8, DLG_CHAT_BTN_DY = 1 };
static const DlgSize dlg_sizes[] = {
    {"#102", 332,  52}, {"#103", 412, 250}, {"#104", 352, 118}, {"#107", 352, 118}, {"#139", 352, 192}, {"#140", 352, 192}, {"#137", 412, 304},
};
static const DlgRow dlg_rows[] = {
    {"#102",  1007, NULL,                16,  16,  80,  20, NULL, 0},
    {"#102",  1003, NULL,              144,  16,  80,  20, NULL, 0},
    {"#102",  1006, NULL,              236,  16,  80,  20, NULL, 0},
    {"#103", 65535, "Game",              16,  16, 100,  17, "Game:", 0},
    {"#103",  1015, NULL,              124,  16, 272,  17, NULL, 0},
    {"#103", 65535, "Your Team",         16,  43, 100,  17, "Your Team:", 0},
    {"#103",  1013, NULL,              124,  40, 272,  22, NULL, 0},
    {"#103", 65535, "Players Joined",    16,  76, 100,  17, "Players Joined:", 0},
    {"#103",  1012, NULL,              124,  74, 272, 120, NULL, 0},
    {"#103", 65535, "Game Details",         0,   0,   0,   0, NULL, 1},
    {"#103",     3, NULL,                16, 214, 100,  20, NULL, 0},
    {"#103",     2, NULL,              184, 214, 100,  20, NULL, 0},
    {"#103",  1001, NULL,              296, 214, 100,  20, NULL, 0},
    {"#104", 65535, "Game",              16,  16,  80,  17, "Game:", 0},
    {"#104",  1015, NULL,              104,  16, 232,  17, NULL, 0},
    {"#104", 65535, "Your Team",         16,  43,  80,  17, "Your Team:", 0},
    {"#104",  1013, NULL,              104,  40, 232,  22, NULL, 0},
    {"#104",  1010, NULL,              236,  82, 100,  20, NULL, 0},
    {"#107", 65535, NULL,                16,  16, 320,  17, NULL, 0},
    {"#107",  1008, NULL,                16,  40, 320,  22, NULL, 0},
    {"#107",     2, NULL,              164,  82,  80,  20, NULL, 0},
    {"#107",     1, NULL,              256,  82,  80,  20, NULL, 0},
    {"#139",  1000, NULL,                16,  16, 320, 120, NULL, 0},
    {"#139",     2, NULL,              164, 156,  80,  20, NULL, 0},
    {"#139",     1, NULL,              256, 156,  80,  20, NULL, 0},
    {"#140",  1006, NULL,                16,  16, 320, 120, NULL, 0},
    {"#140",     2, NULL,              164, 156,  80,  20, NULL, 0},
    {"#140",     1, NULL,              256, 156,  80,  20, NULL, 0},
    {"#137", 65535, "On New Messages", 16, 16, 128, 17, "On New Messages:", 0},
    {"#137",  1011, NULL, 152, 16, 244, 18, NULL, 0},
    {"#137",  1033, NULL, 152, 40, 244, 18, NULL, 0},
    {"#137",  1012, NULL, 152, 64, 244, 18, NULL, 0},
    {"#137", 65535, "After Sending", 16, 96, 128, 17, "After Sending:", 0},
    {"#137",  1013, NULL, 152, 96, 244, 18, NULL, 0},
    {"#137",  1034, NULL, 152, 120, 244, 18, NULL, 0},
    {"#137", 65535, "Hot Key", 16, 152, 128, 17, "Hot Key:", 0},
    {"#137", 61751, NULL, 152, 149, 200, 22, NULL, 0},
    {"#137",  1016, NULL, 152, 180, 244, 18, NULL, 0},
    {"#137", 65535, "Send To Players", 16, 212, 128, 17, "Send To Players:", 0},
    {"#137",  1029, NULL, 152, 212, 116, 18, NULL, 0},
    {"#137",  1030, NULL, 152, 236, 116, 18, NULL, 0},
    {"#137",  1031, NULL, 276, 212, 120, 18, NULL, 0},
    {"#137",  1032, NULL, 276, 236, 120, 18, NULL, 0},
    {"#137",     2, NULL, 224, 268, 80, 20, NULL, 0},
    {"#137",     1, NULL, 316, 268, 80, 20, NULL, 0},
};
#endif
