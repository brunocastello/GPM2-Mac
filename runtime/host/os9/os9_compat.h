/* Minimal POSIX compatibility for the Mac OS 9 port: force-included (-include) in every unit.
 * Retro68 only has the basic libc; here the POSIX calls the core uses become versions over the File Manager.
 * Host paths in this port: "/Volume/Folder/file" (slashes); the conversion to "Volume:Folder:file" is internal. */
#ifndef GPM2_OS9_COMPAT_H
#define GPM2_OS9_COMPAT_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

typedef struct os9_dir os9_DIR;
struct os9_dirent { char d_name[256]; };
FILE *os9_fopen(const char *path, const char *mode);
int os9_open(const char *path, int flags, ...);
int os9_stat(const char *path, struct stat *st);
int os9_fstat(int fd, struct stat *st);
int os9_mkdir(const char *path, int mode);
int os9_unlink(const char *path);
os9_DIR *os9_opendir(const char *path);
struct os9_dirent *os9_readdir(os9_DIR *d);
int os9_closedir(os9_DIR *d);
char *os9_realpath(const char *path, char *out);
void os9_usleep(unsigned us);
struct tm *os9_localtime_r(const time_t *t, struct tm *out);
struct tm *os9_gmtime_r(const time_t *t, struct tm *out);
void os9_tick(void);                              /* host_os9.c: pumps events (at most every ~16 ms) */
void os9_pump(unsigned ms);                       /* host_os9.c: one step of the event loop; waits up to ms */

#ifndef OS9_NO_REDIRECT                                   /* os9_posix.c and host_os9.c use the real names */
#define DIR os9_DIR
#define dirent os9_dirent
#define opendir os9_opendir
#define readdir os9_readdir
#define closedir os9_closedir
#define fopen os9_fopen
#define open os9_open
#define stat(p, s) os9_stat(p, s)
#define fstat(f, s) os9_fstat(f, s)
#define mkdir os9_mkdir
#define unlink os9_unlink
#define realpath os9_realpath
#define usleep os9_usleep
#define localtime_r os9_localtime_r
#define gmtime_r os9_gmtime_r
#define getpid() 1
#endif
#endif
