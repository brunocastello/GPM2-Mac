/* Fixes for original bugs via hooks. Portable C99. Each fix is an option;
 * when off, the behavior is identical to the original. Default: on (the fuzz/insn tests
 * turn everything off with fixes_set_all(0), since they compare the generated C with the original x86). */
#ifndef GPM2_HOOKS_H
#define GPM2_HOOKS_H

enum { FIX_BUG022, FIX_BUG024, FIX_BUG025, FIX_BUG026, FIX_COUNT };

int fix_on(int id);
void fix_set(int id, int on);
void fixes_set_all(int on);
const char *fix_id(int id);          /* "BUG-024" */
const char *fix_label(int id);       /* short text for the Fixes tab */
/* "BUG-024" -> index, or -1 */
int fix_find(const char *bug);
/* "BUG-024=on|off"; returns 0 if ok */
int fix_parse_arg(const char *arg);

#endif
