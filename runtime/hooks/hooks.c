/* Bug-fix hooks. The hookpt_* functions are called by the generated C before an
 * instruction (recomp/hooks.json); they return 0 to follow the normal flow. */
#include "hooks/hooks.h"
#include "machine.h"
#include "funcs.h"

#include <string.h>

static int fix_state[FIX_COUNT] = {1, 1, 1, 1};
static const char *const fix_ids[FIX_COUNT] = {"BUG-022", "BUG-024", "BUG-025", "BUG-026"};
static const char *const fix_labels[FIX_COUNT] = {
    "Engines 13 and 14 get their own manufacturer (new career)",
    "Load DEFAULTS.GCF (version 0) without misalignment or error",
    "Chat messages received while the chat is closed are kept",
    "Send Window To Back does not reload a stale view state",
};

int fix_on(int id) { return id >= 0 && id < FIX_COUNT && fix_state[id]; }
void fix_set(int id, int on) { if (id >= 0 && id < FIX_COUNT) fix_state[id] = on != 0; }
void fixes_set_all(int on) { for (int i = 0; i < FIX_COUNT; i++) fix_state[i] = on != 0; }
const char *fix_id(int id) { return fix_ids[id]; }
const char *fix_label(int id) { return fix_labels[id]; }
int fix_find(const char *bug) {
    for (int i = 0; i < FIX_COUNT; i++) if (!strcmp(bug, fix_ids[i])) return i;
    return -1;
}
int fix_parse_arg(const char *arg) {
    const char *eq = strchr(arg, '=');
    char id[16];
    if (!eq || eq - arg >= (long)sizeof id) return 1;
    memcpy(id, arg, (size_t)(eq - arg));
    id[eq - arg] = 0;
    int i = fix_find(id);
    if (i < 0) return 1;
    if (!strcmp(eq + 1, "on")) fix_set(i, 1);
    else if (!strcmp(eq + 1, "off")) fix_set(i, 0);
    else return 1;
    return 0;
}

/* BUG-022: FUN_00431410 (VA 0x431410) initializes the manufacturer of each engine in a local array at
 * [ebp-0x70 + i]; entries 12 and 13 hold 2 (manufacturer of engine 3). Before entry 14 is written
 * (0x431597), fixes both to 12 and 13. */
int hookpt_bug022_engmaker(void) {
    if (fix_on(FIX_BUG022)) {
        W8(g.ebp - 0x64u, 0x0Cu);
        W8(g.ebp - 0x63u, 0x0Du);
    }
    return 0;
}

/* BUG-024: the .GCF reader (FUN_004adb60) uses the 30-entry table only when version == 1;
 * DEFAULTS.GCF (version 0) has 30. Before `cmp eax, 1` (0x4ae353; eax = version), version 0 becomes 1. */
int hookpt_bug024_gcfver(void) {
    if (fix_on(FIX_BUG024) && (g.eax & 0xFFFFFFFFu) == 0) g.eax = 1;
    return 0;
}

/* BUG-026: FUN_004f50e0 (chat Send Window To Back; brings view <param> to the front) ends with FUN_004f81f0(param),
 * which reloads the view from the copy saved by FUN_004f7d80 (WinG DC and pointer, 0x2f80-byte tables...). With the view already
 * current, that copy is stale and the game stops responding to clicks. Before `mov eax,[ebp+8]` (0x4f51c6): if the view is already
 * the current one (DAT_0071b33c), skips the push, the call and the `add esp,4` (up to 0x4f51d2). */
int hookpt_bug026_viewrestore(void) {
    return fix_on(FIX_BUG026) && R32(g.ebp + 8u) == R32(0x71B33Cu);
}
