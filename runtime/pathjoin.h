/* Host path join without duplicating the separator: "a/" + "b" and "a" + "/b" give "a/b". */
#ifndef GPM2_PATHJOIN_H
#define GPM2_PATHJOIN_H
#include <stdio.h>
#include <string.h>

static inline void path_join(char *out, size_t cap, const char *dir, const char *name) {
    size_t dl = strlen(dir);
    while (dl > 1 && dir[dl - 1] == '/') dl--;                 /* a lone "/" stays "/" */
    while (*name == '/') name++;
    snprintf(out, cap, "%.*s%s%s", (int)dl, dir, (dl == 1 && dir[0] == '/') ? "" : "/", name);
}
/* strips trailing slashes (keeps "/") */
static inline void path_strip_slash(char *p) {
    size_t l = strlen(p);
    while (l > 1 && p[l - 1] == '/') p[--l] = 0;
}
#endif
