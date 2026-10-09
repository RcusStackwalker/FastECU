# Splitting logging preparation PR #558

Status: split boundaries and shared understanding confirmed by the user on 2026-10-10.
This document does not authorize implementation.

The [confirmed preparation design](../specs/2026-10-08-logging-preparation-design.md)
defines the final behavior. Its [original implementation plan](2026-10-08-logging-preparation.md)
remains historical input; this discussion determines delivery boundaries against
current master.

## Settled decisions

- Prioritize smaller PRs and represent dependencies as a GitHub PR stack. Each
  dependent PR is based on its predecessor so its review diff contains only its
  own changes.
- Backend APIs may land before desktop adoption. Until adoption, the application
  continues to use its existing preparation path; passing backend tests alone
  does not establish that desktop behavior has changed.
- Deliver four backend PRs in dependency order: shared per-channel validation
  extraction, definition-channel preparation, immutable run preparation, and
  sample resolution. Keep validation extraction distinct from new definition
  parsing rules, and keep run construction distinct from sample resolution.
- Deliver actionable plain-text startup errors as a separate UI PR before
  desktop adoption. It displays existing error details immediately and richer
  definition errors once backend preparation is adopted; verify failed startup
  leaves logging stopped.
- Prepare ordinary desktop test fixtures through the existing factory in a
  separate PR before snapshot adoption. Keep package-owned helpers and assertions
  before unwrapping results. Retain mutable-snapshot invariant tests until the
  mutable representation is replaced; do not introduce bypass constructors.
- Separate desktop snapshot adoption from desktop sample-resolver adoption.
  Snapshot adoption changes all consumers of the public aggregate together.
  Temporarily retain value-adapter policy using the validated session, captured
  eligibility and derived identity; the next PR delegates it to the resolver.
- Preserve the original agreed numeric/format grammar and empty-unit behavior.
- Reuse current master's shared expression evaluator and validation probes.
  PR #576 removed the duplicate validator; restoring the old #558 parser would
  regress currently accepted expressions.

## Source findings

The existing desktop snapshot already owns captured inputs. The proposed backend
snapshot strengthens validated construction and private immutable state, captures
a typed target at construction, and removes redundant identity maps. It does not
introduce frozen CSV selection: CSV retains current selection within the captured
protocol throughout this work.

The broad desktop migration is caused by consumers and fixtures directly using
the existing public aggregate. Removing its defensive identity checks before
replacing that aggregate would weaken the intermediate implementation.

## Delivery sequence

Preserve the original design and plan through the separate documentation PR #557
before any later completed-document deletion. Rebuild implementation slices
against current master, including its style migration and shared evaluator;
do not cherry-pick the old mixed implementation wholesale.

| PR | Scope | Intermediate application behavior |
| --- | --- | --- |
| 1 | Extract shared per-channel validation | Existing session behavior retained; aggregate checks remain session-owned |
| 2 | Portable definition-channel preparation and tests | New strict parsing API exists; desktop still uses its existing parser |
| 3 | Immutable run snapshot and validating factory | New run API exists; desktop still uses its public snapshot |
| 4 | Portable sample resolution and tests | New resolver exists; desktop still uses its current value adapter |
| 5 | Prepare ordinary engine, transport and UI snapshot fixtures through the existing factory | Production behavior unchanged; preserve invariant tests for the mutable representation |
| 6 | Actionable plain-text startup error dialog | Existing error details become visible; startup failure remains stopped |
| 7 | Adopt immutable runs across desktop consumers | Strict definition preparation becomes active; capture typed target before preparation; preserve transport and CSV behavior |
| 8 | Adopt backend sample resolution in the desktop value adapter | Remove temporary desktop resolution policy while retaining formatting, cache ownership and error continuation |

Each PR is based on the preceding stack branch. Review and verify its own diff;
each intermediate state must build and pass its applicable gates.

For PR 7, unknown delivered channel IDs still report `Internal`, unsupported SSM
samples still skip successfully, and missing cache entries still report errors.
Derive identity from captured protocol and validated channel ID. Remove the
independent identity-map disagreement guard only when that map disappears.
Keep the MainWindow snapshot recheck between samples because error reporting can
synchronously stop a run. Retain worker-generation, target, setup-order and
original SSM-offset coverage.

PR 5 replaces the fabricated nonmonotonic SSM offset fixture with a real
selection containing a missing ID, preserving coverage that selection gaps reach
transport/sample mapping. Arbitrary protocol offset ordering belongs in the
protocol tests.

Update enduring contracts with the PR that activates each behavior. Remove the
completed preparation debt only after the full scope is delivered. Preserve the
original design and plan in delivered history before deleting completed documents.
Backend-only PR descriptions must distinguish tested API behavior from active
desktop behavior. Retire the superseded #558 after the replacement stack is
published, preserving links to its replacements. Physical SSM planning, expanded
selection acquisition and frozen display/CSV behavior remain outside this stack.
