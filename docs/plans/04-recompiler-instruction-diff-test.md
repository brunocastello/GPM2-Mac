# F1: per-instruction differential test (translated C x Unicorn)

**Parent:** 03-recompiler-translate-to-c (F1). **Date:** 2026-11-02..
**User priority:** run on the Mac. Everything runs in the `f1-fuzz-macos` job
(`macos-latest`, arm64). Linux is not used.

## Why

The per-function fuzz (`tests/fuzz.py`) does not reach about 37 of the 154 forms:
- some 20 only exist in functions with an indirect call in the closure;
- the others are on paths that the random cases do not walk.

Moreover, it **does not compare flags**. A flag error only shows up when it changes a
jump. This test executes **one real instruction of the exe at a time** on both sides
and compares registers, flags, memory and x87.

## Constraints

The same as 02-recompiler-code-discovery/b:
- nothing installed on the Mac;
- Python 3.9+ with only the stdlib (plus `capstone` and `unicorn` on the runner);
- no code copied from GPL projects;
- commits in Portuguese with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

The generated code goes to `gen/` (ignored by git).

## Files

```
recomp/insn_gen.py          generates gen/insn_tests.c + gen/insn_tests.json
tests/insn_driver.c         C driver (translated-C side), compiled with -DGUEST_CHECK
tests/insn_test.py          generates cases, runs Unicorn (parallel, fork) and the driver; compares; report
runtime/Makefile            new target `insn` -> build/insn_driver
runtime/machine.[ch]        if needed: fl_set_eflags(uint32_t) and fl_get_eflags(void)
.github/workflows/recomp.yml   steps in the f1-fuzz-macos job (after the fuzz; `if: always()`)
```

## Generation (`recomp/insn_gen.py`, run as `python -m recomp insn bin/Gpm2.exe gen/`)

1. Use the F0/F1 discovery and disassembly (`recomp.discover`, `recomp.disasm`),
   the same path that the `f1` subcommand uses. Walk all the instructions of
   all the functions and group them by form (`ins.form`, the same key as
   `operand_forms.csv`).
2. For each form, choose up to **8 instances** that are distinct by bytes, with
   deterministic order (increasing VA, spaced across the list).
3. **Excluded, and listed in the report with the reason:** control flow
   (`jmp`, `jcc`, `call`, `ret`, indirect `jmp`/`call`, `loop`) and `int3`/`hlt`.
   - The `jcc` conditions are covered by the flag comparison. The driver
     evaluates all the `c_*()` after each instruction (see Comparison).
   - `push`/`pop`/`leave` **are included**.
4. For each instance, emit `static void t_<n>(void) { <lines of translate(ins)> }`.
   - Before each function, the comment with VA, bytes and text.
   - A table `{n, va, len, form}`.
   - Use `recomp.translate.translate(ins)` directly. If the form needs
     emitter context (for example, the `fs` prefix), follow what `emit.py` does.
5. `gen/insn_tests.json` carries, per instance:
   - `n`, `va`, `bytes` (hex), `form`, `mn`;
   - memory operands: base, index, scale, disp, size, `fs`;
   - registers read and written;
   - whether it is a string (`rep`), x87, division or shift;
   - which flags are defined by the instruction. Undefined ones are not compared:
     - `div`/`idiv`: all;
     - `mul`/`imul`: SF, ZF;
     - `shl`/`shr`/`sar`: OF when the count != 1, and nothing is compared if the
       count `& 31` = 0;
     - `bsf`/`bsr` (if they exist): ZF only.

## Cases (`tests/insn_test.py`)

**64 cases per instance**, with a fixed seed (`va`, `k`).

- **Scratch area** `0x00800000–0x00900000`, filled with random bytes
  from the seed, equal on both sides.
- **Random registers** (mix of small values, bytes, words and the extremes
  `0x80000000`/`0x7FFFFFFF`/`0xFFFFFFFF`).
- **Memory operand `[base + idx*scale + disp]`:** draw an EA aligned to the
  size inside the scratch area (with 64 bytes of slack) and solve the base
  register: `base = EA - disp - idx*scale`.
  - The index gets a small value (0..15).
  - If base and index are the same register, solve the equation.
  - **Absolute address** (no base or index): use the image address
    as is, and the whole page enters the comparison.
- **`esp`/`ebp`:** within `0x003F0000–0x003FF000` (stack), for
  `push`/`pop`/`leave`/`[ebp+k]`.
  - When the memory operand uses `esp`/`ebp` as base, the EA falls on the stack;
    in that case the stack is also compared.
- **Strings:**
  - `ecx` in 0..40;
  - `esi`/`edi` at distinct points of the scratch area;
  - `al`/`eax` random; for `repne scasb`, half of the cases put the searched
    byte in the range.
- **`div`/`idiv`:** half of the cases with divisor and `edx:eax` chosen so the
  quotient fits, half random. A case in which **both sides** fail
  (`#DE` in Unicorn, `TRAP divide` in C) counts as equal. A case in which only one side
  fails is a divergence.
- **Initial flags:** random EFLAGS in the bits CF, PF, AF, ZF, SF and OF, plus the
  bit 1, always on. DF = 0.
  - In C, use `fl_set_eflags()`. If it does not exist, implement it in `machine.c`
    as an explicit lazy state, for example `CC_CFOF` with cf/zf/sf/of
    stored, without changing the semantics of the other operations.
  - This tests `adc`/`sbb`/`inc`/`dec`/`setcc` with an input CF.
- **x87:**
  - Push 0 to 3 random doubles on both sides: partly
    "nice" values (integers, 0, -1, 0.5), partly random, and sometimes equal
    to each other, so `fcom` gives equal.
  - **Unicorn:** a stub of `fninit`, `fldcw 0x027F` and k x `fld qword [addr]`
    before the instruction (like the `fuzz.py` stub).
  - **C:** `fx_push` in the same order and `g.sw = 0`.
  - The `m64`/`m32` operand comes from the scratch area, with the chosen value.

## Execution

**Unicorn** (processes with `fork`, one per core):
1. Map 64 MB and copy the image (reuse `build_image` and the `fs` setup
   from `tests/fuzz.py`, importing from there or moving to a common module
   `tests/uni_common.py`).
2. Write the instruction at `0x00F30000`, with the x87 stub before it, if any.
3. Execute up to `0x00F30000 + len`, with `count` = number of stub instructions + 1.

**C driver:** receives one line per case on stdin and returns one line per case.
1. Loads the image and applies the state.
2. Calls `t_<n>()` inside `sigsetjmp`, with the trap and fault hooks as in
   `fuzz_driver.c`.
3. Prints the result.

Run the driver in parallel batches with restart after a crash, as in `fuzz.py`.

## Comparison

- **Registers:** `eax..edi`, `esp`, `ebp`.
- **Defined flags** (CF, ZF, SF, OF; PF and AF are not compared): in C,
  `fl_cf()` etc. In addition, compare the 12 conditions `c_e … c_ns` against the
  same conditions computed from Unicorn's EFLAGS, masking those that depend on an
  undefined flag.
- **Memory:** SHA-256 of the scratch area + of the stack range `0x003F0000–0x00400000`
  + of the page of each absolute address used; on divergence, the first 16
  differing bytes.
- **x87:**
  - `top`;
  - the C0/C2/C3 bits of `sw`;
  - the values of the live ST0..ST(k-1). Read those from Unicorn as 80 bits
    (`UC_X86_REG_FP0..7` relative to `top`) and convert to double.
  - Exact equality. If there is a difference only in the last bit, record it as a
    separate "x87 precision" item, but count it as a divergence.

## Report `out/recomp-f1/insn.md` and `insn.json`

- Forms tested / total.
- Excluded forms and the reason.
- Instances and cases.
- Divergences per form, with VA, bytes, text, seed, input state and
  difference.
- Discarded cases (both sides failed the same way), by reason.
- Total time, with a target below 10 min on the runner.
- Summary in `$GITHUB_STEP_SUMMARY`.
- **Script exit:** 0 if there is no divergence, 1 if there is.

## Fixes

Every real divergence in the translator (`recomp/translate.py`, `emit.py`) or in the
runtime (`runtime/machine.*`) is fixed and noted in the Log of 03-recompiler-translate-to-c,
with the instruction, the cause and the fix.

- **Do not** change the x87 model (53-bit double with FPCW 0x027F) without recording it.
- A divergence that is a harness defect is fixed in the harness and noted as
  such.

## Acceptance (complements the F1 acceptance in 03-recompiler-translate-to-c)

- All non-control-flow forms tested, with **0 divergences**.
- The per-function fuzz (`fuzz.py`) with 0 divergences and >= 300 functions.
  - The per-function fuzz target ">= 130 forms" now applies as a **union**
    (per-function fuzz U per-instruction test U control-flow forms
    exercised in the fuzz).
  - `fuzz.py` does not need to change its exit rule, but the final job summary
    must state both things.
