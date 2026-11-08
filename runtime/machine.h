/* x86-32 machine model of the recompiler.
 *
 * Global state `g`, guest memory `M` (64 MB, addresses 0x00000000-0x03FFFFFF),
 * lazy flags (as in QEMU) and x87 as `double`. Priority: correctness.
 */
#ifndef GPM2_MACHINE_H
#define GPM2_MACHINE_H

#include <stdint.h>
#include <string.h>

/* Host byte order. The guest memory `M` is always little-endian (x86); on a big-endian
 * host (PowerPC) the R16/R32/W16/W32/R64D/W64D accessors swap the bytes. Outside them the core
 * only reads/writes guest memory byte by byte or through these accessors. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define GPM2_BIG_ENDIAN 1
#elif !defined(__BYTE_ORDER__) && (defined(__ppc__) || defined(__POWERPC__) || defined(__BIG_ENDIAN__))
#define GPM2_BIG_ENDIAN 1
#endif
#ifdef GPM2_BIG_ENDIAN
#define LE16_(v) ((uint16_t)__builtin_bswap16((uint16_t)(v)))
#define LE32_(v) ((uint32_t)__builtin_bswap32((uint32_t)(v)))
#define LE64_(v) ((uint64_t)__builtin_bswap64((uint64_t)(v)))
#else
#define LE16_(v) ((uint16_t)(v))
#define LE32_(v) ((uint32_t)(v))
#define LE64_(v) ((uint64_t)(v))
#endif

#ifndef MEM_SIZE
#define MEM_SIZE          0x04000000u
#endif
#define STACK_TOP         0x00400000u
#define STACK_LOW         0x00100000u
#define FS_BASE           0x00010000u
#define IMPORT_MAGIC_BASE 0xFFF00000u
#define THUNK_MAGIC_BASE  0xFFE00000u      /* host C functions exposed to the guest at run time (COM vtables) */

#define NORETURN __attribute__((noreturn))

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    /* lazy flags */
    uint32_t cc_op, cc_sz, cc_a, cc_b, cc_c, cc_r;
    /* x87 */
    double   st[8];
    uint32_t top, sw;
} Cpu;

extern Cpu g;
extern uint8_t *M;

/* ---- guest errors -------------------------------------------------------- */
NORETURN void guest_fault(uint32_t addr, uint32_t size);
NORETURN void guest_trap(const char *what, uint32_t arg);
/* Import stub: without a hook, aborts with the name; with a hook (differential test)
 * the hook emulates the import including the `ret` (removes `pop` bytes of arguments). */
void guest_import(unsigned idx, const char *name, unsigned pop);
extern void (*guest_import_hook)(unsigned idx, const char *name, unsigned pop);
/* optional hook (the fuzz driver uses it to return via longjmp) */
extern void (*guest_trap_hook)(const char *what, uint32_t arg);
extern void (*guest_fault_hook)(uint32_t addr, uint32_t size);

/* ---- memory ----------------------------------------------------------------- */
#ifdef GUEST_CHECK
void guest_note_write(uint32_t addr, uint32_t n);
static inline void chk_(uint32_t a, uint32_t n) {
    if ((uint64_t)a + n > MEM_SIZE) guest_fault(a, n);
}
static inline void chkw_(uint32_t a, uint32_t n) {
    chk_(a, n);
    if (a + n > 0x00800000u) guest_note_write(a, n);
}
#else
#define chk_(a, n)  ((void)0)
#define chkw_(a, n) ((void)0)
#endif

static inline uint32_t R8(uint32_t a) { chk_(a, 1); return M[a]; }
static inline uint32_t R16(uint32_t a) { uint16_t v; chk_(a, 2); memcpy(&v, M + a, 2); return LE16_(v); }
static inline uint32_t R32(uint32_t a) { uint32_t v; chk_(a, 4); memcpy(&v, M + a, 4); return LE32_(v); }
static inline void W8(uint32_t a, uint32_t v) { chkw_(a, 1); M[a] = (uint8_t)v; }
static inline void W16(uint32_t a, uint32_t v) { uint16_t t = LE16_(v); chkw_(a, 2); memcpy(M + a, &t, 2); }
static inline void W32(uint32_t a, uint32_t v) { uint32_t t = LE32_(v); chkw_(a, 4); memcpy(M + a, &t, 4); }
static inline double R64D(uint32_t a) { uint64_t u; double v; chk_(a, 8); memcpy(&u, M + a, 8); u = LE64_(u); memcpy(&v, &u, 8); return v; }
static inline void W64D(uint32_t a, double v) { uint64_t u; chkw_(a, 8); memcpy(&u, &v, 8); u = LE64_(u); memcpy(M + a, &u, 8); }

/* ---- sub-registers --------------------------------------------------------- */
#define GET8L(r)   ((uint32_t)(uint8_t)(r))
#define GET8H(r)   ((uint32_t)(uint8_t)((r) >> 8))
#define GET16(r)   ((uint32_t)(uint16_t)(r))
#define SET8L(r, v) ((r) = ((r) & 0xFFFFFF00u) | (uint32_t)(uint8_t)(v))
#define SET8H(r, v) ((r) = ((r) & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(v) << 8))
#define SET16(r, v) ((r) = ((r) & 0xFFFF0000u) | (uint32_t)(uint16_t)(v))
#define SX8(v)     ((uint32_t)(int32_t)(int8_t)(v))
#define SX16(v)    ((uint32_t)(int32_t)(int16_t)(v))

/* ---- stack and returns ------------------------------------------------------------ */
/* Shadow stack of expected return addresses (always maintained; ret only
 * checks it under GUEST_CHECK). Also serves `jmp reg` used as a return (_chkstk). */
void shadow_push(uint32_t ra);
uint32_t shadow_pop(void);
uint32_t shadow_top(void);
void shadow_reset(void);

static inline void PUSH(uint32_t v) { g.esp -= 4u; W32(g.esp, v); }
static inline void CALLPUSH(uint32_t ra) { PUSH(ra); shadow_push(ra); }
static inline void RETCHK(uint32_t ra) {
    uint32_t e = shadow_pop();
#ifdef GUEST_CHECK
    if (e != ra) guest_trap("unbalanced stack", ra);
#else
    (void)e; (void)ra;
#endif
}

/* ---- lazy flags -------------------------------------------------------------- */
enum { CC_LOGIC = 0, CC_ADD, CC_ADC, CC_SUB, CC_SBB, CC_INC, CC_DEC, CC_CFOF, CC_EFL };

int fl_cf(void);
int fl_zf(void);
int fl_sf(void);
int fl_of(void);
/* Explicit state from EFLAGS (CF, ZF, SF, OF; per-instruction differential test). */
void fl_set_eflags(uint32_t eflags);

static inline int c_e(void)  { return fl_zf(); }
static inline int c_ne(void) { return !fl_zf(); }
static inline int c_l(void)  { return fl_sf() != fl_of(); }
static inline int c_ge(void) { return fl_sf() == fl_of(); }
static inline int c_le(void) { return fl_zf() || fl_sf() != fl_of(); }
static inline int c_g(void)  { return !fl_zf() && fl_sf() == fl_of(); }
static inline int c_a(void)  { return !fl_cf() && !fl_zf(); }
static inline int c_ae(void) { return !fl_cf(); }
static inline int c_b(void)  { return fl_cf(); }
static inline int c_be(void) { return fl_cf() || fl_zf(); }
static inline int c_s(void)  { return fl_sf(); }
static inline int c_ns(void) { return !fl_sf(); }

static inline uint32_t szmask_(uint32_t sz) { return sz == 32 ? 0xFFFFFFFFu : ((1u << sz) - 1u); }
static inline void cc_set_(uint32_t op, uint32_t sz, uint32_t a, uint32_t b, uint32_t r) {
    g.cc_op = op; g.cc_sz = sz; g.cc_a = a; g.cc_b = b; g.cc_r = r;
}

/* Operands `a`, `b` already truncated to size `sz` (8/16/32); so is the result. */
static inline uint32_t alu_add(uint32_t a, uint32_t b, uint32_t sz) {
    uint32_t r = (a + b) & szmask_(sz); cc_set_(CC_ADD, sz, a, b, r); return r;
}
static inline uint32_t alu_adc(uint32_t a, uint32_t b, uint32_t sz) {
    uint32_t cin = (uint32_t)fl_cf();
    uint32_t r = (a + b + cin) & szmask_(sz);
    cc_set_(CC_ADC, sz, a, b, r); g.cc_c = cin; return r;
}
static inline uint32_t alu_sub(uint32_t a, uint32_t b, uint32_t sz) {
    uint32_t r = (a - b) & szmask_(sz); cc_set_(CC_SUB, sz, a, b, r); return r;
}
static inline uint32_t alu_sbb(uint32_t a, uint32_t b, uint32_t sz) {
    uint32_t cin = (uint32_t)fl_cf();
    uint32_t r = (a - b - cin) & szmask_(sz);
    cc_set_(CC_SBB, sz, a, b, r); g.cc_c = cin; return r;
}
static inline uint32_t alu_and(uint32_t a, uint32_t b, uint32_t sz) {
    uint32_t r = a & b; cc_set_(CC_LOGIC, sz, a, b, r); return r;
}
static inline uint32_t alu_or(uint32_t a, uint32_t b, uint32_t sz) {
    uint32_t r = a | b; cc_set_(CC_LOGIC, sz, a, b, r); return r;
}
static inline uint32_t alu_xor(uint32_t a, uint32_t b, uint32_t sz) {
    uint32_t r = a ^ b; cc_set_(CC_LOGIC, sz, a, b, r); return r;
}
/* inc/dec preserve CF */
static inline uint32_t alu_inc(uint32_t a, uint32_t sz) {
    uint32_t cf = (uint32_t)fl_cf();
    uint32_t r = (a + 1u) & szmask_(sz);
    cc_set_(CC_INC, sz, a, 1u, r); g.cc_c = cf; return r;
}
static inline uint32_t alu_dec(uint32_t a, uint32_t sz) {
    uint32_t cf = (uint32_t)fl_cf();
    uint32_t r = (a - 1u) & szmask_(sz);
    cc_set_(CC_DEC, sz, a, 1u, r); g.cc_c = cf; return r;
}
/* neg: like sub 0 - a (CF = a != 0) */
static inline uint32_t alu_neg(uint32_t a, uint32_t sz) { return alu_sub(0u, a, sz); }

/* Shifts: count `c` already in 1..31 (count 0 changes nothing: the caller filters it). */
uint32_t op_shl(uint32_t a, uint32_t c, uint32_t sz);
uint32_t op_shr(uint32_t a, uint32_t c, uint32_t sz);
uint32_t op_sar(uint32_t a, uint32_t c, uint32_t sz);

/* imul r32, r/m32: CF = OF = signed overflow */
uint32_t op_imul32(uint32_t a, uint32_t b);
/* div/idiv of edx:eax by d; flags undefined (unchanged). Trap on divide by zero/overflow. */
void op_div32(uint32_t d, uint32_t va);
void op_idiv32(uint32_t d, uint32_t va);

/* ---- strings (DF = 0) ---------------------------------------------------------------------- */
/* `rep movs{b,w,d}`: n = element size in bytes */
void str_rep_movs(uint32_t n);
void str_movs(uint32_t n);
void str_rep_stos(uint32_t n);
void str_stos(uint32_t n);
void str_repne_scasb(void);

/* ---- x87 ------------------------------------------------------------------------------------ */
void fx_push(double v);
void fx_pop(void);
double fx_st0(void);
void fx_set0(double v);
void fx_mul(double v);
void fx_div(double v);
void fx_chs(void);
void fx_compare(double v);
uint32_t fx_status(void);

/* ---- control flow --------------------------------------------------------------------------------- */
/* indirect call/jump by guest address (function, thunk or magic import address) */
extern void (*guest_thunk_hook)(uint32_t va);
void dispatch(uint32_t va);
/* `jmp reg` outside a switch: if the target is the expected return (shadow stack), consumes it
 * (it is a return by jump, like _chkstk); otherwise dispatches as a tail call
 * (no push). In both cases the generated caller then does `return`. */
void dispatch_jump(uint32_t va);

/* ---- image ----------------------------------------------------------------------------------- */
typedef struct { uint32_t iat; void (*fn)(void); const char *name; } ImportEntry;
typedef struct { uint32_t va; void (*fn)(void); } DispatchEntry;
extern const ImportEntry import_table[];
extern const unsigned import_count;
extern void (*dispatch_yield_hook)(void);   /* dispatch.c: host yields the CPU every 16 imports (Mac OS 9 only) */
extern const DispatchEntry dispatch_table[];
extern const unsigned dispatch_count;

int image_load(const char *path);   /* 0 = ok */
void machine_init(void);            /* allocates M, zeroes the state */

#endif
