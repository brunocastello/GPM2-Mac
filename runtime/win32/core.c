/* Import table, unimplemented(), guest_call, log/trace, guest strings. */
#include "win32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int win32_trace;
int win32_slow_trace;   /* host links with the log: records every import (including host calls) that exceeds 20 ms */
int win32_stub_log = 1;

#define MAX_REG 1024
static struct { const char *name; Win32Fn fn; } regs[MAX_REG];
static unsigned nregs;

#define MAX_IMP 512
static Win32Fn impls[MAX_IMP];
static unsigned counts[MAX_IMP];
static unsigned cur_idx;
static unsigned long long call_seq;
static unsigned long long call_seq_n;
static FILE *trace_f;

#define MAX_STUB 256
static const char *stubs[MAX_STUB];
static unsigned nstubs;

void win32_register(const char *name, Win32Fn fn) {
    if (nregs >= MAX_REG) { fprintf(stderr, "win32_register: table full\n"); exit(1); }
    regs[nregs].name = name; regs[nregs].fn = fn; nregs++;
}

void win32_log(const char *fmt, ...) {
    va_list ap;
    fputs("[win32] ", stderr);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}

static uint32_t cur_ra;

void win32_mem_report(const char *why) {
    uint32_t used, nused, maxfree;
    unsigned cnt[8];
    unsigned long long bmb;
    heap_stats(&used, &nused, &maxfree);
    gdi_stats(cnt, &bmb);
    win32_log("mem (%s): heap %u KB in %u blocks (largest free %u KB); GDI dc=%u bitmap=%u (%llu KB) pal=%u brush=%u pen=%u font=%u; calls=%llu",
              why, used >> 10, nused, maxfree >> 10, cnt[1], cnt[2], bmb >> 10, cnt[3], cnt[4], cnt[5], cnt[6], call_seq_n);
}

NORETURN void win32_die(const char *fmt, ...) {
    va_list ap;
    char msg[512], full[900];
    va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    snprintf(full, sizeof full, "%s\n\nLast call: %s (ra=%08x)", msg, import_count ? import_table[cur_idx].name : "?", cur_ra);
    fprintf(stderr, "[win32] FATAL: %s\n", full);
    win32_mem_report("fatal");
    fflush(stderr);
    host_message_box("GPM2 - fatal error", full, 0x10);
    exit(3);
}

void win32_stub_note(const char *name) {
    for (unsigned i = 0; i < nstubs; i++) if (stubs[i] == name) return;
    if (nstubs < MAX_STUB) stubs[nstubs++] = name;
    if (win32_stub_log) win32_log("stub: %s", name);
}

const char *win32_cur_import(void) { return import_table[cur_idx].name; }
unsigned win32_cur_idx(void) { return cur_idx; }

/* ---- guest strings ------------------------------------------------------------------ */
uint8_t *guest_ptr(uint32_t addr, uint32_t len) {
    if ((uint64_t)addr + len > MEM_SIZE) guest_fault(addr, len);
    return M + addr;
}
uint32_t gstr_len(uint32_t addr) {
    uint32_t n = 0;
    while (R8(addr + n)) n++;
    return n;
}
void gstr_get(char *dst, size_t cap, uint32_t addr) {
    size_t i = 0;
    if (cap == 0) return;
    if (addr == 0) { dst[0] = 0; return; }
    for (; i + 1 < cap; i++) {
        uint32_t c = R8(addr + (uint32_t)i);
        if (!c) break;
        dst[i] = (char)c;
    }
    dst[i] = 0;
}
void gstr_put(uint32_t addr, const char *s) {
    uint32_t n = (uint32_t)strlen(s);
    gmem_put(addr, s, n + 1u);
}
void gstr_putn(uint32_t addr, uint32_t cap, const char *s) {
    uint32_t n = (uint32_t)strlen(s);
    if (cap == 0) return;
    if (n > cap - 1u) n = cap - 1u;
    gmem_put(addr, s, n);
    W8(addr + n, 0);
}
void gmem_get(void *dst, uint32_t addr, uint32_t n) { memcpy(dst, guest_ptr(addr, n), n); }
void gmem_put(uint32_t addr, const void *src, uint32_t n) { memcpy(guest_ptr(addr, n), src, n); }
int gstr_cmp_ci(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}

/* ---- guest_call ------------------------------------------------------------------------ */
#define CALL_SENTINEL 0x00F00010u

static uint32_t call_common(uint32_t addr, unsigned n, va_list ap) {
    uint32_t args[16], s_esp = g.esp, s_ebx = g.ebx, s_esi = g.esi, s_edi = g.edi, s_ebp = g.ebp;
    uint32_t r;
    if (n > 16) win32_die("guest_call: too many arguments");
    for (unsigned i = 0; i < n; i++) args[i] = va_arg(ap, uint32_t);
    for (unsigned i = n; i > 0; i--) PUSH(args[i - 1]);
    CALLPUSH(CALL_SENTINEL);
    dispatch(addr);
    r = g.eax;
    g.esp = s_esp;
    g.ebx = s_ebx; g.esi = s_esi; g.edi = s_edi; g.ebp = s_ebp;
    return r;
}
uint32_t guest_call(uint32_t addr, unsigned n, ...) {
    va_list ap; uint32_t r;
    va_start(ap, n); r = call_common(addr, n, ap); va_end(ap);
    return r;
}
uint32_t guest_call_cdecl(uint32_t addr, unsigned n, ...) {
    va_list ap; uint32_t r;
    va_start(ap, n); r = call_common(addr, n, ap); va_end(ap);
    return r;
}

/* ---- import dispatch --------------------------------------------------------------------- */
static int printable_str(uint32_t a, char *out, size_t cap) {
    size_t i;
    if (a < 0x1000u || (uint64_t)a + 4 >= MEM_SIZE) return 0;
    for (i = 0; i + 1 < cap; i++) {
        uint32_t c;
        if ((uint64_t)a + i >= MEM_SIZE) return 0;
        c = M[a + (uint32_t)i];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7e) return 0;
        out[i] = (char)c;
    }
    out[i] = 0;
    return i >= 2;
}

static void fmt_args(char *buf, size_t cap, unsigned n) {
    size_t o = 0;
    buf[0] = 0;
    for (unsigned i = 0; i < n && o + 80 < cap; i++) {
        uint32_t v = R32(g.esp + 4u + 4u * i);
        char s[48];
        o += (size_t)snprintf(buf + o, cap - o, " %08x", v);
        if (printable_str(v, s, sizeof s)) o += (size_t)snprintf(buf + o, cap - o, "(\"%s\")", s);
    }
}

static NORETURN void unimplemented(unsigned idx, const char *name) {
    char a[1024];
    fmt_args(a, sizeof a, 8);
    fprintf(stderr, "[win32] UNIMPLEMENTED IMPORT: %s (idx %u)\n  return: 0x%08x\n  args:%s\n",
            name, idx, R32(g.esp), a);
    fflush(stderr);
    win32_summary();
    exit(3);
}

static void import_hook(unsigned idx, const char *name, unsigned pop) {
    uint32_t ra = R32(g.esp), r;
    Win32Fn fn = idx < MAX_IMP ? impls[idx] : 0;
    cur_idx = idx; cur_ra = ra;
    if ((++call_seq_n & 0xFFFFFu) == 0) win32_mem_report("periodico");
    if (!fn) unimplemented(idx, name);
    counts[idx]++;
    if (win32_trace && trace_f) {
        char a[512];
        fmt_args(a, sizeof a, pop / 4u ? pop / 4u : 3u);
        fprintf(trace_f, "%llu t=%u %s ra=%08x%s\n", ++call_seq, win32_now_ms(), name, ra, a);
    }
    if (win32_slow_trace) {
        unsigned t0 = win32_now_ms(), dt;
        r = fn();
        dt = win32_now_ms() - t0;
        if (dt > 20u) { fprintf(stderr, "[slow] import %s: %u ms (t=%u ra=%08x)\n", name, dt, t0, ra); fflush(stderr); }
    } else r = fn();
    g.eax = r;
    g.esp += 4u + pop;
    RETCHK(ra);
    if (win32_trace && trace_f) fprintf(trace_f, "   -> %08x\n", r);
}

#define MAX_THUNK 128
static struct { Win32Fn fn; unsigned pop; } thunks[MAX_THUNK];
static unsigned nthunks;

uint32_t win32_thunk(Win32Fn fn, unsigned pop) {
    if (nthunks >= MAX_THUNK) win32_die("too many thunks");
    thunks[nthunks].fn = fn; thunks[nthunks].pop = pop;
    return THUNK_MAGIC_BASE + 4u * nthunks++;
}

static void thunk_hook(uint32_t va) {
    unsigned i = (va - THUNK_MAGIC_BASE) / 4u;
    uint32_t ra = R32(g.esp), r;
    if (i >= nthunks || (va & 3u)) guest_trap("thunk", va);
    r = thunks[i].fn();
    g.eax = r;
    g.esp += 4u + thunks[i].pop;
    RETCHK(ra);
}

void win32_init(void) {
    const char *t = getenv("GPM2_TRACE");
    if (import_count > MAX_IMP) win32_die("too many imports");
    for (unsigned i = 0; i < import_count; i++) {
        impls[i] = 0;
        for (unsigned k = 0; k < nregs; k++)
            if (strcmp(regs[k].name, import_table[i].name) == 0) { impls[i] = regs[k].fn; break; }
    }
    if (t && t[0] == '1') {
        win32_trace = 1;
        trace_f = fopen("out/trace.txt", "w");
        if (!trace_f) { win32_trace = 0; win32_log("no out/trace.txt"); }
    }
    guest_import_hook = import_hook;
    guest_thunk_hook = thunk_hook;
}

void win32_summary(void) {
    unsigned used = 0, total = 0;
    unsigned long long calls = 0;
    fprintf(stderr, "[win32] summary of imports called:\n");
    for (unsigned i = 0; i < import_count; i++) {
        if (!counts[i]) continue;
        used++; calls += counts[i];
        fprintf(stderr, "  %6u  %s\n", counts[i], import_table[i].name);
    }
    for (unsigned i = 0; i < import_count; i++) if (impls[i]) total++;
    fprintf(stderr, "[win32] %u distinct imports called, %llu calls, %u implemented of %u\n",
            used, calls, total, import_count);
    if (nstubs) {
        fprintf(stderr, "[win32] stubs used:");
        for (unsigned i = 0; i < nstubs; i++) fprintf(stderr, " %s", stubs[i]);
        fputc('\n', stderr);
    }
    win32_mem_report("end");
    if (trace_f) fflush(trace_f);
}

void win32_exit(int code) {
    win32_summary();
    fflush(stderr);
    exit(code);
}

#ifdef GUEST_CHECK
void guest_note_write(uint32_t addr, uint32_t n) { (void)addr; (void)n; }   /* only the fuzz needs the write trace */
#endif
