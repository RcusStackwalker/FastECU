# Colt Vendor-Key Response Implementation Plan

**Goal:** Accept the ECU-confirmed `63 27 34` vendor-key success reply and reject the formerly assumed `63 27 42` reply.

**Architecture:** Keep the vendor challenge request and key calculation unchanged. Update the executor's existing response predicate and exercise the complete executor flow through `ScriptedCanFlashTransport`.

**Tech Stack:** C++23, GoogleTest/GoogleMock, Bazel

## Constraints

- The key request remains `23 27 42 <four-byte key>`.
- Only `63 27 34` grants vendor access.
- Short and mismatched responses stop execution before diagnostic-session entry.
- Do not modify unrelated factory SecurityAccess handling.

## Steps

- [x] Change the success fixture to `63 27 34` and verify the focused test fails.
- [x] Add a named `0x34` acceptance constant and use it in the executor predicate.
- [x] Verify the focused success test passes.
- [x] Change the rejection fixture to `63 27 42`.
- [x] Run the full executor target, adjacent vendor protocol target, and `git diff --check`.
