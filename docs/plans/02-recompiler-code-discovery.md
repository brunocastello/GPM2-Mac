# F0: recompiler base (code discovery and inventory)

**Parent:** 01-overview-static-recompilation §5 (F0). **Date:** 2026-11-02.

## Goal

Build the first part of the recompiler: read the PE, disassemble `.text`
with `capstone`, discover all functions, blocks, `switch` tables and
indirect calls, and publish a report on GitHub Actions. **It does not generate C yet**
(that is F1). F0's output is the exact map that F1 will translate.

## Constraints

- Nothing is installed on the Mac. `capstone` (pip) only exists on the CI runner.
- Python **3.12 in CI**, but the code must **also run on Python 3.9**
  (the Mac's): use `from __future__ import annotations`, no `match`, no `X | Y`
  outside annotations.
- `pe.py` uses only the stdlib and is testable locally. Anything that depends on `capstone`
  imports the module only inside the functions that use it, or in a separate module.
- Do not copy code from the CM 01/02 project (GPLv3). `tools/pe_map.py` from the
  local project (ours) can be reused freely.
- The exe lives in `bin/Gpm2.exe` (SHA-256
  `266f214714b6a2d3396f969f0e142dc267b63d80beb255b9998f66ffcd253546`).
  The output goes to `out/recomp-f0/` (already ignored by `.gitignore`).

## Files to create in ``

```
recomp/__init__.py
recomp/pe.py          PE: sections, imports (IAT entry VA -> "DLL!name"), relocs, entry, va<->offset, reading
recomp/disasm.py      capstone wrapper (CS_MODE_32, detail=True), cache by VA
recomp/discover.py    recursive discovery of functions/blocks/switch/indirect
recomp/report.py      writes CSV/JSON/MD to out/recomp-f0/
recomp/__main__.py    `python -m recomp f0 bin/Gpm2.exe out/recomp-f0 [--ghidra data/ghidra-functions.csv]`
recomp/test_pe.py     unittest with stdlib only (runs locally and in CI)
recomp/test_discover.py  unittest that requires capstone (CI)
data/ghidra-functions.csv   copy of GPM2-Mac/extracted/dumps/ghidra/functions.csv
.github/workflows/recomp.yml
```

Update `README.md` with a short section about `recomp/` and the artifact.

## Discovery algorithm (`discover.py`)

1. **Function seeds:**
   - the entry point;
   - the targets of `call rel32`;
   - the **relocation values that point into `.text`** and are not inside a
     `switch` table nor a case target (these are functions with a taken address:
     WndProc, DlgProc, timer and `qsort` callbacks).

   Iterate until a fixed point: new functions reveal new `call`s.
2. **Function = blocks reachable** from the entry by intraprocedural
   edges: fall-through, `jcc`, `jmp rel` and `switch` cases. A block
   ends at `ret`/`ret N`, `jmp`, `jcc`, a call that does not return (see below) or
   at the start of another known block (split blocks when a jump lands in the middle).
   - **`jmp rel` to the entry of another known function** = tail call
     (outward edge; record in `tailcalls.csv`).
   - **Import thunk:** function whose only content is `jmp dword ptr [IAT]` →
     `import_thunk = "DLL!name"`. The old map found **114**.
   - **Calls that do not return:** `ExitProcess`/`exit`/`_exit` (imports) have
     no fall-through.
3. **`switch` (`jmp dword ptr [4*reg + T]`):**
   - `T` is the table; each dword at `T + 4k` that has a relocation and points into
     `.text` is a target.
   - The bound comes from the preceding `cmp reg, N` + `ja` in the same block or in the
     predecessor (N+1 entries); without `cmp`, stop at the first dword without relocation.
     Record the origin of the bound (`cmp` or `reloc`).
   - If before it there is `mov reg8, byte ptr [reg + T2]` (or `movzx`), `T2` is a
     **byte index table**: record it with size = N+1 of the `cmp` of that
     index and the number of entries of `T` = max(byte)+1.
   - The table bytes are **data**: they are never disassembled and enter the
     coverage as `jumptable`/`indextable`.
4. **Indirect calls and jumps:** `call reg`, `call [mem]` (non-IAT),
   `jmp reg` and `jmp [mem]` (non-table, non-IAT) go to `indirect.csv`, with the
   operand text. The old map found **143** indirect calls.
5. **`.text` coverage:** each byte classified as `code`, `jumptable`,
   `indextable` or `gap`. In `gaps.csv`, list each `gap` interval with its
   size and whether it is only `0xCC`/`0x90`/`0x00` (padding) or something else (possibly
   undiscovered code). **Intervals that are not padding are the main
   review result of F0.**
6. **Errors:** invalid instruction inside reached code → record in
   `errors.csv` (do not abort in F0).

## Outputs (`out/recomp-f0/`)

| File | Columns / content |
|---|---|
| `functions.csv` | `va,end,size_bytes,n_blocks,n_insns,origin(entry/call/reloc, joined by |),import_thunk,n_switch,n_indirect,n_tailcalls,returns(true/false)` |
| `blocks.csv` | `func,start,end,n_insns,terminator` |
| `switches.csv` | `func,site,reg,table,n_entries,bound_source,index_table,index_size,targets(space-separated)` |
| `indirect.csv` | `func,site,kind(call/jmp),operand` |
| `tailcalls.csv` | `func,site,target` |
| `mnemonics.csv` | `mnemonic,count,n_functions,first_va` |
| `operand_forms.csv` | `form,count,first_va` with form = mnemonic + operand types, e.g. `mov r32,m32`, `add m8,imm8`, `fld m64`, `rep movsd` (`rep`/`repne`/`lock`/segment prefixes included). It is the list of cases that F1 needs to translate |
| `gaps.csv` | `start,end,size,kind(padding/unknown),first_bytes_hex` |
| `errors.csv` | `va,bytes_hex,context` |
| `ghidra_diff.csv` | `va,status(only_ours/only_ghidra/size_diff),ours_size,ghidra_size,ghidra_name` (Ghidra: `entry` without `0x`, hex, `size` decimal; ignore `thunk=true` entries that are outside `.text`) |
| `summary.json` | counts: functions, thunks, blocks, instructions, switches (and how many with a `cmp` bound), indirect, tailcalls, bytes per class, unknown gaps, errors, distinct mnemonics, distinct forms, differences with Ghidra by status |
| `report.md` | readable summary: tables from `summary.json`, the 30 rarest operand forms, the 20 largest unknown gaps, the differences with Ghidra |

## Tests

`test_pe.py` (stdlib; must pass locally with `python3 -I -m unittest recomp.test_pe` from ``):
- 8 sections, image base `0x400000`, section names and VAs equal to those in 01-overview-static-recompilation §2;
- 260 imports in total; `MSVCRT20.dll!rand` present with the VA of its IAT entry;
- number of relocations = **52,954**;
- `va_to_off`/`read` consistent (e.g.: the string `"MS Sans Serif"` at `0x71BA38`).

`test_discover.py` (capstone; runs in CI):
- functions `0x0042BF70`, `0x0043C3A0`, `0x004F1B80`, `0x005053E0` (WndProc with a taken address, origin `reloc`) and `0x00401000` present;
- 114 import thunks;
- at least 320 switches and all targets inside the function itself;
- the table that starts right after `0x00401CB8` (`jmp [4*eax + 0x401cbf]`) is classified as `jumptable` and **not** as code;
- `errors.csv` empty.

## Workflow `.github/workflows/recomp.yml`

- `on`: `workflow_dispatch` and `push` on `recomp/**`, `data/ghidra-functions.csv`, `bin/**` and the workflow itself.
- `ubuntu-latest`, `timeout-minutes: 30`, `permissions: contents: read`.
- steps:
  1. checkout;
  2. check the exe's SHA-256 (same as `oracle.yml`);
  3. `setup-python` 3.12;
  4. `pip install capstone==5.0.6` (if that version does not exist on PyPI, pin the newest 5.0.x and note it in the README);
  5. `python -m unittest recomp.test_pe recomp.test_discover -v`;
  6. `python -m recomp f0 bin/Gpm2.exe out/recomp-f0 --ghidra data/ghidra-functions.csv`;
  7. `cat out/recomp-f0/report.md >> $GITHUB_STEP_SUMMARY`.
- `upload-artifact@v4`, name `recomp-f0`, path `out/recomp-f0/`, `if: always()`, 90-day retention.

## Execution and delivery

1. Implement; run `test_pe` locally.
2. Commit in the old development repository (`main`) with a message in Portuguese (like the history) and
   push. Follow with `gh run watch -R <build repository>`; fix until the
   workflow is green. Red tests must not be hidden nor loosened
   without noting the reason.
3. Download the artifact to `GPM2-Mac/extracted/dumps/recomp-f0/`
   (`gh run download -R <build repository> -n recomp-f0 -D ...`).
4. Report: `summary.json`, the differences with Ghidra (counts), the unknown
   gaps, the switches without a `cmp` bound, the number of operand forms and
   anything unexpected.

## F0 acceptance criterion

- `recomp` workflow green, artifact published.
- All `unknown` gaps and all differences with Ghidra **explained** in the
  log below (dead code, data in `.text`, Ghidra false positive...).

## Log

- 2026-11-02 — specification created.
- 2026-11-02 — F0 implemented in the old development repository (commits `ee805a4`..`b9e9fe2`); `recomp` workflow
  green (run 41378508965); artifact in `extracted/dumps/recomp-f0/`. Deviations and findings:
  - **Local command**: `python3 -I -m unittest recomp.test_pe` fails (`-I` removes the current
    directory from `sys.path`); use `python3 -m unittest recomp.test_pe -v` from ``.
  - **Fourth origin `scan`** (not in the spec): after the `entry/call/reloc` fixed point, it looks for
    prologues `55 8B EC` / `55 89 E5` and thunks `FF 25 <IAT>` in bytes not yet covered, preceded
    by padding or by covered code, and treats them as functions (dead code without references).
    359 `scan` functions + 44 `call|scan`. Without this, 247 Ghidra functions and 16 thunks were left out.
  - **Thunk test**: the spec asked for 114; `.text` has **259** `jmp [IAT]` thunks (one per import,
    259 of 260). The 114 of the old map are all contained in the 259; the others were just those
    that the old scanner did not count. The test now requires exactly 259.
  - **`switch` bound**: the `cmp` can be against memory (`cmp dword ptr [ebp-0x168], 0xa`), not just
    a register; the dword table has one extra entry (the `default`) right before the byte
    index table, so with an index table the number of entries is
    `max(max(byte)+1, (T2-T)/4)`. Result: 326 switches, all with a `cmp` bound, 97 with
    an index table.
  - `jmp rel` to an import thunk is treated as a tail call (case `0x0053A690` ->
    `0x0053DAF0`, GetTickCount).
  - **Differences with Ghidra** (final): `only_ghidra` 0, `size_diff` 0, `only_ours` 153:
    149 six-byte import thunks that Ghidra did not create as functions, plus 4 real functions that
    Ghidra does not see for lack of a code reference: `0x00539E30` and `0x00539E47` (exception
    filters/handlers pointed to by `.rdata` `0x714104/8`) and `0x0053B0D0` / `0x0053B0F0` (method
    pointers written by `mov [ebx], imm`; the second has 1215 bytes and a `switch`).
  - **`unknown` gaps** (985, 16,423 bytes of 1,297,226): `dead_jmp` 763 (3,812 B; 5-byte `jmp rel32`
    after another `jmp`, emitted by MSVC and unreachable), `dead_code` 69 (8,110 B; blocks that
    decode cleanly and end in `ret`/`jmp`, with no reference from reached code; the
    largest: `0x00487969` 1437 B, `0x0047D4B6` 1402 B, `0x0053100F` 1281 B) and `decodes_no_clear_end`
    153 (4,501 B; sequences that decode cleanly but without a clear terminator, 92 of them starting
    with `lea`, i.e. alignment padding `lea esp,[esp]`/`lea ecx,[ecx]`). List in
    `gaps_unknown_analysis.csv`. No reference comes from reached code (by construction);
    43 gaps have a reference only from inside other dead code. Final review is left for F1.

### Final review (main session, 2026-11-02) — **F0 ACCEPTED**

- Green run 41378508965; artifact in `extracted/dumps/recomp-f0/`; `test_pe` passes locally (`python3 -m unittest recomp.test_pe`).
- **Unknown gaps provably dead:** no relocation points inside a gap (in a PE with `.reloc` every absolute code address is relocated,
  so there is no table, callback or function pointer that reaches them). The
  only 2 relative jumps that land in gaps (`0x41E27D`, `0x5075D7`) come from
  `switch` table bytes that the old linear disassembler read as code
  (false positives). The largest gap, `0x487969`, is an entire `for` loop
  (`jmp cond; inc i; …`) with no entry: dead code that MSVC 2.0 did not
  eliminate.
- Against Ghidra: **0 Ghidra-only functions, 0 different sizes**; the 153 ours-only
  = 149 thunks + 4 explained.
- **Input for F1:** only **60 mnemonics** and **154 operand forms**. There are
  126 indirect: 67 `call [abs]` (global function pointers), 57 `call [reg+k]`,
  `call eax` ×3 and 1 `jmp eax` (`0x539C98`, CRT). 326 switches, all bounded by
  `cmp`, 97 with an index table.
- Minor pending item: the 326th switch, compared with the 325 old sites, was not
  traced. Check in F1, when each switch becomes a C `switch`.
