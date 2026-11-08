/* Minimal POSIX over the File Manager for the Mac OS 9 port + empty network layer (Link Play unavailable).
 * Host path: "/Volume/Folder/file"; the Mac uses "Volume:Folder:file". */
#include <Files.h>
#include <Errors.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>

#include "os9_compat.h"
#include "../host.h"

static void to_mac(const char *p, char *out, size_t cap) {
    size_t n = 0;
    int absolute = *p == '/';
    if (!absolute && !strchr(p, ':')) out[n++] = ':';                  /* relative */
    else if (absolute) p++;
    for (; *p && n + 1 < cap; p++) {
        if (*p == '/' && p[1] == '/') continue;                      /* "//" would become "::" (= parent folder in HFS) */
        out[n++] = *p == '/' ? ':' : *p;
    }
    if (absolute && n && !memchr(out, ':', n) && n + 1 < cap) out[n++] = ':';   /* "/Vol" = volume root; without ':' HFS would read a relative name (creating folder "Vol") */
    out[n] = 0;
}
static int to_spec(const char *p, FSSpec *sp) {
    char m[PATH_MAX];
    Str255 ps;
    size_t n;
    to_mac(p, m, sizeof m);
    n = strlen(m);
    if (n > 255) n = 255;
    ps[0] = (unsigned char)n; memcpy(ps + 1, m, n);
    return FSMakeFSSpec(0, 0, ps, sp);
}
static int info(const FSSpec *sp, CInfoPBRec *pb) {
    memset(pb, 0, sizeof *pb);
    pb->hFileInfo.ioNamePtr = (StringPtr)sp->name;
    pb->hFileInfo.ioVRefNum = sp->vRefNum;
    pb->hFileInfo.ioDirID = sp->parID;
    pb->hFileInfo.ioFDirIndex = 0;
    return PBGetCatInfoSync(pb);
}

FILE *os9_fopen(const char *path, const char *mode) {
    char m[PATH_MAX];
    to_mac(path, m, sizeof m);
    return fopen(m, mode);
}
int os9_open(const char *path, int flags, ...) {
    char m[PATH_MAX];
    int mode = 0666;
    va_list ap;
    va_start(ap, flags);
    if (flags & O_CREAT) mode = va_arg(ap, int);
    va_end(ap);
    to_mac(path, m, sizeof m);
    return open(m, flags, mode);
}
int os9_stat(const char *path, struct stat *st) {
    FSSpec sp;
    CInfoPBRec pb;
    if (to_spec(path, &sp) != noErr || info(&sp, &pb) != noErr) { errno = ENOENT; return -1; }
    memset(st, 0, sizeof *st);
    if (pb.hFileInfo.ioFlAttrib & 0x10) st->st_mode = S_IFDIR | 0755;
    else { st->st_mode = S_IFREG | 0644; st->st_size = (off_t)pb.hFileInfo.ioFlLgLen; }
    st->st_mtime = (time_t)(pb.hFileInfo.ioFlMdDat - 2082844800u);
    st->st_ctime = (time_t)(pb.hFileInfo.ioFlCrDat - 2082844800u);
    st->st_atime = st->st_mtime;
    return 0;
}
int os9_fstat(int fd, struct stat *st) {
    off_t cur = lseek(fd, 0, SEEK_CUR), end;
    if (cur < 0) return -1;
    end = lseek(fd, 0, SEEK_END);
    lseek(fd, cur, SEEK_SET);
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFREG | 0644; st->st_size = end;
    return 0;
}
int os9_mkdir(const char *path, int mode) {
    FSSpec sp;
    long id;
    OSErr e = to_spec(path, &sp);
    (void)mode;
    if (e == noErr) { errno = EEXIST; return -1; }
    if (e != fnfErr || FSpDirCreate(&sp, -1, &id) != noErr) { errno = ENOENT; return -1; }
    return 0;
}
int os9_unlink(const char *path) {
    FSSpec sp;
    if (to_spec(path, &sp) != noErr || FSpDelete(&sp) != noErr) { errno = ENOENT; return -1; }
    return 0;
}
char *os9_realpath(const char *path, char *out) {
    FSSpec sp;
    if (to_spec(path, &sp) != noErr) return NULL;
    strcpy(out, path);
    return out;
}

struct os9_dir { short vref; long dirid; short idx; struct os9_dirent e; };
os9_DIR *os9_opendir(const char *path) {
    FSSpec sp;
    CInfoPBRec pb;
    os9_DIR *d;
    if (to_spec(path, &sp) != noErr || info(&sp, &pb) != noErr || !(pb.hFileInfo.ioFlAttrib & 0x10)) return NULL;
    d = (os9_DIR *)calloc(1, sizeof *d);
    d->vref = sp.vRefNum; d->dirid = pb.dirInfo.ioDrDirID;
    return d;
}
struct os9_dirent *os9_readdir(os9_DIR *d) {
    CInfoPBRec pb;
    Str255 nm;
    memset(&pb, 0, sizeof pb);
    pb.dirInfo.ioNamePtr = nm;
    pb.dirInfo.ioVRefNum = d->vref;
    pb.dirInfo.ioDrDirID = d->dirid;
    pb.dirInfo.ioFDirIndex = ++d->idx;
    if (PBGetCatInfoSync(&pb) != noErr) return NULL;
    memcpy(d->e.d_name, nm + 1, nm[0]);
    d->e.d_name[nm[0]] = 0;
    return &d->e;
}
int os9_closedir(os9_DIR *d) { free(d); return 0; }

void os9_usleep(unsigned us) { os9_pump(us / 1000u); }
struct tm *os9_localtime_r(const time_t *t, struct tm *out) { struct tm *r = localtime(t); if (!r) return NULL; *out = *r; return out; }
struct tm *os9_gmtime_r(const time_t *t, struct tm *out) { struct tm *r = gmtime(t); if (!r) return NULL; *out = *r; return out; }

/* Retro68's newlib has neither lround nor cexp (gcc swaps the sin/cos pair for cexp); this file is compiled with -fno-builtin */
long lround(double x) { return (long)(x < 0 ? -floor(-x + 0.5) : floor(x + 0.5)); }
double _Complex cexp(double _Complex z) {
    double e = exp(__real__ z), y = __imag__ z, c = cos(y), s = sin(y);
    double _Complex r;
    __real__ r = e * c; __imag__ r = e * s;
    return r;
}
