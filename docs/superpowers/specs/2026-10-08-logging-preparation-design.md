# Portable logging preparation and sample validation

Status: design confirmed by the user on 2026-10-08; ready for implementation
planning. This document does not authorize implementation.

## Agreed scope

Address the first action under [P1: Separate UI from application logic](../../tech-debt.md#p1-separate-ui-from-application-logic):
move logging snapshot/channel preparation and sample validation into backend
logging together, reusing existing session validation and sample conversion.

Qt formatting, display-cache updates, workers, and transport binding retain their
desktop owners. CSV migration and selection-persistence orchestration remain
separate debt items.

The [logging contracts](../../reference/logging-contracts.md) govern selection,
support, protocol identity, per-run ownership, and raw assembly. Compatibility
does not require preserving Qt numeric conversion quirks that lack a useful
meaning for definition authors. Define the accepted input syntax explicitly
before implementation.

## Definition input syntax

Addresses are hexadecimal digits with an optional `0x` or `0X` prefix. Lengths
are positive decimal integers. Both allow surrounding ASCII whitespace and reject
signs, internal whitespace, trailing junk, and overflow. Existing protocol address
and length bounds continue to apply.

Display formats are exactly `0`, or `0.` followed by 1–15 zeros. The number of
fractional zeros specifies fixed decimal precision. Other patterns are invalid;
the error identifies the parameter and invalid format. Existing parser defaults
remain applicable when the XML attribute is absent.

This deliberately replaces the desktop adapter's accidental acceptance of
arbitrary nonempty format strings. For example, `banana` currently produces zero
decimal places and `0.0garbage0` produces two. Neither has a defined author-facing
meaning under the agreed grammar.

Empty units are valid and mean a value displayed without a unit suffix. Preserve
the parser's defaults for omitted attributes; validate the resulting effective
expression and format rather than rejecting an omitted XML attribute itself.

## Prepared run ownership

Backend preparation returns one immutable, owned logging run snapshot containing
the validated session/channels, protocol, selection, per-channel support
eligibility, original SSM response offsets, and a typed ECU/TCU target. Derive
sample identities from the captured protocol and validated channel IDs instead
of independently mutable identity maps. Later definition, selection, support,
or target edits do not change an active run.

The desktop captures operator inputs before preparation and retains widget
access, adapter capability checks, and transport binding. Capturing selection
does not change CSV's contract: CSV resolves the current selection within the
active run's captured protocol.

## Validation and failure handling

An invalid participating selected channel prevents logging from starting. Errors
identify the protocol, parameter ID, offending field, and reason. Unresolved
selection IDs retain their existing treatment; protocol-specific support and
selection rules remain governed by the logging contracts.

Unsupported MUT/DMA channels are filtered before field validation. Unsupported
SSM channels still participate in polling and must validate; CDBG does not filter
by support. The startup dialog displays the actionable definition error as well
as logging it, while retaining desktop ownership of the dialog.

Unknown raw protocol channels and nonfinite conversions terminate the run before
publishing any samples from that polling batch. Delivered-sample identity errors
and missing display-cache entries report an error and allow other samples to
continue; existing cache values remain intact.

Portable sample resolution returns an accepted identity/value/precision, a
deliberate skip, or an error. Unsupported SSM samples remain polled but do not
update display values. Desktop code formats accepted values and owns cache
lookup, mutation, and reporting of cache inconsistencies.

## Current implementation evidence

The [desktop snapshot adapter](../../../src/platform/desktop/common/logging/logging_snapshot_adapter.cpp)
owns selection resolution, support rules, response offsets, channel construction,
numeric parsing, and format interpretation. The
[desktop value adapter](../../../src/platform/desktop/common/logging/logging_value_adapter.cpp)
checks sample identity and enabled status before formatting and updating the
display cache.

Backend [session preparation](../../../src/backend/logging/logging_session.cpp)
already validates channel and wire shape; backend
[sample conversion](../../../src/backend/logging/logging_conversion.cpp)
already rejects unknown channels and nonfinite converted values.

## Implementation acceptance evidence

Move existing preparation and sample-resolution policy cases into package-owned
backend GoogleTest suites. Cover accepted and rejected numeric syntax, format
grammar and precision limits, empty units, contextual errors, immutable captured
inputs, identity namespaces, support rules, and original SSM offsets. Retain
desktop tests for formatting, cache updates, actionable startup error display,
target binding, and worker lifetime. Reuse existing run-loop tests for fatal
conversion failures and batch publication behavior.

Run focused package tests and the applicable repository-wide Bazel, formatting,
Gazelle, and static-analysis gates during implementation. No wire behavior change
or hardware qualification claim is part of this design.

## Documentation lifecycle

During implementation, update the owning logging contracts and design notes,
including implementation links and the explicit definition-input grammar.
Remove the completed first debt action only after its whole scope is delivered.
Preserve this proposal in delivered Git history before removing it under the
[completed-document lifecycle](../../README.md#recover-completed-work-and-deleted-source-citations).
