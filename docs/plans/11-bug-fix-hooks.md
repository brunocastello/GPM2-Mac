# F8: fixing the known bugs with hooks

**Parent:** 01-overview-static-recompilation §7. **Date:** 2026-11-02. Catalog: `docs/bugs/known-bugs.md`.

## Rules

- **Original behavior** is the reference default. Each fix is an
  option that can be turned off (`settings`, default: **on**). With the option
  off, the behavior goes back to being identical to the original.
- **Fidelity:** behavior of the original that is not a bug stays the same. Example:
  Link Play shows "Unable to complete connection" on cancel.
- **Out-of-scope bugs:** those that only exist on Windows or in emulation
  (BUG-011, PCem) and the network ones marked as resolved by the port itself.
  Note the reason.

## Hook mechanism

- The translator gains a table `recomp/hooks.json`, with an entry
  `{va, sha256_of_the_function_bytes, name, bug}` for each function to fix.
  - If the hash does not match, **the build fails**.
  - The generated function `f_<va>` now calls `hook_<name>()`, handwritten in
    `runtime/hooks/` (portable C99), which decides at run time between the original
    version (`f_<va>_orig`, also generated) and the fixed one.
- A small fix inside a large function can use a point hook:
  - the translator emits `if (hookpt_<name>()) goto L_x;` before a specific
    instruction;
  - an entry of its own in `hooks.json` records the instruction's VA and the expected bytes.
- **Options:** `Settings` gains a "Fixes" tab with one checkbox per bug (short
  text). They are stored in `NSUserDefaults` and in the headless through
  `--fix BUG-xxx=on|off`.
- This phase **changes the translator**. The generated C must be regenerated in CI
  (`f1-build-macos`) and downloaded. The fuzz and the per-instruction test continue
  to apply to the functions without a hook.

## Work

1. **Triage of the 24 bugs:** for each one, read `docs/bugs/known-bugs.md` and
   locate the cause in the decompilation (`extracted/dumps/ghidra/decompiled.c`).
   Classify as:
   - **confirmed in the code**, with function, VA and mechanism;
   - **not reproducible or without evidence**;
   - **out of scope**.

   Record the result in `known-bugs.md` itself, in a column or section
   "Port: state/hook".
2. **Implement the confirmed ones,** in order of impact:
   - BUG-001/008 (electronic failure);
   - BUG-023 (fastest lap not recorded);
   - BUG-015 (repeated TOP CARS);
   - BUG-017 (wet setup requested in dry weather);
   - BUG-020 (occupied car number);
   - BUG-022 (engines 13/14);
   - BUG-024 (`.GCF` reader);
   - and the others the triage confirms.
3. **Tests:**
   - for each hook, a headless or unit test showing the original behavior
     (option off) and the fixed one (option on), over a save or
     a prepared state;
   - use the saves in `original/savegames/` (read-only) and
     `~/Downloads/Test.gps` (read-only) when they serve.

## Constraints

- Nothing installed.
- Never write to `original/` nor to `~/Downloads`.
- Do not commit game files, `gen/` or outputs.
- Runtime in portable C99.
- `make lib fuzz` compiles; CI green.
- Commits in Portuguese, with a blank line before
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Push.

## Acceptance

- Complete triage in `known-bugs.md`.
- Hooks for the confirmed bugs with an original x fixed test.
- "Fixes" tab in Settings.
- App rebuilt.

## Log

- 2026-11-02 — specification created.
- 2026-11-02 — implementation (old development repo `94c0801`): hook mechanism in the translator (`recomp/hooks.py`,
  `hooks.json` with the EXE's sha256; wrong hash/bytes fail the build; `func` and `point` supported, only
  `point` used), `runtime/hooks/` (options, `--fix`, `fixes_set_all(0)` in the fuzz/insn drivers),
  Fixes tab in Settings (NSUserDefaults `Fix.BUG-xxx`), `tests/hooks_test.c` (`make hookstest`).
  Hooks: BUG-022 (`FUN_00431410`, VA 0x431597) and BUG-024 (`FUN_004adb60`, VA 0x4ae353). Triage of the 24
  in `known-bugs.md` §2.1. BUG-023: candidate in `FUN_004972e0`, no hook. The fuzz keeps comparing
  against x86 with all fixes turned off. Local test with a manual gen (capstone absent); the real gen
  comes from CI.
