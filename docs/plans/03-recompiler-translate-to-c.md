# F1: translating all functions to C

**Parent:** 01-overview-static-recompilation §5 (F1). **Base:** 02-recompiler-code-discovery (F0 accepted; artifact
`extracted/dumps/recomp-f0/`). **Date:** 2026-11-02..

## Goal

1. `python -m recomp f1 bin/Gpm2.exe gen/` generates C for **all** 2,613
   functions discovered in F0.
2. The generated C + a minimal runtime compile with `clang -O1` on `macos-latest`
   (arm64) **and** on `ubuntu-latest` (for the differential test).
3. **Differential test:** real functions executed by the generated C and by Unicorn
   (original x86) with the same inputs give the same registers and the same
   memory.

Out of scope: implementing Windows APIs (F3). In F1 each import is a stub
that aborts with its name.

## Constraints

The same as 02-recompiler-code-discovery: nothing installed on the Mac, Python code compatible with 3.9,
no code copied from GPL projects. Commit messages in Portuguese, with
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. The generated C **is not
committed** (goes to `gen/`, ignored by git, and to an artifact).

## F0 facts that simplify the translation

- **60 mnemonics and 154 forms** (`recomp-f0/operand_forms.csv`). Everything in
  flat 32-bit, with 8-, 16- and 32-bit operands.
- **No `cld`/`std`:** DF = 0 always; `rep movs*`, `rep stos*` and `repne scasb`
  advance forward.
- **x87 only with 64-bit memory:** `fld/fst/fstp/fmul/fdiv/fcom/fcomp m64`,
  `fild m32`, `fchs` and `fnstsw ax` (followed by `test ah, imm8` + `jcc`).
- **`fs:` only in 2 forms** (`mov r32, fs:[m]` and `mov fs:[m], r32`), in the CRT's SEH.
- Indirect: 126 (`call [abs]`, `call [reg+k]`, `call reg`, 1 `jmp eax`).
  326 switches, all with a `cmp` bound.
- Calls to imports: `call dword ptr [IAT]` and `call`/`jmp` to the 259 thunks.

## Layout in ``

```
recomp/translate.py     instruction -> C (one function per form)
recomp/emit.py          function -> C (labels, goto, switch); files; tables
recomp/__main__.py      new subcommand `f1`
runtime/machine.h       Cpu, guest memory, access macros, lazy flags, x87
runtime/machine.c       global state, flags (on-demand computation), x87, traps
runtime/image.c         loads the PE from the file into guest memory; IAT -> magic addresses
runtime/dispatch.c      binary search address -> function (generated table)
runtime/Makefile        compiles runtime + gen/ (clang, -O1, -j); targets `lib` and `fuzz`
tests/fuzz_driver.c     executable of the differential test (generated-C side)
tests/fuzz.py           generates cases, runs Unicorn and the driver, compares
.github/workflows/recomp.yml   + jobs `f1-build-macos` and `f1-fuzz-linux`
```

Output generated in `gen/`:

- `gen/fXX.c`: about 64 files, grouped by address range and with
  similar size;
- `gen/funcs.h`: prototypes;
- `gen/dispatch_table.c`: sorted `{va, ptr}`, covering all functions and
  thunks;
- `gen/imports.c`: one stub per import;
- `gen/manifest.json`: functions, counts, SHA-256 of the exe, translator version.

## Machine model (`runtime/machine.h`)

- **Global state** `Cpu g;` with `eax..edi, esp, ebp` (`uint32_t`), the lazy
  flags, the explicit CF (needed because of `inc`/`dec`) and the x87:
  `double st[8]`, `top`, `sw`.
  - Generated functions: `void f_00401000(void)`.
  - For now performance does not matter; the priority is correctness.
- **Guest memory:** `uint8_t *M` (64 MB, `0x00000000–0x03FFFFFF`).
  - Access through `R8/R16/R32(a)` and `W8/W16/W32(a, v)` with `memcpy` (little-endian,
    unaligned allowed). The current host is little-endian; this is asserted in
    the code.
  - With `-DGUEST_CHECK`, every access checks the bounds and calls
    `guest_fault(addr)`. The fuzz driver uses this mode.
- **Image:** `image_load(path)` copies **all** sections of the exe to their VAs
  (including `.text`, because the `switch` index tables are read as
  data) and zeroes `.bss`.
  - Each IAT entry gets a magic address `0xFFF00000 + 4*i`.
  - `dispatch()` recognizes these addresses and calls the import's stub. This covers
    `mov reg, [IAT]` + `call reg`.
- **Stack:** initial `esp` at `0x00400000` (grows downward to
  `0x00100000`).
- **`fs`:** fixed base of a fake TIB at `0x00010000` (`fs:[0]` = SEH chain).
  There are no hardware exceptions on the host, so the chain only needs
  coherent reads and writes.

## Translation rules

**Control flow (`emit.py`):**

- One label per F0 block (`L_0042bf7a:`). Direct jumps become `goto`.
- **`call rel`:** `PUSH(next_va); f_target();`. The real return address is
  pushed because the code reads `[ebp+4]` and the arguments at `[esp+k]`.
- **`ret` / `ret N`:** `POP(tmp); g.esp += N; return;`. With `-DGUEST_CHECK`, a
  `ret` that returns to an address different from the pushed one calls `guest_trap`
  ("unbalanced stack"). For this, keep a shadow stack of expected returns
  (only in check mode).
- **Tail call** (`jmp`/`jcc`/fall-through to the entry of another function,
  `tailcalls.csv`): `f_target(); return;`.
- **`call [IAT]` and `call` to a thunk:** `PUSH(next_va); imp_DLL_name();`.
  - The stub implements the import's `ret`. The F1 stubs abort with the name of the
    import; in F3 they will also remove the `stdcall` arguments.
  - `jmp` to a thunk = tail call of the import.
- **Indirect `call`:** `PUSH(next_va); dispatch(target);`. An unknown target
  calls `guest_trap("dispatch", target)`.
- **Switch:** `switch (reg) { case k: goto L_target; ... default: guest_trap(...); }`,
  with the index register at the moment of the `jmp` and the targets from
  `switches.csv`. Repeated targets are grouped.
- **`jmp eax` (`0x539C98`):** `dispatch_jump(target)`. If the target is a
  known label inside the same function, it becomes `goto`; otherwise, `f_target(); return;`.
  Document what it does (CRT/SEH).

**Flags (lazy, as in QEMU):**

- Each instruction that alters flags writes `g.cc_op` (ADD/SUB/LOGIC/INC/DEC/SHL/
  SAR/SHR/NEG/ADC/SBB/MUL..., with size 8/16/32) and the operands and result
  needed.
- `jcc`, `setcc`, `adc`, `sbb` and `inc`/`dec` (which preserve CF) call `static inline`
  functions that compute the condition on demand.
- Semantic points to respect:
  - `inc`/`dec` do not touch CF;
  - a shift with count `& 31 == 0` does not alter any flag;
  - `neg` leaves CF = (src != 0);
  - `test`/`and`/`or`/`xor` clear CF and OF;
  - `imul r32, r/m32` (2 operands) has CF = OF = signed overflow;
  - in `div`/`idiv` the flags are undefined (they are not compared).
- **Optional fusion** (permitted optimization, not required): `cmp`/`test` immediately
  followed by `jcc` in the same block may become `if (direct condition)`, **provided
  that** the lazy state is also written.

**Arithmetic:**

- `cdq`, `idiv`/`div` in 64 bits.
- Division by zero or quotient out of range → `guest_trap("divide")`. On real x86
  this brings the game down; the trap records the address.
- Shifts: count `& 31`.
- `sar` uses an explicit arithmetic shift, because `>>` on a negative `int32_t` is
  not guaranteed in C.

**x87:**

- The value in memory is always `m64`, that is `double`, and `fild m32` converts from `int32_t`.
- `fcom`/`fcomp` write C0/C2/C3 into `g.sw`:
  - less → C0;
  - equal → C3;
  - NaN → C0 = C2 = C3.
- `fnstsw ax` copies `g.sw` (with `top` in bits 11–13) to `ax`.
- Model: `double` (53-bit precision). F2 confirms the precision against the
  oracle; note any difference found in the fuzz.

**Strings:** `rep movsd/movsw/movsb`, `rep stosd/stosw`, standalone `movsb/movsw/stosw`,
`repne scasb` (with the flags of the last `cmp` and the final `ecx` correct).

**Generated code format:**

- Each instruction is preceded by the comment `// 0x00401234: mov eax, dword ptr [ebp + 8]`.
- Deterministic output: two runs produce identical bytes.

**Mnemonic or form without translation:** the translator **fails** with the complete list
(it does not generate partial C).

## Differential test (`tests/fuzz.py` + `tests/fuzz_driver.c`, Linux job)

1. **Candidates:** functions whose transitive call closure **contains no**
   import, indirect call nor `switch` with a trap. List how many there are.
   Select up to 400, prioritizing those that cover forms not yet covered, until
   covering as many of the 154 forms as possible.
2. **Case:** image loaded as above (the same layout in Unicorn: the same
   64 MB memory, the same sections, the same stack).
   - Random registers.
   - Stack with a sentinel return address and 8 random arguments. Half
     of the arguments and registers point inside `.data`/`.bss`
     (`0x715000–0x76C000`) and half are a small integer.
   - Fixed seed per case, to be reproducible.
3. **Execution:**
   - Unicorn (`pip install unicorn==2.1.1` on the runner) up to the sentinel, with
     a limit of 5 million instructions.
   - C driver (compiled with `-DGUEST_CHECK`) up to the return, with a 2 s timeout
     per case.
   - A case in which one of the sides fails (invalid access, limit, timeout) is
     **discarded** and counted; a discard does not count as a divergence. But if one
     side fails and the other does not, that **is** a divergence.
4. **Comparison:**
   - `eax, ecx, edx, ebx, esi, edi, ebp, esp`;
   - SHA-256 of the range `0x00100000–0x0076C000` (stack + image);
   - on divergence, the list of the first 32 differing bytes.
   - Flags and x87 are not compared, except through their effect on memory and
     registers.
5. **Report** `out/recomp-f1/fuzz.md`: candidates, cases run,
   discards by reason, divergences (function VA, seed, first
   difference) and operand forms exercised (with the unexercised ones
   listed).

The emulator `oracle/emu.py` can serve as a base to set up Unicorn
(section mapping); reuse what makes sense.

## Workflow

- **Job `f0`** (the current one): stays the same.
- **Job `f1-build-macos`** (`macos-latest`):
  - `pip install capstone==5.0.6`;
  - `python -m recomp f1 bin/Gpm2.exe gen/`;
  - `make -C runtime lib -j`;
  - publishes the artifact `recomp-f1-gen` (`gen/` + `manifest.json` + build log with
    the number of warnings).
- **Job `f1-fuzz-linux`** (`ubuntu-latest`): generates, compiles the driver with clang,
  runs `tests/fuzz.py` and publishes the artifact `recomp-f1-fuzz`.
- `$GITHUB_STEP_SUMMARY` receives each job's summary.

## Execution

1. Implement in small steps and commit at each green step. Suggested order:
   1. machine.h and the translator of the most common forms;
   2. emission and build;
   3. the rest of the forms;
   4. fuzz.
2. Follow with `gh run watch -R <build repository>`.
3. Download the artifacts to `GPM2-Mac/extracted/dumps/recomp-f1/` (without the
   whole `gen/` if it exceeds 50 MB; in that case, only `manifest.json`, the logs and
   the fuzz report).
4. Write the result in the Log below.

## F1 acceptance criterion

- 2,613 functions translated, 0 forms without translation.
- Green build on macOS (arm64) and Linux. Warnings counted and listed by type.
- Fuzz: **0 divergences** in at least 300 functions executed. At least 130
  of the 154 forms exercised; the unexercised ones listed with the reason
  (for example, `fs:`, which only exists in the CRT).
- Every divergence found along the way and fixed is noted in the Log
  (instruction, cause, fix).

## Log

- 2026-11-02 — specification created.
- 2026-11-02 (end of session 4, pause at the user's request) — **F1 in progress, not accepted.**
  - Done and green (run 41380166384): 2,613 functions translated, 0 forms without translation, 58 files / ~698 thousand lines of C, macOS arm64 build with 0 warnings; tests `test_flags.c`, `recomp.test_translate`, `recomp.test_emit`.
  - Commits `ea98b0c`, `c3e014e`, `63c459f`, `2e4620b` (all on `origin/main`).
  - **Fuzz (run 41380707323) FAILED after 28 min, with no report:** `UcError UC_ERR_READ_UNMAPPED` escaped at `tests/fuzz.py:137` (`uni.run` → `emu_start`). It should have been a discard ("one side failed"), not an exception. It also logged `fs setup FS_BASE failed: UC_ERR_ARG` and fell back to "fs via MSR". It ran 17 min before breaking, so it is necessary to measure the time per case and reduce/parallelize.
  - Lead decisions already accepted (see message to the implementer): extended set with imports simulated identically on both sides (count strict and extended separately; acceptance = 0 divergences, >=300 functions, >=130 forms); `jmp eax` at `0x539C98` = `_chkstk` (switch over the block starts + `dispatch_jump` consuming the top of the shadow stack); shadow stack always on; pointer range 0x53E000–0x720000; soft discard at Unicorn's limit; FPCW 0x027F; do not change the x87 model without recording the case.
  - **Resume:** (1) wrap `uni.run` in try/except → discard with reason; fix the `fs` setup in Unicorn; (2) measure the time per case and limit the job to ~20 min (fewer cases per function or parallel shards); (3) run until green, fix divergences in the translator and record here; (4) download the artifacts to `extracted/dumps/recomp-f1/` and delete `gen1/` (27 MB) from there.
- 2026-11-02 (session 5) — **Per-function fuzz green: 0 divergences, 348 functions, 4,851 cases compared** (run 41449754441, 174 s). User priority: run on the Mac. The fuzz moved from Linux to `macos-latest` (arm64), and the `f1-fuzz-linux` job was removed.
  - All fixes this session were in the **harness**; the translator needed none:
    1. `UC_ERR_READ_UNMAPPED`:
       - Cause: the `fninit/fldcw` stub at `0xF10000` is above `RESTORE_END` and was zeroed by the dirty-page restore.
       - Fix: the stub is rewritten on each case; `fs` is now configured via MSR.
    2. 28 divergences at `0x457ED0`/`0x457B20`:
       - Cause: the mocked imports `sin`/`cos` left no result in ST0, and the following `fst` read an empty register (underflow), handled differently by C and Unicorn. The C mock also compared `"cos"` with `"MSVCRT20.dll!cos"`.
       - Fix: opaque x87 model identical on both sides (`sqrt`/`asin`/`acos`/`atan2`/`cos`/`sin` push 1.0; `_ftol` pops).
    3. Intermittent CRASH/OK (rc -6/-10/-11) right after a TIMEOUT:
       - Cause: the `setitimer` SIGALRM belongs to the process, and on macOS the 2nd alarm reached the main thread; the `siglongjmp` to the worker's `jb` corrupted the stack.
       - Fix: a watchdog in the main thread sends a `pthread_kill(SIGUSR1)` directed at the cases' thread, and the handler only jumps if `in_guest`. Reproduced and confirmed locally with Xcode's clang.
  - Performance and diagnostics:
    - Unicorn in parallel (fork), with 16 cases per function and a limit of 2 million instructions; the whole job takes about 3 min.
    - Driver in a thread with a 1 GB stack.
    - `gen/` included in the fuzz artifact.
  - Pending: **forms exercised 117/154** (target 130). The missing ones only occur in functions with an indirect call or on paths not walked. Solution: per-instruction differential test, in `docs/plans/04-recompiler-instruction-diff-test.md`.
- 2026-11-02 (session 6) — **Per-instruction differential test (04-recompiler-instruction-diff-test) implemented and green: 0 divergences** (run 41453079111, job `f1-fuzz-macos`; old development repo commits: `c916a9f`, `c596a9c`, `0749449`; all on `origin/main`).
  - **Forms:** 154 in total; **134 tested** (all non-control-flow), 20 excluded (`call` x3, `jmp` x3, `ret` x2, `jcc` x12; the `jcc` conditions are covered by the 12 `c_*()` compared). 652 instances (up to 8 per form), 64 cases each = 41,728 cases; 41,458 compared, 270 discarded (both sides fail: `div`/`idiv` with `#DE` in Unicorn and `TRAP divide` in C). The 134 forms had at least one compared case.
  - **Union with the per-function fuzz:** the fuzz exercises 117/154; the per-instruction test covers the 35 that were missing (those that only exist in functions with an indirect call or paths not walked). Forms outside the union: none. The `Fuzz` step remains red in CI **only** because of `fuzz.py`'s old exit rule (>=130 forms alone); it was not changed by order of the lead, and the job summary carries `insn.md` with the union.
  - **Divergences found: none on the first run**, so no translator or runtime defect appeared (the translator had already been validated by the fuzz and by `test_flags.c`). To avoid accepting an empty zero:
    - non-vacuity check: in 40,170 of 41,458 cases Unicorn altered a register, flag, memory or x87; the only form with no effect is `nop`;
    - **negative control** (`tests/insn_mutants.py`, runs in CI): 14 mutants of `machine.c`/`machine.h` (CF of `sub`, OF of `inc`, CF of `shl`/`sar`, overflow of `imul`, remainder of `idiv`, `repne scasb`, `rep movs`, `fcom`, `fdiv`, `fchs`, `fmul`, `fl_set_eflags`, input CF of `adc`) and **all caught** (0 survivors).
  - **Changes:** runtime: `fl_set_eflags(uint32_t)` and `CC_EFL` (explicit cf/zf/sf/of state; `fl_cf/of/zf/sf` gained the `CC_EFL` branch; the other operations did not change). Harness: `recomp/insn_gen.py`, `python -m recomp insn`, `tests/insn_driver.c`, `tests/insn_test.py`, `tests/insn_mutants.py`, target `make insn`.
  - **Harness decisions:**
    - memory by windows (±64 bytes around the EA, `esp`, `ebp`, `esi`/`edi`) initialized equally on both sides and compared byte by byte, instead of a SHA of the whole scratch area;
    - flags compared per instruction: `div`/`idiv` none; `mul`/`imul` only CF/OF; shifts: ZF/SF, CF if count <= size, OF only with count 1, count 0 compares everything; the rest, all four;
    - `fnstsw`: `ax` masked with `0x7D00` (C0/C2/C3 and `top`); the x87 exception flags are not modeled; NaN against NaN counts as equal;
    - `#DE` in Unicorn shows up as a stop without error with `eip` at the instruction, treated as a failure;
    - each instance's code has its own address (avoids rewriting code at the same address in Unicorn);
    - Unicorn's `FP0..FP7` are the physical registers (probed at the start); random initial C0/C2/C3 in the x87 (`FPSW` writable).
  - Time of the whole test: about 3 s (41 thousand cases); the job remains dominated by the per-function fuzz (~3 min). Final artifacts in `extracted/dumps/recomp-f1/insn-run/` (`insn.md`, `insn.json`, `insn-mutants.md`).
- 2026-11-02 (session 5, lead's final review) — **F1 ACCEPTED.** Run 41453881319, all jobs green (`f0`, `f1-build-macos`, `f1-fuzz-macos`).
  - Per-function fuzz: 0 divergences in 348 functions.
  - Per-instruction test: 134 forms and 41,458 cases, with 0 divergences.
  - Mutants: 14 of 14 caught.
  - Union: no form outside, except the excluded control-flow ones.
  - The forms target now applies to the union and is checked in `tests/insn_test.py --fuzz-json`. `fuzz.py` requires only 0 divergences and >= 300 functions.
  - Review of the runtime diff: `CC_EFL`/`fl_set_eflags` (C99) do not alter the other operations.
  - The translator needed no fix in the whole F1 testing.
