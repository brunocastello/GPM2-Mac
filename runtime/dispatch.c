/* Indirect calls and jumps: guest address -> C function. */
#include "machine.h"

/* host yield hook (Mac OS 9, cooperative multitasking): called every 16 imports; NULL on the other backends */
void (*dispatch_yield_hook)(void);

void dispatch(uint32_t va) {
    if (va >= IMPORT_MAGIC_BASE && va < IMPORT_MAGIC_BASE + 4u * import_count && (va & 3u) == 0) {
        static unsigned yield_n;
        if (dispatch_yield_hook && (++yield_n & 15u) == 0) dispatch_yield_hook();
        import_table[(va - IMPORT_MAGIC_BASE) / 4u].fn();
        return;
    }
    if (va >= THUNK_MAGIC_BASE && va < IMPORT_MAGIC_BASE && guest_thunk_hook) {
        guest_thunk_hook(va);
        return;
    }
    unsigned lo = 0, hi = dispatch_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2u;
        uint32_t k = dispatch_table[mid].va;
        if (k == va) { dispatch_table[mid].fn(); return; }
        if (k < va) lo = mid + 1u; else hi = mid;
    }
    guest_trap("dispatch", va);
}

void dispatch_jump(uint32_t va) {
    if (shadow_top() == va) {          /* return by jump (_chkstk) */
        (void)shadow_pop();
        return;
    }
    dispatch(va);                      /* tail call */
}
