# ADR 0014: Check Result with has_value

## Status

Accepted

## Context

`fastecu::Result<T>` is `std::expected<T, Error>`, and `fastecu::Status` is
`Result<void>`. Both convert implicitly to `bool`, so a success check can be
written either as `if (!result.has_value())` or as `if (!result)`.

The codebase had already settled on the explicit form by a wide margin --
about 1277 `.has_value()` uses across `src/backend` and `src/algorithms`
against at most about 139 sites still written as the implicit `if (!x)` /
`; !x)` form. That second count is an upper bound, not an exact one: the
pattern also matches plain `bool` variables, not only `Result`/`Status`, so
the true operator-bool count is somewhat lower. Either way nothing recorded
the choice, so new code drifted both ways.

The implicit form is also ambiguous in a way that matters here. For a
`Result<bool>` and a `Result<std::optional<T>>`, `if (result)` reads as a
question about the contained value, not about success, and the two readings
disagree exactly when the contained value is falsy.

## Decision

Check `Result` and `Status` with `.has_value()`.

Do not use the implicit `operator bool`. Init-statement conditions keep their
form and spell the check out: `if (Status s = f(); !s.has_value())`, not
`if (Status s = f(); !s)`. There is no reason to split the declaration out of
the `if` -- doing so would widen the variable's scope to no benefit.

## Consequences

Success checks read the same everywhere, and no reader has to know a type's
value category to know what is being tested. The remaining operator-bool
sites are a legible cleanup rather than an inconsistency; they are tracked in
a follow-up issue and converted opportunistically as files are touched, not
in one repo-wide diff.
