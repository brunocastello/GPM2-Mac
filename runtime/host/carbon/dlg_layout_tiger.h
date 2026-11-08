/* Tiger (Aqua HIG 10.4): Lucida Grande 13, 20-high button and popup, 16 margin, 12 between buttons, right-aligned
 * labels; the default button sits on the right, Cancel to its left and the secondary action at the left margin. */
#ifndef DLG_LAYOUT_TIGER_H
#define DLG_LAYOUT_TIGER_H
#include <stdint.h>
#include "../host.h"
typedef struct { const char *dlg; uint32_t id; const char *text; int x, y, w, h; const char *label; int hide; } DlgRow;
typedef struct { const char *dlg; int w, h; } DlgSize;
enum { DLG_CHAT_M = HOST_DLG_MARGIN, DLG_CHAT_MB = HOST_DLG_MARGIN, DLG_CHAT_BW = 72, DLG_CHAT_RH = 22, DLG_CHAT_GAP = 8, DLG_CHAT_BTN_DY = 1 };
static const DlgSize dlg_sizes[] = {
    {"#102", 348,  52}, {"#103", 440, 260}, {"#104", 364, 120}, {"#107", 372, 120}, {"#139", 372, 200}, {"#140", 372, 200}, {"#137", 432, 304},
};
static const DlgRow dlg_rows[] = {
    {"#102",  1007, NULL,                16,  16,  84,  20, NULL, 0},
    {"#102",  1003, NULL,              152,  16,  84,  20, NULL, 0},
    {"#102",  1006, NULL,              248,  16,  84,  20, NULL, 0},
    {"#103", 65535, "Game",              16,  16, 104,  16, "Game:", 0},
    {"#103",  1015, NULL,              128,  16, 296,  16, NULL, 0},
    {"#103", 65535, "Your Team",         16,  44, 104,  16, "Your Team:", 0},
    {"#103",  1013, NULL,              128,  42, 216,  20, NULL, 0},
    {"#103", 65535, "Players Joined",    16,  78, 104,  16, "Players Joined:", 0},
    {"#103",  1012, NULL,              128,  78, 296, 124, NULL, 0},
    {"#103", 65535, "Game Details",         0,   0,   0,   0, NULL, 1},
    {"#103",     3, NULL,                16, 224, 108,  20, NULL, 0},
    {"#103",     2, NULL,              196, 224, 108,  20, NULL, 0},
    {"#103",  1001, NULL,              316, 224, 108,  20, NULL, 0},
    {"#104", 65535, "Game",              16,  16,  84,  16, "Game:", 0},
    {"#104",  1015, NULL,              108,  16, 240,  16, NULL, 0},
    {"#104", 65535, "Your Team",         16,  44,  84,  16, "Your Team:", 0},
    {"#104",  1013, NULL,              108,  42, 240,  20, NULL, 0},
    {"#104",  1010, NULL,              240,  84, 108,  20, NULL, 0},
    {"#107", 65535, NULL,                16,  16, 340,  16, NULL, 0},
    {"#107",  1008, NULL,                16,  40, 340,  22, NULL, 0},
    {"#107",     2, NULL,              184,  84,  80,  20, NULL, 0},
    {"#107",     1, NULL,              276,  84,  80,  20, NULL, 0},
    {"#139",  1000, NULL,                16,  16, 340, 128, NULL, 0},
    {"#139",     2, NULL,              184, 164,  80,  20, NULL, 0},
    {"#139",     1, NULL,              276, 164,  80,  20, NULL, 0},
    {"#140",  1006, NULL,                16,  16, 340, 128, NULL, 0},
    {"#140",     2, NULL,              184, 164,  80,  20, NULL, 0},
    {"#140",     1, NULL,              276, 164,  80,  20, NULL, 0},
    {"#137", 65535, "On New Messages", 16, 17, 136, 17, "On New Messages:", 0},
    {"#137",  1011, NULL, 160, 16, 256, 18, NULL, 0},
    {"#137",  1033, NULL, 160, 40, 256, 18, NULL, 0},
    {"#137",  1012, NULL, 160, 64, 256, 18, NULL, 0},
    {"#137", 65535, "After Sending", 16, 97, 136, 17, "After Sending:", 0},
    {"#137",  1013, NULL, 160, 96, 256, 18, NULL, 0},
    {"#137",  1034, NULL, 160, 120, 256, 18, NULL, 0},
    {"#137", 65535, "Hot Key", 16, 153, 136, 17, "Hot Key:", 0},
    {"#137", 61751, NULL, 160, 149, 220, 20, NULL, 0},
    {"#137",  1016, NULL, 160, 180, 256, 18, NULL, 0},
    {"#137", 65535, "Send To Players", 16, 213, 136, 17, "Send To Players:", 0},
    {"#137",  1029, NULL, 160, 212, 122, 18, NULL, 0},
    {"#137",  1030, NULL, 160, 236, 122, 18, NULL, 0},
    {"#137",  1031, NULL, 290, 212, 126, 18, NULL, 0},
    {"#137",  1032, NULL, 290, 236, 126, 18, NULL, 0},
    {"#137",     2, NULL, 244, 268, 80, 20, NULL, 0},
    {"#137",     1, NULL, 336, 268, 80, 20, NULL, 0},
};
#endif
