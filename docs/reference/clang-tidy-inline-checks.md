# Inline clang-tidy checks

Repository checks are the `CustomChecks` entries in [the configuration](../../.clang-tidy).
All report errors with no replacements.

## Scope

- **Result assertions:** GoogleTest boolean assertions on `std::expected<T, fastecu::Error>`.
  Plain `std::optional` and the low-level Result API suite are excluded. Only the
  direct boolean operand is matched, optionally negated once. Compound expressions
  and predicates nested in unrelated calls or aggregates are excluded; callbacks are not.
- **Container assertions:** boolean container predicates, with the same operand rules.
- **Message concatenation:** `"text " + x` passed directly to `fastecu::Fail` or
  `IEventSink::Log`, following transparent temporary and conversion wrappers. Not
  matched: arbitrary functions, locals traced across statements, path construction
  nested in another call. Directly formatted messages are allowed.
- **Headers:** first-party source, application, test, and script headers, with native
  and normalized separators. Coverage depends on the translation units buildable on
  the host; a macOS scan does not cover Windows-only code.

## Redundant address/dereference (`fastecu-redundant-address-dereference`)

Flags `&` applied to a built-in `*`, after parentheses and implicit casts. Both
operators must be built-in. Overloaded iterator `*`, optional/expected access, and
overloaded `&` are allowed. Dependent templates are checked only on resolved
instantiations. `*&` is not matched. Macro-generated pairs are matched, which is
broader than Sonar cpp:S3491.

Fix: use `std::to_address(p)` for object pointers, keeping any `end()` guard. For
function pointers, remove the pair, since `std::to_address` does not accept them.

## Validation

- [Fixture harness](../../scripts/clang_tidy_checks_test.py): GoogleTest suites compiled
  with the Bazel flags; diagnostics are compared to severity-tagged markers.
- [Adapter tests](../../scripts/clang_tidy_adapter_test.py) and
  [runner tests](../../scripts/clang_tidy_runner_test.py).
