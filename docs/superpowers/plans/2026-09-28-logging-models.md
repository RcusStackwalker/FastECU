# Logging models implementation plan

Spec: [Logging models design](../specs/2026-09-28-logging-models-design.md).

1. Add LoggerModel (immutable installed definition, selection setters, support
   queries/updates, capability application and support-aware default selection).
   Add portable tests for independence, identity, limits, capability asymmetry.
   Change LoggerDefinitionService fallback argument to LoggerSelection; adapt
   legacy caller and service tests. Run logging package tests and commit 6l-1.
2. Wire model/service through desktop composition. Add desktop cache keyed by
   protocol and ID with separate parameter/switch maps. Migrate snapshot builder
   and sample updates; preserve protocol filtering, offsets and conversions.
   Migrate startup, capability parsing, chooser (item data identities), displays
   and CSV (captured protocol, empty unresolved cells). Adapt existing tests and
   add exact chooser/CSV regression assertions and mutation checks.
3. Remove logging legacy files, FileActions methods and obsolete test cases,
   targets/dependencies/Qt visibility. Update roadmap/design notes. Run focused
   desktop/connection/worker tests, repository release build/tests, tidy, prek
   and local packaging. Record unavailable platform/hardware gates. Commit 6l-2.

Review focus: selection failure semantics; no definition reinstall; CSV file
lifetime across runs; supported switches with missing bytes; unresolved display
slots; protocol identity in delayed samples. The two tasks share the model and
service interface from task 1; consumers must never mutate the installed definition.
