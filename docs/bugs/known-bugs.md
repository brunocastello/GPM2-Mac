# Bug Registry — GPM2

**Status:** initial registry. **Six bugs are confirmed by official primary sources** (1.02 and 1.02B notes): BUG-001, BUG-015, BUG-016, BUG-017, BUG-018 and BUG-019. **BUG-022 is confirmed by direct observation of the executable's bytes.** The rest remain reports or hypotheses until reproduced in the original game or confirmed by a binary diff.

**Goal:** track each bug with its original behavior and expected corrected behavior, plus the verification method and the regression test that validates the fix in the port.

---

## 1. Registry rules

1. **Original ≠ corrected.** Every item records both behaviors separately. The fix must not change the UI, the texts or the screen flow beyond what is necessary.
2. **Evidence required.** `[REPORT]` is a starting point, not a conclusion. To promote to `CONFIRMED`, one of the following is needed:
   - controlled reproduction in the original (version and emulator recorded), or
   - a diff between versions that shows the official fix, or
   - a primary source (official patch notes, code, MicroProse documentation).
3. **Environment is not a game bug.** Failures caused by the emulator, a read-only folder, the operating system or the resolution must be classified as `Environment` and do not enter the port's logic.
4. **One bug, one ID.** Duplicate reports point to the same ID; do not create new IDs for the same defect.
5. **Every fix has a test.** Each fixed `BUG-###` must have a regression test that fails on the original behavior and passes on the corrected one.

### Classes
| Class | Meaning |
|---|---|
| `Logic` | Simulation/game rule (race, contracts, finances, research). |
| `Data` | Incorrect or misinterpreted data table/file. |
| `UI` | Rendering, layout, input, text. |
| `Save` | Persistence, loading, integrity. |
| `Environment` | Emulator, operating system, permissions, hardware. |
| `Undetermined` | Not yet classified. |

### Status
`HYPOTHESIS` → `REPORT` → `TO VERIFY` → `CONFIRMED` → `FIXED` / `DISCARDED` / `OUT OF SCOPE`

### Severity
`Critical` (loss of save/progress or crash), `High` (incorrect race/career result), `Medium` (visibly wrong behavior, can be worked around), `Low` (cosmetic/rare).

### 1.1 Confirmed primary source — patch 1.02B notes

`original/GPM2 Updates v1.02b/V102b.txt` (253 bytes, SHA-256 `06be2c20ab8bbdf12c037896ceb8caf255d4a3dc7dc1078796f063e354b06646`) contains:

> Grand Prix Manager 2
> v1.02b
>
> Requirements
> Same as v1.02 as per other documentation (Win95 or above plus DirectX etc.).
>
> Change
> **Removed rule from the race that takes cars out due to 'Electronic Problems'.**
>
> Grand Prix Manager Development Team

This confirms that the earlier version had a **race rule** that took cars out for "Electronic Problems" and that 1.02B removed it. The behavior is recorded in BUG-001.

### 1.2 Confirmed primary source — official 1.02 instructions

`original/GPM2 Updates v1.02b/GPM2-V1.02 Instructions.doc` (23,040 bytes, SHA-256 `c83b4c7cf23f576ee8dc1cc247ca75e687b57277890496f6afcb0a33e726e592`) contains the **BUGS FIXED** section, extracted with `textutil -convert txt -stdout`:

**Network Play**
- Many crashes fixed; area rewritten with Direct X.
- Cars often retired from the race with "Electronic Problems".
- The master machine's background sound sometimes stayed off.
- Speech samples sometimes "crackled" a lot.
- At the end of the race, the game sometimes froze while displaying the results.
- Occasionally, in the first race of the season, drivers asked for a wet setup even in dry conditions.

**Single Player**
- "Electronic Problems" retirements fixed.
- During the race, the "TOP CARS" report sometimes listed the same car six times instead of the top six.

> Important: the document is from patch **1.02**. `V102b.txt` is a later update that **removes the rule** for electronic failure. The history is therefore: 1.02 tried to fix the behavior; 1.02B eliminated the rule.

---

## 2. Overview of bugs and candidates

| ID | Title | Class | Evidence | Severity | Status |
|---|---|---|---|---|---|
| BUG-001 | Electronic failure rule removes cars from the race | Logic | `[CONF]` | High | Confirmed (official) |
| BUG-002 | Save/Save As fails or crashes the game | Save / Environment | `[REPORT]` | Critical | To verify |
| BUG-003 | Excess of spin-offs regardless of setup/driver | Logic | `[REPORT]` | High | To verify |
| BUG-004 | Only one car in the race despite two qualified | Logic / Save | `[REPORT]` | High | To verify |
| BUG-005 | Cutting the Suzuka bridge yields ~1/4 of a lap | Data | `[REPORT]` | Medium | To verify |
| BUG-006 | Abnormally long pit stops | Logic | `[REPORT]` | Medium | To verify |
| BUG-007 | Traction control bug fixed in 1.02B | Logic | `[REPORT]` | Medium | To verify |
| BUG-008 | "Electronic problem" only partially fixed | Logic | `[CONF]` | High | Consolidated into BUG-001 |
| BUG-009 | Strong car with Renault engine 1-1.5 s/lap off the pace in season 5 | Logic / Data | `[REPORT]` | Medium | To verify |
| BUG-010 | Qualifying problems | Undetermined | `[REPORT]` | Medium | To verify |
| BUG-011 | Crash at race start with 1.02B on PCem | Environment | `[REPORT]` | Low (outside the logic) | Out of scope |
| BUG-012 | Performance/research calibration | Logic | `[HYP]` | High | To verify |
| BUG-013 | Contract invariants (duplicates, renewal) | Logic | `[HYP]` | High | To verify |
| BUG-014 | Save limits/structure at the season rollover | Save | `[HYP]` | Critical | To verify |
| BUG-015 | "TOP CARS" report repeats the same car six times | UI | `[CONF]` | Medium | Confirmed (official) |
| BUG-016 | Freeze when displaying results at the end of the race (network) | Logic / UI | `[CONF]` | High | Confirmed (official; network) |
| BUG-017 | Wet setup request in the first race with dry weather | Logic | `[CONF]` | Medium | Confirmed (official) |
| BUG-018 | Master's background sound stays off (network) | Audio | `[CONF]` | Low | Confirmed (official; network) |
| BUG-019 | Speech samples crackle (network) | Audio | `[CONF]` | Low | Confirmed (official; network) |
| BUG-020 | Assigning an occupied car number erases the 2nd driver of the 4th team | Logic / Save | `[REPORT]` | High | To verify |
| BUG-021 | Next button misplaced on the Option 13 debug screen | UI | `[REPORT]` | Low | To verify |
| BUG-022 | Engines 13/14 reuse the manufacturer of engine 3 | Data | `[OBS]/[REPORT]` | Medium | Confirmed (data) |
| BUG-023 | Fastest lap is not recorded after the player's race | Data / Save | `[CONF]` (data) | Medium | Confirmed (data) |
| BUG-024 | `.GCF` reader ignores the `DEFAULTS.GCF` format (version 0): misaligns by 14 bytes and fails the footer check | Data / Logic | `[CONF]` (static + oracle) | Medium | Confirmed |
| BUG-025 | Chat messages received while the chat is closed are lost | Network / UI | `[CONF]` (static; no two-instance test) | Low | Confirmed in code |
| BUG-026 | The chat's "Send Window To Back" reloads an old copy of the current view and the game stops responding to clicks | Logic / UI | `[CONF]` (static) | Medium | Confirmed in code |


### 2.1 Port triage (11-bug-fix-hooks, 2026-11-02): state and hook

Each fix is an option (`Settings > Fixes`, `--fix BUG-xxx=on|off`; default on). With the option off the
behavior is the original's. Hooks: `recomp/hooks.json` (bytes verified at build) +
`runtime/hooks/hooks.c`. Test: `tests/hooks_test.c` (original vs corrected).

| ID | Port: state | Hook / reason |
|---|---|---|
| BUG-022 | **Fixed** (point hook) | `FUN_00431410` (new-career init), before `0x431597`: the manufacturer of engines 12/13 (the `2` immediates at `0x43158f/0x431593`) becomes 12/13. The file bytes `0x30992/0x30996` are code immediates, not a data table. |
| BUG-024 | **Fixed** (point hook) | `FUN_004adb60` (`.GCF` reader), before `cmp eax,1` at `0x4ae353`: version 0 is treated as 1 (30 entries). Test: reads `DEFAULTS.GCF` and ends at `size-63` (original: 14 bytes earlier). |
| BUG-025 | **Fixed** (core, `win32/dialog.c`) | `SendMessageA` to the chat list with the window closed: the text is kept (up to 256 lines) and enters the list on reopening. Can be disabled as `BUG-025=off`. Two-instance test missing. |
| BUG-026 | **Fixed** (point hook) | `FUN_004f50e0`, before `mov eax,[ebp+8]` at `0x4f51c6`: if the requested view is already the current one (`DAT_0071b33c`), skips `FUN_004f81f0` (up to `0x4f51d2`). User test missing. |
| BUG-023 | Confirmed in the data; **cause in the code only partial, no hook** | The track record is written in the "fastest lap" branch of `FUN_004972e0` (`decompiled.c` ~54132-54194: writes `0x6ac458`, `0x66d128` and `0x6b6608` with `0x4f4..0x4f8` of the car). The condition of that branch compares `0x638818+car*0xe` (which is the **qualifying** time, used as pole in `decompiled.c` 99779/99795) with `0x66d128`, but writes the best race time (`0x4f4`): suspected copy of the pole logic. Without tracing a real race, it was not possible to prove it or validate the fix; it stays open. |
| BUG-001 / BUG-008 | No hook: already fixed in 1.02B | Official note: 1.02B removes the rule. Not searched for in the code. |
| BUG-015, BUG-017 | No hook: already fixed in 1.02 | Official 1.02 instructions (the port is 1.02B). Not verified in the code. |
| BUG-011 | Out of scope | Only exists under PCem. |
| BUG-016, BUG-018, BUG-019 | Out of scope | Network: the port has its own DirectPlay engine (10-network-link-play). |
| BUG-002, 003, 004, 005, 006, 007, 009, 010, 012, 013, 014, 020, 021 | No evidence | Only reports or hypotheses; no cause located in the decompilation. BUG-020 (editor) and BUG-021 (Option 13 debug screen) were not investigated in the code. |

Count (24): 2 fixed, 1 confirmed without hook, 4 already officially fixed (001, 008, 015, 017), 4 out of scope, 13 without evidence.

---

## 3. Details of the bugs and candidates

### BUG-001 — Electronic failure rule removes cars from the race

| Field | Value |
|---|---|
| Class | Logic |
| Versions | 1.00 through 1.02 (pre-1.02B) affected; 1.02B removes the rule |
| Evidence | `[CONF]` (official note) + `[REPORT]` (same-lap detail) |
| Severity | High |
| Status | Confirmed (official) |
| Sources | `V102b.txt`; `GPM2-V1.02 Instructions.doc`; GPM Planet (thread 21); TCRF; f1-forum.fi; Abandonia |

**Original behavior (rule confirmed by the official notes; mechanism corroborated by the community):** there was a **race rule** that retired cars for "Electronic Problems". The 1.02 document already listed "Cars would often retire from the race with 'Electronic Problems'" among the fixed bugs; `V102b.txt` goes further and says the rule was **removed**. The community describes the specific trigger `[REPORT]`: if the **EMS** (engine management system) level was less than or equal to the level of the best developed **driver aid**, the chance of electrical failure fired, even if the aid was not active. Example: level 5 semi-automatic gears would require EMS >= 50%. The component that failed/was replaced was the electrical system, but the trigger was the EMS. Reports also describe both cars retiring on the same lap after ~10 laps, between mid-season 1 and the end of season 3; this pattern has not yet been reproduced.

**Expected corrected behavior:** the port must not contain the forced electronic failure rule. Any retirement due to electronic failure must come from the normal reliability/wear model, independently per car and reproducible by seed. The reference behavior is that of 1.02B (rule removed), which succeeds the 1.02 fix attempt.

**How to verify:** three-version diff (1.00, 1.02 and 1.02B) to locate the EMS vs. driver aid comparison and its removal; reproduce in 86Box with EMS below a developed aid and compare with 1.02B. In the port, ensure that no electrical failure depends on developed aids and test the distribution of DNFs by component/team.

**Confirmation criterion:** the existence and removal of the rule are confirmed by the official note. Reproduction of the same-lap pattern remains pending.

**Relation:** BUG-008 was discarded; the official note states removal of the rule, not partial correction.

---

### BUG-002 — Save/Save As fails or crashes the game

| Field | Value |
|---|---|
| Class | Save / Environment (triage needed) |
| Versions | 1.00 and 1.02B (to be confirmed) |
| Evidence | `[REPORT]` |
| Severity | Critical |
| Status | To verify |
| Sources | f1-forum.fi; worldracing.info |

**Original behavior (reported):** Save/Save As may fail or return to Windows; the community countermeasure is to remove the read-only attribute from the saves folder.

**Expected corrected behavior:** saving must be atomic (temporary file + validation + swap), with a clear error message in case of permission/space problems, without losing the previous save. In the port, writing in the macOS sandbox eliminates the permission cause.

**How to verify:** test a writable folder, read-only, full disk, long path and invalid name. Distinguish a game bug from a system configuration problem. Check whether a partial/truncated save is accepted on load.

**Confirmation criterion:** failure reproduced with a writable folder and without antivirus/emulator interference.

---

### BUG-003 — Excess of spin-offs regardless of setup/driver

| Field | Value |
|---|---|
| Class | Logic |
| Versions | To be confirmed |
| Evidence | `[REPORT]` |
| Severity | High |
| Status | To verify |
| Sources | Abandonia; f1-forum.fi |

**Original behavior (reported):** drivers such as Damon Hill spin off with excessive frequency, regardless of setup and driver quality; the patch does not fix it.

**Expected corrected behavior:** spin probability derived in a documented way from driver, setup, weather, track and car state; aggregate rate comparable to the F1 of the period and reproducible by seed.

**How to verify:** collect a sample of simulated races in the original (min. 20 GPs), tabulate spins by driver/setup/weather/track. Without this baseline, there is no way to claim "excessive".

---

### BUG-004 — Only one car in the race despite two qualified

| Field | Value |
|---|---|
| Class | Logic / Save |
| Versions | To be confirmed |
| Evidence | `[REPORT]` |
| Severity | High |
| Status | To verify |
| Sources | Abandonia |

**Original behavior (reported):** the team qualifies two cars, but only one appears/lines up in the race.

**Expected corrected behavior:** both of the team's entries must always be present on the grid and in the race, except by explicit rule (withdrawal before the start, nonexistent spare car, etc.), with the reason recorded in the log.

**How to verify:** inspect saves at the moment of the bug; check whether the second entry exists in the file and is discarded by the loader/renderer. Test with teams of 1 and 2 cars.

---

### BUG-005 — Cutting the Suzuka bridge yields ~1/4 of a lap

| Field | Value |
|---|---|
| Class | Data |
| Versions | To be confirmed |
| Evidence | `[REPORT]` |
| Severity | Medium |
| Status | To verify |
| Sources | f1-forum.fi |

**Original behavior (reported):** cars cut the Suzuka bridge and gain approximately a quarter of a lap.

**Expected corrected behavior:** the layout and the lap logic must prevent gaining distance by cutting; position and lap time consistent with the official course. In the port, the race view must use the same geometry as the original, with collision/checkpoint correction.

**How to verify:** map Suzuka's `.RAC`; locate the bridge segment; measure lap distance with and without the cut. Confirm whether it is geometry, checkpoint or AI.

---

### BUG-006 — Abnormally long pit stops

| Field | Value |
|---|---|
| Class | Logic |
| Versions | To be confirmed |
| Evidence | `[REPORT]` |
| Severity | Medium |
| Status | To verify |
| Sources | f1-forum.fi |

**Original behavior (reported):** pit stops take much longer than expected.

**Expected corrected behavior:** duration = travel time in the pit lane (with speed limit) + stationary time (refueling/tyres, depending on team/error), documented and calibrated per track. It must be identical to the original's, unless the original demonstrably violates the rule.

**How to verify:** time pits in the original across several tracks/teams; compare with the pit lane length and with the period. Caution: perceived slowness may be intentional fidelity.

---

### BUG-007 — Traction control bug fixed in 1.02B

| Field | Value |
|---|---|
| Class | Logic |
| Versions | 1.00 affected; fixed in 1.02B (reported) |
| Evidence | `[REPORT]` |
| Severity | Medium |
| Status | To verify |
| Sources | Race Sim Central (article about the launcher), indirect mention |

**Original behavior (reported):** a version prior to 1.02B had a traction control bug, later fixed by the developer.

**Expected corrected behavior:** the traction control adjustment must produce the effect documented in the manual, without undue penalty/benefit. The 1.02B fix must be identified and incorporated.

**How to verify:** the official 1.02B note (`V102b.txt`) **does not mention traction control**; it only mentions the removal of the electronic failure rule. Diff the 1.00 <-> 1.02B executables and look for TC strings/constants; in the absence of evidence, treat it as an unconfirmed report.

> **Note:** the claim that 1.02B fixed traction control comes from a secondary source and is not supported by the official notes. Do not implement a TC fix until there is evidence in the binary or a controlled reproduction.

---

### BUG-008 — "Electronic problem" only partially fixed

| Field | Value |
|---|---|
| Class | Logic |
| Versions | 1.00, 1.02 and 1.02B |
| Evidence | `[CONF]` (official notes) + `[REPORT]` (Abandonia) |
| Severity | High |
| Status | Consolidated into BUG-001 |
| Sources | `GPM2-V1.02 Instructions.doc`; `V102b.txt`; Abandonia |

**Original behavior (confirmed):** version 1.00 retired cars for "Electronic Problems". The 1.02 document lists the problem as fixed; `V102b.txt` says the rule was **removed**. The community report of a "only partial" fix is consistent with the intermediate 1.02 state, which still kept the rule in some form.

**Expected corrected behavior:** adopt the 1.02B behavior: no forced electronic failure rule; failures only from the normal reliability model. The ID does not represent a separate defect.

**How to verify:** three-version diff (1.00, 1.02, 1.02B) to reconstruct the evolution; reproduce DNFs by cause. Reopen only if there is an independent electronic mechanism in 1.02B.

**Relation:** consolidated into BUG-001.

---

### BUG-009 — Renault 1-1.5 s/lap off the pace in season 5

| Field | Value |
|---|---|
| Class | Logic / Data |
| Versions | Reported in a season 5 save |
| Evidence | `[REPORT]` |
| Severity | Medium |
| Status | To verify |
| Sources | worldracing.info |

**Original behavior (reported):** in season 5, a strong car with a Renault engine is 1-1.5 s/lap slower, despite being lighter than its rivals.

**Expected corrected behavior:** performance must be a monotonic, documented function of chassis, engine, tyres, weight, aerodynamics and research; no supplier should suffer a hidden penalty not explained by the data.

**How to verify:** obtain the save and compare the performance table; check for field overflow (byte/short), per-season engine table, or AI bonus. Reproduce on 1.00 and 1.02B.

---

### BUG-010 — Qualifying problems

| Field | Value |
|---|---|
| Class | Undetermined |
| Versions | To be confirmed |
| Evidence | `[REPORT]` |
| Severity | Medium |
| Status | To verify |
| Sources | Abandonia (generic mention) |

**Original behavior (reported):** "qualifying problems" cited without detail: times not computed, incorrect grid or drivers not taking part.

**Expected corrected behavior:** all entrants have a time or a justified absence (rain, mechanical problem, 107%); grid ordered by time with a documented tie-break; substitutions preserve the correct order.

**How to verify:** collect specific reports; test simulated and assisted qualifying; compare the grid in the save with the times shown.

**Note:** until there is a concrete symptom, this ID works as an umbrella and may be split.

---

### BUG-011 — Crash at race start with 1.02B on PCem

| Field | Value |
|---|---|
| Class | Environment |
| Versions | 1.02B under PCem/Windows 98 |
| Evidence | `[REPORT]` |
| Severity | Low (outside the logic) |
| Status | Out of scope |
| Sources | GPM Planet (1.02b thread; administrator attributes it to PCem and recommends 86Box) |

**Original behavior (reported):** the unpatched game works on PCem; after 1.02B, it freezes at the start of the race. The forum administration attributes it to the emulator.

**Expected corrected behavior:** does not apply to the native port. Record it so it is not confused with a logic bug and to ensure that validation uses 86Box, not PCem.

---

### BUG-012 — Performance/research calibration

| Field | Value |
|---|---|
| Class | Logic |
| Versions | All (hypothesis) |
| Evidence | `[HYP]` |
| Severity | High |
| Status | To verify |
| Sources | Hypothesis from the project's initial briefing |

**Original behavior (to be determined):** suspected unbalanced research/performance progression (AI evolves inconsistently, hidden bonuses, non-linear part effect).

**Expected corrected behavior:** documented, stable evolution curve over 10 seasons; no gain unexplained by data; AI and player subject to the same rules.

**How to verify:** extract performance/research tables; simulate 10 seasons with a fixed seed; measure time delta per team; compare with the original. Only classify as a bug after establishing the reference behavior.

---

### BUG-013 — Contract invariants

| Field | Value |
|---|---|
| Class | Logic |
| Versions | All (hypothesis) |
| Evidence | `[HYP]` |
| Severity | High |
| Status | To verify |
| Sources | Hypothesis from the project's initial briefing |

**Original behavior (to be determined):** possible failures in renewal, simultaneous hiring, salaries, duration, penalties and linking of sponsors/suppliers.

**Expected corrected behavior:** guaranteed invariants: a driver cannot race for two teams; an active contract blocks transfer without penalty/termination; renewal does not duplicate records; salaries and budget limits are applied consistently; expiration dates correct at the season rollover.

**How to verify:** property tests on the contract model; explore negotiation in the original; inspect saves after each step.

---

### BUG-014 — Save limits/structure at the season rollover

| Field | Value |
|---|---|
| Class | Save |
| Versions | All (hypothesis) |
| Evidence | `[HYP]` |
| Severity | Critical |
| Status | To verify |
| Sources | Hypothesis from the initial briefing; save reports in BUG-002 |

**Original behavior (to be determined):** suspected corruption/truncation after many seasons, counter overflow, or incompatibility between saves from different versions.

**Expected corrected behavior:** byte-for-byte identical save->load round-trip in the model; explicit version migration; size/counter limits validated; no loss of history when advancing the season.

**How to verify:** create saves at each phase (pre-season, mid, end, rollover) in versions 1.00 and 1.02B; compare structures; test cross-loading; fuzz fields.

---

### BUG-015 — "TOP CARS" report repeats the same car six times

| Field | Value |
|---|---|
| Class | UI |
| Versions | 1.00/1.01; fixed in 1.02 |
| Evidence | `[CONF]` (official 1.02 instructions) |
| Severity | Medium |
| Status | Confirmed (official) |
| Sources | `GPM2-V1.02 Instructions.doc`, **Single Player Bugs** section |

**Original behavior (confirmed):** during the race, the "TOP CARS" report sometimes listed the same car six times instead of showing the top six cars.

**Expected corrected behavior:** the report must list six distinct cars, in position order, with no duplicates; the fix does not change the screen layout.

**How to verify:** reproduce in 1.00/1.01 with the report open during the race; compare with 1.02; ensure that the list is derived from the live standings and not from a reused buffer.

**UI impact:** only the list content; position, font and panel size remain the same.

---

### BUG-016 — Freeze when displaying results at the end of the race (network)

| Field | Value |
|---|---|
| Class | Logic / UI |
| Versions | 1.00/1.01; fixed in 1.02 (network play) |
| Evidence | `[CONF]` (official 1.02 instructions) |
| Severity | High |
| Status | Confirmed (official; network mode) |
| Sources | `GPM2-V1.02 Instructions.doc`, **Network Play** section |

**Original behavior (confirmed):** at the end of a network race, the game sometimes froze while displaying the results.

**Expected corrected behavior:** the results screen must always render and allow advancing; no deadlock between clients. The document does not detail whether it occurred in single-player.

**How to verify:** network race with 2+ players, repeat several times in 1.00 and 1.02; capture synchronization logs. In the port, test the results flow with simulated network state.

---

### BUG-017 — Wet setup request in the first race with dry weather

| Field | Value |
|---|---|
| Class | Logic |
| Versions | 1.00/1.01; fixed in 1.02 |
| Evidence | `[CONF]` (official 1.02 instructions) |
| Severity | Medium |
| Status | Confirmed (official) |
| Sources | `GPM2-V1.02 Instructions.doc`, **Network Play** section |

**Original behavior (confirmed):** occasionally, in the first race of the season, drivers asked for a wet setup even in dry conditions.

**Expected corrected behavior:** the setup recommendation must derive from the forecast/actual track condition; never request a wet setup in dry conditions. Applies to advice to the player and to the AI.

**How to verify:** repeat the first race of the season in 1.00/1.01 and 1.02, record the forecast and the request; unit test with dry/wet weather.

---

### BUG-018 — Master's background sound stays off (network)

| Field | Value |
|---|---|
| Class | Audio |
| Versions | 1.00/1.01; fixed in 1.02 (network play) |
| Evidence | `[CONF]` (official 1.02 instructions) |
| Severity | Low |
| Status | Confirmed (official; network mode) |
| Sources | `GPM2-V1.02 Instructions.doc`, **Network Play** section |

**Original behavior (confirmed):** the master machine's background sound sometimes stayed off.

**Expected corrected behavior:** the audio state must be consistent and restorable on both machines; the master's background sound cannot stay permanently muted without user action.

**How to verify:** network session, toggle sound on master/slave; compare the state after screen transitions.

---

### BUG-019 — Speech samples crackle (network)

| Field | Value |
|---|---|
| Class | Audio |
| Versions | 1.00/1.01; fixed in 1.02 (network play) |
| Evidence | `[CONF]` (official 1.02 instructions) |
| Severity | Low |
| Status | Confirmed (official; network mode) |
| Sources | `GPM2-V1.02 Instructions.doc`, **Network Play** section |

**Original behavior (confirmed):** speech samples sometimes "crackled" a lot.

**Expected corrected behavior:** clean playback, without clicks/pops; correct audio format and buffer. May be a consequence of timing/DirectPlay, not a content defect.

**How to verify:** capture audio in a network session 1.00 vs 1.02; compare waveform/energy at the transitions.

> The document also cites "many crashes fixed under network play" without specific symptoms. Do not create an umbrella ID until a concrete reproduction is obtained.

---

### BUG-020 — Assigning an occupied car number erases the 2nd driver of the 4th team

| Field | Value |
|---|---|
| Class | Logic / Save |
| Versions | To be confirmed (reported in versions with the in-game editor) |
| Evidence | `[REPORT]` (GPM Planet, modding guide) |
| Severity | High |
| Status | To verify |
| Sources | GPM Planet, *GPM2 Ultimate Modding Guide* |

**Original behavior (reported):** in the in-game editor, assigning a driver a car number already taken by another triggers a bug that can **erase the second driver of the fourth team**. The suggested countermeasure is to use a temporary number or accept the warning with OK.

**Expected corrected behavior:** car numbers must be unique and validated; attempting to assign an occupied number must be rejected with a clear message, without removing or corrupting any driver. The operation must be atomic in the save.

**How to verify:** reproduce in 1.00/1.02/1.02B by assigning occupied numbers on all teams; inspect the save before/after; confirm whether the driver disappears or it is just a display effect. Test the order of the teams (the bug mentions the fourth).

**Confirmation criterion:** reproduction with driver loss in the save; or evidence in the code of an out-of-bounds index when writing the number.

---

### BUG-021 — Next button misplaced on the Option 13 debug screen

| Field | Value |
|---|---|
| Class | UI |
| Versions | To be confirmed (1.00/1.02/1.02B) |
| Evidence | `[REPORT]` (TCRF) |
| Severity | Low |
| Status | To verify |
| Sources | TCRF — *Grand Prix Manager 2* |

**Original behavior (reported):** on the screen opened with the `debug` password in Option 13, the **Next** button is misplaced/out of position, hindering navigation.

**Expected corrected behavior:** the Next button must be in the correct position and respond to click/keyboard; the fix must not alter the other hidden screens or the normal flow. Since the menu is hidden, the fix must be made in the same template, preserving the original layout as much as possible.

**How to verify:** enter Option 13 with `debug` in 86Box, capture the screen, compare with the other Option 13 screens and with the Swift implementation. Test navigation by mouse and keyboard.

**Confirmation criterion:** screenshot showing the displacement and a click test outside the expected area.

---

### BUG-022 — Engines 13/14 reuse the manufacturer of engine 3

| Field | Value |
|---|---|
| Class | Data |
| Versions | 1.02B (and probably earlier) |
| Evidence | `[OBS]` (EXE bytes) + `[REPORT]` (community fix) |
| Severity | Medium |
| Status | Confirmed (data) |
| Sources | GPM Planet, *Hex Editing Guide* (thread 36) and thread 319 |

**Original behavior (observed):** in the 1.02B `Gpm2.exe`, the manufacturer ID bytes of engines 13 and 14 are `0x02`, the same ID as engine 3. The community calls this the "engine 13 bug" and recommends changing `0x30992` from `02` to `0C` and `0x30996` from `02` to `0D` to get distinct manufacturers.

**Expected corrected behavior:** engines 13 and 14 must have their own manufacturer IDs (12 and 13, per the guide), without sharing contract/identity with engine 3. Validate that the suppliers UI and negotiations show the correct manufacturers.

**How to verify:** compare the engine tables in the 1.00, 1.02 and 1.02B EXEs; open the suppliers/contracts screen in the game and check the names assigned to engines 13/14; apply the edit to a copy and compare.

**Relation:** may influence `BUG-009` (anomaly with Renault engine), but there is no direct evidence.

---

### BUG-023 — Fastest lap is not recorded after the player's race

| Field | Value |
|---|---|
| Class | Data / Save |
| Versions | To be confirmed (real saves analyzed; executable version not identified) |
| Evidence | `[CONF]` (save before/after Melbourne + driver's career counter) |
| Severity | Medium (incorrect fastest-lap history; does not affect the 10-6-4-3-2-1 scoring) |
| Status | Confirmed (data) |
| Sources | `A Season With McLaren.gps` / `A Season With McLaren 2.gps`; FMT-001B §6 and §9.1 |

**Original behavior (observed):** after the 1996 Melbourne GP, the track's historical record (148 bytes; FMT-001B §6) was updated with **winner** (Damon Hill), **team** (Williams) and **pole** (Damon Hill, `1:33.169`), but the **fastest lap** fields stayed `N /A` (driver, offset `+0x1E`) and `9999/9999/9999` (time, offset `+0x18`). In the working record archived at the end of the table, the pole, the position and the race points were written; the fastest lap was not either. `[OBS]`

**Proof that the data existed in the simulation (`[CONF]`):** the career block of the driver record (`+0x20`-`+0x2B`; FMT-001B §5.5) has a fastest-lap counter at `+0x28`. Between the saves before/after Melbourne, **only Damon Hill** gains a fastest lap (14 -> 15), and no other driver changes that counter. That is, the race computed the fastest lap and updated the driver's statistic, but did not write driver/time into the track record. The bug is one of persistence in the 148-byte record, not of missing data.

**Expected corrected behavior:** at the end of each race, the port must write the fastest-lap driver and time into the track record, in addition to winner/team/pole. The time must use the same `min:sec.ms` format (3 x `uint16`, ms = 0-999) used for the pole.

**How to verify:** repeat a player race and an AI-simulated race in the original; save right after the race and after advancing to the next track; compare `+0x18`/`+0x1E` and `+0x5E`/`+0x64`. Confirm whether the fastest lap is only recorded in simulated races, or whether the field is never updated in this version.

**Confirmation criterion:** post-race save of the player's race with fastest lap `N/A` and a test with a simulated race showing the field filled (or total absence of updates in both cases).

**Regression test in the port:** run a race, save, reload and check the fastest-lap driver/time in the track record; if the original does not write the data, the fix must fill the field consistently with the history UI.

**Relation:** independent of BUG-015 (TOP CARS) and BUG-001 (electronic failures). May share code with the pole/winner update.

---

### BUG-024 — `.GCF` reader (`FUN_004adb60`) misaligns the bundled `DEFAULTS.GCF`

| Field | Value |
|---|---|
| Class | Data / Logic |
| Versions | 1.02B (`Gpm2.exe`); file `original/GPM2/DEFAULTS.GCF` |
| Evidence | `[CONF]` (decompiled reader + the oracle's memory after loading the file = reader applied to the file) |
| Severity | Medium (staff data loaded misaligned; "faulty" file dialog when loading a `.GCF`) |
| Status | Confirmed |
| Sources | `decompiled.c` `FUN_004adb60` (61242), `FUN_004ae600` (61390), `FUN_004ad4e0` (61017); FMT-017 §2; `FileMapsTests.testGCFMapAndBug024` |

**Original behavior:** the header is `[u16 a][u16 version]`. The writer (`FUN_004ae600`) writes `a=1`, `version=3` and 16 entries at `0x69DF01 + i*0x20`. The bundled `DEFAULTS.GCF` has `a=1`, `version=0` but **30 (0x1E) entries** in that table (old layout). The reader only uses the 30-entry loop when `version == 1`; with `version == 0` it reads 16, and the remaining 14 bytes of the table are consumed by the following loops (staff ratings `0x20 x 12` bytes), which end up **shifted by 14 bytes** (370 bytes different in memory, the first at `0x69D250`). At the end, `FUN_004ad4e0` reads 63 bytes of the footer 14 bytes early, the `strcmp` fails and shows the error dialog (strings `0x17B1/0x17B2`), returning 2: the origin of the "faulty" reputation.

**Expected corrected behavior:** handle the version according to the real table (version 0 and 1 => 30 entries, version >= 2/3 => 16) or, better, validate the size by the footer; load `DEFAULTS.GCF` without shift and without a dialog.

**Regression test in the port:** `DEFAULTS.GCF` read with the 30-entry map ends exactly at the footer (offset `d.count - 63`); with the original path, 14 bytes earlier.

**Relation:** the oracle's "PRE" memory (PLAN-003/004) already contains the effect (loaded by the real reader); `fn_42cc20` rewrites the staff ratings afterwards, but any other consumer of the `0x69DF01` table/pre-new-career staff sees wrong data.

### BUG-025 — Chat messages received while the chat is closed are lost

| Field | Value |
|---|---|
| Class | Network / UI |
| Versions | 1.02B (`Gpm2.exe`) |
| Evidence | `[CONF]` (decompiled; Windows behavior deduced, no capture) |
| Severity | Low |
| Status | Confirmed in code; fix without a two-instance test |
| Sources | `decompiled.c` `FUN_005072d0` case 0 (`LB_ADDSTRING` on `DAT_0057b2d6`), chat `WM_DESTROY` (only zeroes `DAT_0057b2d2`), `FUN_005050f0` |

**Original behavior:** closing the chat destroys the window and zeroes only `DAT_0057b2d2`. The next message goes to the old list handle and is lost. The chat comes back only through the hotkey (F7 by default, `0x71b950`), empty.

**Corrected behavior:** messages received while the chat is closed are kept and enter the list when it reopens. With "Bring Window To Top" checked, the new message reopens the chat, at the user's request (in the original, `BringWindowToTop` received a null handle). Without the option, the chat does not open by itself.

### BUG-026 — "Send Window To Back" reloads an old copy of the view

| Field | Value |
|---|---|
| Class | Logic / UI |
| Versions | 1.02B (`Gpm2.exe`) |
| Evidence | `[CONF]` (decompiled; symptom on OS 9: click reaches the right window, context and input unchanged, game does not react) |
| Severity | Medium |
| Status | Fixed in the port; no user test |
| Sources | `decompiled.c` `FUN_004f50e0` (85995), `FUN_004f81f0` (restores the view), `FUN_004f7d80` (saves the view, only on opening and on initial screens) |

**Original behavior:** with "Send Window To Back" checked, sending a message calls `FUN_004f50e0(0)`, which ends in `FUN_004f81f0(0)`. That routine reloads view 0 from the copy saved by `FUN_004f7d80` (WinG DC and pointer, tables of 0x2f80 and 0x251c bytes), even when view 0 is already the current one. The copy is old and the game stops responding to clicks.

**Corrected behavior:** if the requested view is already the current one, the reload is skipped.

---

## 4. Research sources

### Primary
- **Obtained:** `original/GPM2 Updates v1.02b/V102b.txt` (253 bytes), official notes of patch 1.02B. SHA-256 `06be2c20ab8bbdf12c037896ceb8caf255d4a3dc7dc1078796f063e354b06646`.
- **Obtained:** `original/GPM2 Updates v1.02b/GPM2-V1.02 Instructions.doc` (23,040 bytes), official 1.02 instructions with the **BUGS FIXED** section. SHA-256 `c83b4c7cf23f576ee8dc1cc247ca75e687b57277890496f6afcb0a33e726e592`. Text extracted with `textutil -convert txt -stdout`.
- **Obtained:** `original/GPM2 Updates v1.02b/Gpm2.exe` (PE32, 1,651,712 bytes), fixed executable. SHA-256 `266f214714b6a2d3396f969f0e142dc267b63d80beb255b9998f66ffcd253546`.
- **Obtained:** `original/GPM2/GPM2.EXE` (NE16, 2,388,976 bytes), base 1.00 executable. SHA-256 `bf58e09d41485835404aaf092689a7a2e645f22f8484e0d37b8b19218d04bcc8`.
- **To obtain:** original GPM2 manual (UK/US version) and reference guide.
- **To do:** binary diff 1.00 <-> 1.02B (the strongest source for knowing what was actually fixed).

### Secondary (reports already located)
- GPM Planet — *GPM2 Hex Editing Guide* (offsets of the 1.02B `Gpm2.exe`: engines, tracks, suppliers, parts, texts): <https://gpmplanet.proboards.com/thread/36/>
- GPM Planet — "Retirements" thread (EMS vs. driver aids mechanism, affected versions): <https://gpmplanet.proboards.com/thread/21/>
- GPM Planet — *GPM2 Ultimate Modding Guide* (EDY dimensions, transparency, unused assets, car number bug): <https://gpmplanet.proboards.com/thread/32/>
- TCRF — *Grand Prix Manager 2* (version differences, debug menus, unused content): <https://tcrf.net/Grand_Prix_Manager_2>
- GPM Planet — WIP editor (EDY format as raw indexed pixels; `.exe` findings): <https://gpmplanet.proboards.com/thread/319/gpm2-0b-editor-work-progress>
- jeuxvideo.com — guide to editing money in the `.GPS` save (decimal offset 2698): <https://www.jeuxvideo.com/wikis-soluce-astuces/avoir-plus-d-argent/202602>
- Abandonware France — page of the 1.02B patch ("converts to 32 bits, fixes some bugs"): <https://www.abandonware-france.org/ltf_abandon/ltf_jeu.php?id=1255&fic=liens>
- GPM Planet — "1.02b Patch" thread (crash on PCem; 86Box recommendation): <https://gpmplanet.proboards.com/thread/295/>
- GPM Planet — "Update Crash when Racing": <https://gpmplanet.proboards.com/thread/129>
- Race Sim Central — article about the GPM2 Launcher (mention of the fixed traction control bug): <https://racesimcentral.net/grand-prix-manager-2-launcher-utility-sets-up-game-environment-and-allows-single-click-running/>
- f1-forum.fi — thread "Grand Prix Manager 2 (apuva)" (electrical failure, save, Suzuka, pits, spins): <https://f1-forum.fi/threads/grand-prix-manager-2-apuva.1364/>
- Abandonia — old forum (spins, electronic problem, only one car): <http://www.abandonia.com/vbullet/printthread.php?s=673cd4fda59b93e79f2aa4db7682d82e&t=12244&pp=10&page=5>
- worldracing.info — save report and Renault anomaly in season 5: <https://www.forum.worldracing.info/thread-482-post-162480.html>

> Warning: search summaries and forums may mix versions, mods and emulation problems. No secondary source is treated as proof.

### Tertiary (to research)
- Usenet archives (Google Groups/archive.org) from 1996-1999.
- Magazines of the period (PC Gamer, PC Zone, Joystick) and letters/tips sections.
- Documentation of community save editors and trainers.
- Archived game sites (Wayback Machine) with FAQs and patch notes.

## 5. Next steps

1. Apply the 1.02B patch to a separate copy and preserve both versions in `/original/v100` and `/original/v102b`.
2. Extract the patch's README/changelog, if any, and attach it as evidence.
3. Diff the strings and sections of the executables; map the changed functions.
4. Reproduce BUG-001 through BUG-009 in 86Box with saves preserved in `extracted/savegames/`.
5. Promote to `CONFIRMED` only the items with reproduction or diff; discard those that do not hold up.
6. For each fix, create a regression test and record it in `_TEMPLATE.md`.
