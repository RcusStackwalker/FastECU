# Re-slicing PR #23 and #24 into focused, reviewable PRs

**Date:** 2026-07-18
**Status:** Proposed
**Goal:** Reviewability — replace two large PRs with a set of small, single-concern
PRs a human reviewer can actually read end to end.

## Background

Two open PRs advance the qmake → Bazel migration (ADR 0001 / 0007) but are each too
large and multi-concern to review well:

- **PR #23 — "build: complete Bazel migration, remove qmake"** — +181 / −1472, 8 commits.
  Adds Bazel packaging scripts, cuts the release pipeline over to them, and deletes qmake
  (`.pro`/`.pri`, flatpak, the qmake↔bazel sync test). Based on current `master` tip.
- **PR #24 — "build: make Bazel clang-tidy actionable"** — +1201 / −290, 10 commits.
  One cohesive feature (a clang-tidy runner replacing `clang_tidy_report.py`, plus report
  and autofix Bazel targets), developed with 7 iterative `fix:` commits. It also drags in
  a hermetic Python + pip(pyyaml) toolchain. Based one commit behind `master`.

### Key facts established during analysis

- PR #23's commits are already cleanly separated by concern; most slices are pure
  `git cherry-pick` along commit boundaries.
- The `external/ → hardware/` rename claimed in PR #23's description **is not in its diff** —
  stale body text. Drop that claim; do not build on it.
- PR #24's clang-tidy `py_binary`/`py_test` targets declare `deps = ["@python_deps//pyyaml"]`
  (used to parse clang-tidy's `-export-fixes` YAML). The hermetic Python/pip toolchain is
  therefore a **genuinely required foundation** for the clang-tidy targets, not an orphan.
- The two PRs both edit `BUILD.bazel`, `.github/workflows/pr.yml`, and `.gitignore`, and
  PR #24's local verification *runs* `//:qmake_bazel_sync` while PR #23 *deletes* it — i.e.
  PR #24 was built on a pre-removal world. The clang-tidy **targets** do not reference
  `qmake_bazel_sync`, so the two efforts are functionally independent.

## Decisions

- **Split motivation:** reviewability (minimal, single-concern diffs).
- **PR #24 granularity:** three PRs (Python infra → runner+report → autofix+docs).
- **Branch strategy:** sequential off `master`. Each PR targets `master` and opens only
  once its predecessor has merged. No stacked branches, no rebase chains.

## The plan: 6 PRs in two independent tracks

The two tracks share no functional dependency and may progress in parallel; within a track
the PRs are strictly ordered.

### Track 1 — finish the migration (from PR #23), ordered safest-first

**PR A — `build: add Bazel packaging scripts + PR-job verification`**
- Contents: `scripts/package-macos.sh` (`cb9e679`), `scripts/package-windows.ps1`
  (`068636b` + `69eaed0`), PR-job package verification steps (`3846b78`), the bundle-assert
  additions and `.gitignore` hunks from `808b8ec`.
- Shape: ~+130 lines, additive. **Touches no qmake file; fully reversible.**
- Reviewer focus: the scripts assemble correct bundles and the Qt/OpenSSL asserts fail a
  partial deploy.
- Depends on: nothing.

**PR B — `ci: cut release pipeline over to Bazel packaging`**
- Contents: `release.yml` rewire (`6223b1e`).
- Shape: ~+22 / −32, one file.
- Reviewer focus: release wiring mirrors the CI-proven `bazel` job. Carry PR #23's caveat —
  `release.yml` only runs on push to `master`, so it can't be exercised pre-merge; watch the
  first post-merge release artifacts.
- Depends on: A (uses the packaging scripts).

**PR C — `build: remove qmake — Bazel is the sole build graph`**
- Contents: drop the redundant qmake PR job (`35d26a3`); delete all `.pro`/`.pri`, the
  flatpak manifest + icons, the qmake↔bazel sync test/scripts, and flip ADR 0001 to Accepted
  (`767ac7d`); the `sonar-project.properties` `external/` exclude hunk from `808b8ec`.
- Shape: large deletion (~−1200). Isolated so the reviewer sees exactly what is removed.
- **The point of no return** — nothing before this deletes qmake, so A and B stay revertible.
- Reviewer focus: nothing still-live references the deleted files; Bazel already covers every
  build/test/release path.
- Depends on: B (release must build via Bazel before qmake leaves the release path).
- **Do not** re-assert the `external/ → hardware/` rename in the PR description.

### Track 2 — clang-tidy tooling (from PR #24), ordered D → E → F

**PR D — `build: add hermetic Python + pip(pyyaml) toolchain`**
- Contents: `MODULE.bazel` python/pip extension, `requirements.txt` (pyyaml 6.0.3),
  `MODULE.bazel.lock`, and the toolchain pin (`db52f35`).
- Shape: small, foundational infra.
- Reviewer focus: hermetic 3.11 toolchain and a single pinned, hashed dependency.
- Depends on: nothing.

**PR E — `build: actionable clang-tidy runner + report target`**
- Contents: the runner + tests (`a6ac10a`), the report Bazel target (`1fc5d75`), expose the
  shared `compile_commands` target (`657c31d`), and the report-oriented fixups (macOS SDK
  `5ec9a10`, unreadable-database reporting `18a6e44`). **Deletes** `scripts/clang_tidy_report.py`.
- Reviewer focus: the runner reproduces (and replaces) the old report behavior from the
  Bazel-derived compilation database.
- Depends on: D (targets need `@python_deps//pyyaml`).

**PR F — `build: clang-tidy autofix target + docs`**
- Contents: the `clang_tidy_fix` target and replacement normalize/canonicalize logic
  (`7e2687a`, `45523e7`), README + `pr.yml` workflow wiring (`9fc8215`), and the worktree
  `.gitignore` entry (`445ed0d`).
- Reviewer focus: deferred, deduplicated, canonicalized replacement application; idempotent
  (`clang_tidy_fix` run twice leaves an unchanged tracked diff).
- Depends on: E.

## Split cost — stated honestly

- **Track 1 slices cleanly along commit boundaries.** Only `808b8ec` needs splitting: its
  packaging-assert + `.gitignore` hunks go to A, its `sonar-project.properties` hunk goes to C.
  Everything else is a straight `git cherry-pick`.
- **Track 2 needs one in-commit split, but only at file granularity.** `7e2687a`
  ("normalize clang-tidy replacements safely") mixed the **pip infra** (`MODULE.bazel`,
  `MODULE.bazel.lock`, `requirements.txt` → D) with the **runner autofix logic** (`BUILD.bazel`,
  `scripts/clang_tidy_runner*.py`, `README.md` → F) in one commit. The split is whole-file, so it
  is a `git cherry-pick -n` + `git restore` of the other track's files — no hunk dissection. This
  is the only non-clean cherry-pick in either track.

## Rejected alternatives

- **One combined 6-PR stack (A→B→C→D→E→F):** unnecessary serialization; the tracks are
  independent, and chaining clang-tidy behind the qmake removal only delays it.
- **Stacked branches instead of sequential-off-master:** rejected per branch-strategy
  decision — keeps later PRs red/blocked and forces rebase chains.
- **Leaving PR #24 as a single squashed PR:** rejected in favor of three PRs; the pip
  toolchain is a distinct foundational concern from the runner and the autofix.

## Success criteria

- Six PRs open/merge in the two orders above, each single-concern with a diff a reviewer can
  read in one sitting.
- No PR before **C** deletes any qmake file (A and B remain revertible without touching qmake).
- CI is green on each PR at merge time; `master` is never left in a state where Bazel does not
  cover a build/test/release path that qmake previously covered.
- The stale `external/ → hardware/` rename claim is not carried into any PR description.
