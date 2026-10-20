# Execution decisions and tradeoffs

This dated record preserves the execution rulings in order, including their
costs. Later refinements and the current analysis boundaries are described in
[tooling](../../tooling.md#repository-checks). The
[qualification record](../clang-tidy-plugin-qualification.md) owns results.

1. Create a linked worktree under the existing ignored .worktrees directory — native execution authorizes reversible workspace setup; preserve unrelated root files — cost if wrong is an extra local worktree, removable after delivery.

2. Windows linked-host qualification requires Task 2's build integration, so finish that part of Task 1 alongside Task 2 — keep Task 1 incomplete until real platform evidence exists — cost if wrong is integration work before Windows runtime confirmation.

3. use the selected LLVM Clang compiler for the Linux analysis job, leaving the product build job's compiler unchanged — avoids GCC-only LLVM-header optimizer diagnostics without warning opt-outs and aligns analysis-extension compiler with host release — cost if wrong is additional Clang prebuild findings and compile-cache separation.

4. combine migration and runner consolidation in a dependency-closed delivered change because native fixtures now call the unified filter helper — independently recorded parity remains the migration gate — cost if wrong is a larger review unit.

5. single-use detection initially requires the adjacent pair to end its block, plus a void Result and a pure IsOk matcher — preserves observable cleanup ordering without guessing about later statements — cost if wrong is narrower automatic coverage, documented as review-only outside the proof.

6. document the 13 Mitsu write-fixture test bodies as exact Result-access exceptions, make the shared Result member const, and retain the fatal SetUp guard — GoogleTest skips bodies after fatal setup failure and immutable state remains proved; avoid redundant assertions and do not infer interprocedural framework proofs — cost is 13 auditable entries which full-scope unused validation will retire as tests change. This is a supported proof exception, not a violation baseline.

7. use bounded structured intraprocedural state analysis rather than the plan's suggested CFG/dataflow implementation — the binding spec requires state proofs, mutation invalidation and terminating guards, not a particular LLVM API; functions with goto, switch, or exception handlers conservatively receive no propagated proofs, avoiding invented paths — cost is false positives in these uncommon shapes, requiring simplification or an exact rationale exception. Jump/handler regression fixture observed RED before this boundary.

8. select pinned clang-cl for the Windows analysis job, analogous to Linux Clang — official LLVM headers trigger MSVC C4805 under /WX despite being system headers; keep first-party warnings enabled without opt-outs — cost is separate compiler/cache behavior in analysis prebuilds. Product MSVC builds already pass unchanged.

9. retain original approved spec/plan in this PR until delivered history is preserved on master, then retire in a separate change — adding and deleting them in the same squash would lose their contents, contrary to AGENTS.md — cost is temporary active-work documents after implementation qualification.

10. repeated migration/coverage/extension timings isolate repository checks on the complete frozen 546-TU graph, with built-ins disabled equally in all modes — separates the implementation variables rather than charging unchanged analyzer cost to migration; full combined built-in/native correctness and profile are verified separately, and the existing public benchmark remains available for full combined-mode timing — cost if wrong is unmeasured timing interaction with built-ins, so report no end-to-end CI speedup claim. The isolated benchmark still measures parse/load/analysis wall time and investigates its >10 percent regressions.

11. regrade the nested SCOPED_TRACE false positive as Important — valid assertion-free loops must not fail the new CI convention gate — cost is one extra targeted regression and fix in the same pass.

12. cross-function captured/framework proofs stay bounded out; independently analyze lambda-local bodies, keep immutable fatal-SetUp exceptions — users get local state checking without invented caller guarantees — cost is explicit local guards or justified framework exceptions.

13. unknown pointer aliases and complex matcher compositions remain documented boundaries — do not invent identities or matcher semantics — cost is review for missed aliases and conservative diagnostics for unrecognized guards.

14. complete CFG proofs through goto, handlers and switch bodies remain outside bounded structured reasoning — conservative barriers prevent false proofs — cost is false positives requiring simplification or exact rationale.

15. expense classification, parameter-name quality and assertions hidden in separate helpers remain review responsibilities — no unreliable heuristic — cost is manual review of those conventions.

16. arbitrary custom byte containers/projections remain outside supported typed patterns — permitted counterexamples stay allowed — cost is incomplete automatic coverage for those types.

17. mechanical rename/trace/generator edits receive representative independent inspection plus compilation, test and analysis gates — bounded review rather than claiming exhaustive inspection — cost is residual mechanical-change risk.

18. timing and Linux qualification are pending gates, not waived defects — keep draft status and finish evidence before delivery — cost is additional runtime and external CI dependence.

19. hardware qualification is outside this software-tooling change — make no hardware claims — cost is unchanged need for recorded bench evidence for hardware-facing work.

20. add two balanced measurement rounds after the initial three — a large host-speed transition made raw medians depend on where each mode fell in the schedule; identical fingerprints and forward/reverse rounds distinguish stable feature cost from that drift — cost is eight extra local analysis passes, concurrent with pending Linux CI. Preserve every original sample and report the variability, not a selected best run.

21. retain the required new checks and report the noisy timing observations without asserting a causal header-coverage slowdown — the 10-percent threshold requires investigation, not a timing gate; profiles identify required loop-trace, uniform-byte and single-use analysis, and all raw samples remain visible — cost is added analysis runtime and an inconclusive isolated header-coverage cost estimate on this host.

All independent review findings were fixed. No minor findings were deferred.
