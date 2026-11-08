/* GetComputerName on the Mac: the machine's network name, without domain, capped at the Windows 15-character limit */
#ifndef HOSTNAME_POSIX_H
#define HOSTNAME_POSIX_H
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static void hostname_short(char *out, size_t cap) {
    char n[256];
    char *dot;
    if (gethostname(n, sizeof n) != 0) n[0] = 0;
    n[sizeof n - 1] = 0;
    if ((dot = strchr(n, '.')) != NULL) *dot = 0;
    if (!n[0] || !strcmp(n, "localhost")) snprintf(n, sizeof n, "GPM2");
    n[15] = 0;
    snprintf(out, cap, "%s", n);
}
#endif
