# Parser Utilities Library Design

## Goal

Reduce the size and compile-time surface of `parser_utils.h` while preserving
the shared implementation that keeps the RomRaider and EcuFlash parsers below
SonarQube's duplication threshold.

## Structure

- Add a portable `//src/backend/definition:parser_utils` C++ library.
- Keep declarations and the callback-dependent `populate_axes` function
  template in `parser_utils.h`.
- Move every non-template helper implementation to `parser_utils.cpp`.
- Make the RomRaider and EcuFlash parser libraries depend on `:parser_utils`
  instead of compiling the utility header as a source.
- Add `:parser_utils` to the repository's portable-closure roots.

## Behavior

This is a behavior-preserving refactor. Parser error messages, optional-field
semantics, normalization rules, and public parser APIs remain unchanged.

## Verification

- Run both co-located parser test suites.
- Run the definition-model tests and portable-closure check.
- Run formatting and lint hooks.
- Confirm SonarCloud's PR quality gate remains below the 3% new-code
  duplication threshold.
