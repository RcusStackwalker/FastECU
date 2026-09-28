# Step 6l — Logging model migration

Introduce a portable LoggerModel, then migrate desktop consumers and retire the
legacy logging lists and bridge. Preserve behavior except for two corrections:
chooser items resolve by protocol and ID rather than label; CSV columns resolve
within the active run's protocol rather than the first matching ID.

LoggerModel owns an immutable definition installed once, a separate selection,
and separate ECU support. Definition enabled fields retain XML defaults.
Parameters and switches use separate (protocol, ID) namespaces. Preserve order.
DesktopComposition owns the model and LoggerDefinitionService; MainWindowServices
exposes both. The GUI owns values initialized to parameter "0.00" and switch "0".
Selection persistence accepts an explicit fallback selection, with no runtime
flags copied into definitions. Startup limits remain 15 gauges, 12 digital
values, 20 switches; missing-ECU defaults use support in definition order.

Capability parsing retains its asymmetry: unavailable parameter bytes disable
parameters, unavailable switch bytes retain previous support. Identification
without capabilities leaves support untouched. Unreadable selection files retain
selection; successful reads clear IDs before resolution. Missing definitions
never initialize an absent ECU entry. Failed saves retain in-memory edits.
Definition failures are nonfatal.

Snapshots capture protocol and stable identities, LoggingSession, response
offsets, enabled IDs and ECU/TCU target. Workers receive value snapshots. Polling
and reconnect policy remain in LoggingUseCase::run(). Preserve SSM disabled-slot
offsets and decimal-byte assembly, MUT/DMA enabled filtering, CDBG selection,
first conversion and fixed decimal formatting.

Migrate startup, chooser, display, CSV, snapshot and sample application together.
Retain unresolved IDs when saving, skip unresolved displays and leave empty CSV
cells. Preserve file schemas, formatting, column order and lifetime. Remove
LogValuesStructure, LegacyLoggerAdapter and logger-specific FileActions code,
obsolete targets/tests and logging legacy Qt visibility. Register portable targets.

Deliver two reviewable commits/PR slices: 6l-1 portable model, capability and
persistence groundwork with identity reproductions; 6l-2 consumer cutover and
retirement. Tests cover support-aware defaults, round trips, persistence failures,
nonfatal loads, duplicate labels, shared IDs, reordered definitions, unresolved
IDs, unknown samples, immutable snapshots, connection/cancellation/restarts and
teardown. Prove both identity corrections with mutations. Run focused tests,
release build/tests, changed-file clang-tidy, prek and available platform packaging.
Windows/Linux CI and hardware qualification require their actual environments.
Android, live reconfiguration, CDBG wire changes and calibration are out of scope.
