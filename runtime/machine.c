/* Global state, lazy flags, x87, strings, traps. */
#include "machine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

Cpu g;
uint8_t *M;

void (*guest_trap_hook)(const char *what, uint32_t arg);
void (*guest_fault_hook)(uint32_t addr, uint32_t size);

NORETURN void guest_fault(uint32_t addr, uint32_t size) {
    if (guest_fault_hook) guest_fault_hook(addr, size);
    fprintf(stderr, "guest fault: invalid access at 0x%08x (%u bytes)\n", addr, size);
    abort();
}

NORETURN void guest_trap(const char *what, uint32_t arg) {
    if (guest_trap_hook) guest_trap_hook(what, arg);
    fprintf(stderr, "guest trap: %s (0x%08x)\n", what, arg);
    abort();
}

void (*guest_import_hook)(unsigned idx, const char *name, unsigned pop);
void (*guest_thunk_hook)(uint32_t va);

void guest_import(unsigned idx, const char *name, unsigned pop) {
    if (guest_import_hook) {
        guest_import_hook(idx, name, pop);
        return;
    }
    guest_trap(name, 0);
}

/* ---- shadow stack --------------------------------------------------------- */
#define SHADOW_MAX 262144u
static uint32_t shadow[SHADOW_MAX];
static uint32_t shadow_n;

void shadow_push(uint32_t ra) {
    if (shadow_n >= SHADOW_MAX) guest_trap("call stack too deep", ra);
    shadow[shadow_n++] = ra;
}
uint32_t shadow_pop(void) {
    if (shadow_n == 0) guest_trap("shadow stack empty", 0);
    return shadow[--shadow_n];
}
uint32_t shadow_top(void) { return shadow_n ? shadow[shadow_n - 1] : 0; }
void shadow_reset(void) { shadow_n = 0; }

void machine_init(void) {
    if (!M) {
        M = (uint8_t *)calloc(MEM_SIZE, 1);
        if (!M) { fprintf(stderr, "out of memory for the guest\n"); exit(1); }
    }
    memset(&g, 0, sizeof g);
    g.esp = STACK_TOP;
    shadow_reset();
}

/* ---- flags ------------------------------------------------------------------ */
static uint32_t sign_(void) { return 1u << (g.cc_sz - 1u); }

int fl_cf(void) {
    switch (g.cc_op) {
    case CC_ADD: return g.cc_r < g.cc_a;
    case CC_ADC: return g.cc_c ? g.cc_r <= g.cc_a : g.cc_r < g.cc_a;
    case CC_SUB: return g.cc_a < g.cc_b;
    case CC_SBB: return g.cc_c ? g.cc_a <= g.cc_b : g.cc_a < g.cc_b;
    case CC_INC: case CC_DEC: case CC_CFOF: case CC_EFL: return (int)g.cc_c;
    default: return 0;
    }
}

int fl_of(void) {
    uint32_t s = sign_();
    switch (g.cc_op) {
    case CC_ADD: case CC_ADC: return ((g.cc_a ^ g.cc_r) & (g.cc_b ^ g.cc_r) & s) != 0;
    case CC_SUB: case CC_SBB: return ((g.cc_a ^ g.cc_b) & (g.cc_a ^ g.cc_r) & s) != 0;
    case CC_INC: return g.cc_r == s;
    case CC_DEC: return g.cc_r == s - 1u;
    case CC_CFOF: case CC_EFL: return (int)g.cc_a;
    default: return 0;
    }
}

int fl_zf(void) { return g.cc_op == CC_EFL ? (int)(g.cc_b & 1u) : g.cc_r == 0; }
int fl_sf(void) { return g.cc_op == CC_EFL ? (int)((g.cc_b >> 1) & 1u) : (g.cc_r & sign_()) != 0; }

/* CC_EFL: cf in cc_c, of in cc_a, zf in bit 0 and sf in bit 1 of cc_b */
void fl_set_eflags(uint32_t e) {
    g.cc_op = CC_EFL; g.cc_sz = 32;
    g.cc_c = e & 1u;                        /* CF */
    g.cc_a = (e >> 11) & 1u;                /* OF */
    g.cc_b = ((e >> 6) & 1u) | (((e >> 7) & 1u) << 1);   /* ZF, SF */
    g.cc_r = 0;
}

uint32_t op_shl(uint32_t a, uint32_t c, uint32_t sz) {
    uint32_t r = (a << c) & szmask_(sz);
    uint32_t cf = c <= sz ? (a >> (sz - c)) & 1u : 0u;
    uint32_t of = (((a << (c - 1u)) ^ r) >> (sz - 1u)) & 1u;
    cc_set_(CC_CFOF, sz, of, 0, r); g.cc_c = cf;
    return r;
}

uint32_t op_shr(uint32_t a, uint32_t c, uint32_t sz) {
    uint32_t r = a >> c;
    uint32_t t = a >> (c - 1u);
    cc_set_(CC_CFOF, sz, (((t ^ r) >> (sz - 1u)) & 1u), 0, r); g.cc_c = t & 1u;
    return r;
}

static uint32_t sar32_(uint32_t x, uint32_t c) {
    /* explicit arithmetic shift (>> on a negative int32_t is not guaranteed) */
    return (x & 0x80000000u) ? ~((~x) >> c) : (x >> c);
}

uint32_t op_sar(uint32_t a, uint32_t c, uint32_t sz) {
    uint32_t x = sz == 32 ? a : (sz == 16 ? SX16(a) : SX8(a));
    uint32_t r = sar32_(x, c) & szmask_(sz);
    uint32_t t = sar32_(x, c - 1u);
    cc_set_(CC_CFOF, sz, 0, 0, r); g.cc_c = t & 1u;
    return r;
}

uint32_t op_imul32(uint32_t a, uint32_t b) {
    int64_t p = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    uint32_t r = (uint32_t)p;
    uint32_t ov = p != (int64_t)(int32_t)r;
    cc_set_(CC_CFOF, 32, ov, 0, r); g.cc_c = ov;
    return r;
}

void op_div32(uint32_t d, uint32_t va) {
    uint64_t n = ((uint64_t)g.edx << 32) | g.eax;
    if (d == 0) guest_trap("divide", va);
    uint64_t q = n / d;
    if (q > 0xFFFFFFFFu) guest_trap("divide", va);
    g.edx = (uint32_t)(n % d);
    g.eax = (uint32_t)q;
}

void op_idiv32(uint32_t d, uint32_t va) {
    int64_t n = (int64_t)(((uint64_t)g.edx << 32) | g.eax);
    int32_t s = (int32_t)d;
    int64_t q, rem;
    if (s == 0) guest_trap("divide", va);
    if (s == -1) {
        if (n == INT64_MIN) guest_trap("divide", va);
        q = -n; rem = 0;
    } else {
        q = n / s; rem = n % s;
    }
    if (q < INT32_MIN || q > INT32_MAX) guest_trap("divide", va);
    g.eax = (uint32_t)(int32_t)q;
    g.edx = (uint32_t)(int32_t)rem;
}

/* ---- strings (DF = 0 always) -------------------------------------------------- */
static void movs_one_(uint32_t n) {
    uint32_t v = n == 1 ? R8(g.esi) : (n == 2 ? R16(g.esi) : R32(g.esi));
    if (n == 1) W8(g.edi, v); else if (n == 2) W16(g.edi, v); else W32(g.edi, v);
    g.esi += n; g.edi += n;
}
void str_movs(uint32_t n) { movs_one_(n); }
void str_rep_movs(uint32_t n) {
    while (g.ecx) { movs_one_(n); g.ecx--; }
}
static void stos_one_(uint32_t n) {
    if (n == 1) W8(g.edi, g.eax); else if (n == 2) W16(g.edi, g.eax); else W32(g.edi, g.eax);
    g.edi += n;
}
void str_stos(uint32_t n) { stos_one_(n); }
void str_rep_stos(uint32_t n) {
    while (g.ecx) { stos_one_(n); g.ecx--; }
}
void str_repne_scasb(void) {
    while (g.ecx) {
        uint32_t b = R8(g.edi), a = g.eax & 0xFFu;
        uint32_t r = alu_sub(a, b, 8);
        g.edi += 1u; g.ecx--;
        if (r == 0) break;
    }
}

/* ---- x87 (double, 53 bits) ----------------------------------------------------- */
#define ST0 g.st[g.top & 7u]

void fx_push(double v) { g.top = (g.top - 1u) & 7u; ST0 = v; }
void fx_pop(void) { g.top = (g.top + 1u) & 7u; }
double fx_st0(void) { return ST0; }
void fx_set0(double v) { ST0 = v; }
void fx_mul(double v) { ST0 = ST0 * v; }
void fx_div(double v) { ST0 = ST0 / v; }
void fx_chs(void) { ST0 = -ST0; }
void fx_compare(double v) {
    double s = ST0;
    g.sw &= ~0x4700u;                              /* C0, C1, C2, C3 */
    if (isnan(s) || isnan(v)) g.sw |= 0x4500u;     /* unordered: C3 = C2 = C0 = 1 */
    else if (s < v) g.sw |= 0x0100u;               /* C0 */
    else if (s == v) g.sw |= 0x4000u;              /* C3 */
}
uint32_t fx_status(void) { return (g.sw & ~0x3800u) | ((g.top & 7u) << 11); }
