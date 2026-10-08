# Logging Selection and Run Ownership Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans for the user's selected native execution, or superpowers:subagent-driven-development if the user changes that preference. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Acquire every selected logging measurement, distinguish definition enablement from ECU support, freeze active display/CSV bindings, and give every run a new CSV file.

**Architecture:** Normalize definition metadata and keep ECU support as a separate three-state model. Prepare unique logical measurements and their protocol read layout into an immutable run snapshot, then convert each complete polling batch into parameter or switch values. Desktop code renders the captured selection, saves pending edits, and owns exclusive CSV file creation and lifetime.

**Tech Stack:** C++23, Qt 6 at desktop boundaries, Bazel 9.1.1, package-owned GoogleTest, Gazelle-managed BUILD attributes.

**Spec:** [Confirmed logging selection design](../specs/2026-10-08-logging-selection-policy-design.md).

## Global Constraints

- Backend/algorithms have no Qt, owned threads, direct filesystem I/O, or platform dependencies. Caller composition owns execution and lifetime.
- Acquire all selected gauges, Digital values, and switches. Repeated display identities share a measurement; parameter and switch namespaces remain distinct.
- Unknown support does not exclude explicitly selected user-authored MUT measurements. No firmware-match or capability-discovery gate is introduced.
- Known unsupported and unresolved selections remain visible and prevent startup with actionable errors. XML enablement is default eligibility, not ECU support evidence.
- Retain native MUT 16-bit request codes, widths 1/2/4, first conversion, arithmetic `x`, empty units, and fixed precision 0–15. Do not silently truncate full RAM addresses or relax address-window guards.
- Preserve SSM decimal-byte concatenation and historical 15-digit intermediate expression evaluation independently of correcting response mapping.
- Active acquisition, displays, and CSV columns use captured run data. Saved edits apply on the next run with a visible restart-needed state.
- Every new run gets a new CSV when file logging is enabled, including identical restarts and Connect restarts. Never append another run or overwrite a completed file.
- XML authoring is this scope. The in-app editor and construction of a new gauge renderer remain follow-ups; switch value indicators and gauge-selection acquisition/cache/CSV are included.
- Existing `ErrorKind` values, `.has_value()` checks, warnings-as-errors, visibility groups, package-owned mocks, and worker generation/lifetime guards remain mandatory.
- Read [logging contracts](../../reference/logging-contracts.md), [coding conventions](../../coding-style.md), [Gazelle ownership](../../adr/0017-generate-bazel-targets-with-gazelle.md), and [logging composition qualification](../../checklists/logging-composition-bench-checklist.md). Static/scripted evidence does not establish hardware qualification.

## Review Focus

- A gauge and Digital slot sharing an ID must receive one acquisition; a switch with the same ID must still receive its distinct value (Tasks 3–4).
- Missing capability metadata or bytes must leave support unknown; no stale support evidence from a previous connection may authorize the new target (Task 2).
- SSM two-byte values, explicit address lists, filtered/missing IDs, and switch bits must map to actual requested response positions (Tasks 1, 3–4).
- A truncated but checksummed stream must not become valid zero readings or leak preceding samples from a failed batch (Tasks 1, 4).
- Two identical runs in the same second, a pre-existing filename, and failed file creation must preserve old files and fixed header/row column meaning (Task 6).

## Delivery and Evidence Gates

This plan builds on the portable preparation implementation. Start from a revision containing its reviewed changes; resolve the dependencies on PRs #557/#558 before publishing this follow-up. Preserve the current uncommitted glossary, README, evidence/debt, and confirmed spec changes with named paths before implementation setup.

Task 1 is an evidence gate. SSM byte mapping has primary source evidence already. MUT/DMA has a demonstrated discrepancy between the maintained codec and the analyzed 33520003 firmware, plus firmware-specific capacity limits. Characterization must produce a reviewable contract before changing that wire path. If fixing it requires dialect selection or routing beyond the confirmed XML scope, write and approve that focused amendment before proceeding with the affected MUT portion; continue independent tasks. Do not invent a universal 96-entry ECU limit, change generic byte order from one ROM alone, or claim the complete implementation delivered while this prerequisite remains unresolved.

The remaining tasks use current protocol APIs except where explicitly replaced below. Each task must compile its affected consumers; migrate call sites in the same task when an interface changes. Intermediate commits stay on the feature branch until the full integrated gates pass.

## File Structure

| Files | Responsibility |
| --- | --- |
| Create `docs/reference/logging-wire-evidence.md`; modify existing logging evidence/debt owners | Checked SSM/MUT wire contracts, scope, and unresolved qualification |
| Modify `src/backend/logging/logger_definition_model.h`, `logger_definition_parser.cpp`, `logger_model.{h,cpp}` and their tests | Separate sample/capability metadata, definition defaults, and support state |
| Create `src/backend/logging/logging_read_plan.{h,cpp}` and `_test.cpp` | Ordered SSM physical byte reads and response positions |
| Modify `logging_types.h`, `logging_channel_preparation.{h,cpp}`, `logging_session.{h,cpp}`, `logging_run_snapshot.{h,cpp}` and their tests in `src/backend/logging` | Logical identities, normalized read sources, selection union, immutable bindings |
| Modify `logging_conversion.cpp`, `logging_sample_resolution.{h,cpp}`, `logging_use_case_test.cpp` and associated tests | Switch extraction, tagged resolution, complete-batch integrity |
| Modify `src/backend/logging/protocols/portable_ssm_logging_protocol.{h,cpp}`, protocol tests, and evidence-gated MUT codec/driver files | Consume validated physical plans and reject incomplete replies |
| Modify `src/platform/desktop/common/logging/logging_snapshot_adapter.{h,cpp}`, `logging_value_adapter.{h,cpp}`, adapter/runtime tests | Bind new run data and maintain fresh parameter/switch display caches |
| Modify `src/platform/desktop/common/transport/desktop_logging_protocol_registration.cpp` and its tests | Pass validated read plans into existing factories |
| Modify `src/ui/desktop/widgets/mainwindow.{h,cpp}`, `menu_actions.cpp`, `logvalues.cpp`, `logbox.{h,cpp}`, `mainwindow_test.cpp` | Captured display bindings, pending edits, unavailable choices, switch indicators |
| Create `src/backend/logging/logging_csv_record.{h,cpp}` and `_test.cpp` | Portable CSV field quoting and trailing-comma record serialization |
| Create `src/platform/desktop/common/logging/logging_csv_file.{h,cpp}` and `_test.cpp`; modify `src/ui/desktop/widgets/log_operations_ssm.cpp` | Frozen columns and exclusive per-run CSV lifecycle |
| Regenerate affected BUILD files | Package-owned libraries/tests and precise dependencies |
| Modify domain language, logging contracts/design notes/debt, XML README, checklists, confirmed spec/plan | Enduring contracts, practical flow, qualification, history preservation |

### Task 1: Establish the Physical Wire Contract

**Files:** Create the wire-evidence reference; inspect the existing SSM/MUT codec/driver/protocol suites, and modify the logging contract/debt references. Production codec changes require the resulting scoped contract.

**Interfaces:** Produces the source-pinned read contract consumed by Tasks 3–4: physical entry meaning, byte order, response shape, capacity, and the scope of each established limit. No ECU support discovery API is produced.

- [ ] Pin the SSM primary sources from the logging contract: three-byte requested addresses, one returned byte per address, base-plus-length expansion, and explicit ordered address lists. Record a literal example requesting `0x000008` and `0x00001c` and receiving `0x7d,0xb1`; their positions are 0 and 1, independently of display slots.
- [ ] Trace MUT free-form setup, descriptor placement, RX-buffer ingestion, request-word reconstruction, pointer-table/RAM resolution, and stream width handling in the parent OEM evidence. Record the 33520003 fill-loop checks at `0x11600`, request reconstruction at `0x1164c`–`0x11660`, and RX-buffer pointer assignment at `0x11004`. Contrast a non-symmetric request such as `0x1234` against the maintained encoder; distinguish request byte order from streamed-value byte order.
- [ ] Record that the host's count field represents up to 255 entries, while the evidenced firmware bounds element index and output bytes at 96. Trace width-specific boundary checks and request-buffer capacity before deriving usable packet limits. Do not label either number a universal device capability.
- [ ] Inspect short-reply handling: current MUT decoding can synthesize zeros when payload bytes are missing, and SSM can publish partial values. Define malformed/truncated reply handling as `BadResponse` with no fabricated values or samples from that poll.
- [ ] Write the evidence owner with exact sources, literal packet expectations, supported dialect scope, and unresolved hardware checks. Link it from the existing contract and debt owner instead of copying the trace inventory.
- [ ] Run `prek run lychee --all-files` and review the scoped evidence diff. Expected: PASS and every claimed wire property traceable to primary evidence. Commit named documentation paths: `docs: characterize logging wire layout and capacity`.
- [ ] Gate affected MUT request changes on the evidence result. A scoped correction must have independent golden request/response bytes and boundary cases, RED→GREEN tests, and a mutation check restoring the old defect. If dialect routing is needed, obtain its focused design approval; never conceal the unresolved prerequisite by retaining a knowingly mismatched fixture and claiming correctness.

### Task 2: Normalize Definitions and Separate ECU Support

**Files:** Definition model/parser/model sources and tests, backend BUILD; capability-update call sites in `mainwindow.cpp`, `log_operations_ssm.cpp`, and their tests when needed.

**Produces** (namespace `fastecu::logging`):

```cpp
enum class EcuSupport { Unknown, Supported, Unsupported };
struct LoggerAddressSpec {
    std::string value;
    std::optional<std::string> length;
    std::optional<std::string> bit;
};
// Add address_specs to LoggerParameter, retaining legacy address/length fields.
// Add sample_bit to LoggerSwitch; ecu_bit remains capability metadata.
EcuSupport LoggerModel::parameter_support(std::string_view protocol, std::string_view id) const;
EcuSupport LoggerModel::switch_support(std::string_view protocol, std::string_view id) const;
void LoggerModel::set_parameter_support(std::string_view protocol, std::string_view id, EcuSupport state);
void LoggerModel::set_switch_support(std::string_view protocol, std::string_view id, EcuSupport state);
void LoggerModel::reset_support(std::string_view protocol);
bool LoggerModel::parameter_available(std::string_view protocol, std::string_view id) const;
bool LoggerModel::switch_available(std::string_view protocol, std::string_view id) const;
```

Availability is chooser/default eligibility: supported is available; unsupported is unavailable; unknown uses XML enablement. Explicit selection with unknown support is not silently removed by preparation. Keep compatibility methods only while migrating callers, then remove the conflated support/availability methods.

- [ ] Add parser fixtures for legacy parameter-level `length="2"`, standard `<address length="2">`, and explicit SSM address forms found in Task 1. Preserve the raw address metadata needed to detect conflicting lengths rather than discarding it or validating every unselected channel prematurely. Test a standard switch with capability `ecubyteindex/bit` and nested sample `<address bit="5">`, plus legacy `byte/bit` with an explicit independent capability bit. Assert sample bit 5 can differ from capability bit 1.
- [ ] Add model tests: XML `enabled="1"` yields support `Unknown` and available true; XML-disabled unknown yields available false; known-supported overrides the default; known-unsupported is unavailable even when XML-enabled. A valid capability bit set/clear yields Supported/Unsupported; missing metadata or unavailable capability bytes yields Unknown. Test separate parameter/switch identities sharing ID `rpm`.
- [ ] Run `bazel test --config=release //src/backend/logging:logger_definition_parser_test //src/backend/logging:logger_model_test`. Expected RED for the new metadata/state APIs.
- [ ] Implement metadata preservation and normalization. For standard nested switches, use the nested bit for samples and top-level capability metadata for support; for legacy byte/bit input, do not infer capability from the sample bit. Parse optional switch enablement as a default, preserving immutable definitions. Conflicting explicit metadata must become a contextual validation error when that selected measurement is prepared.
- [ ] Replace boolean support storage with EcuSupport, seeded Unknown. Reset evidence when a new connection/target is identified before applying its capabilities; an identification without capability bytes cannot carry old support into the new target. Defaults select available entries in definition order at the existing 15/12/20 limits. Saved IDs remain unchanged for later validation.
- [ ] Regenerate BUILD files and run the two backend suites plus `//src/ui/desktop/widgets:test_mainwindow` for capability/identification regressions. Expected: PASS. Commit named paths: `refactor: distinguish definition defaults from ECU support`.

### Task 3: Prepare Logical Measurements and Physical SSM Reads

**Files:** New read-plan files/tests; logging types, preparation/session/snapshot sources/tests; backend BUILD.

**Consumes:** Task 2 support and raw definition metadata; Task 1 wire contract.

**Produces** (namespace `fastecu::logging`):

```cpp
enum class LoggingMeasurementKind { Parameter, Switch };
struct LoggingMeasurement {
    LoggingMeasurementKind kind;
    LoggerIdentity identity;
    std::string channel_id; // internal transport/sample identity
    std::string name;
    std::string unit;
    std::uint8_t decimal_precision;
    EcuSupport support;
};
struct SsmReadPlan {
    std::vector<std::uint32_t> addresses;
    std::vector<std::vector<std::size_t>> response_positions; // one list/channel
};
fastecu::Result<SsmReadPlan> make_ssm_read_plan(std::span<const LoggingChannel> channels);
// Extend LoggingChannel with normalized SSM byte_addresses and optional sample_bit.
// LoggingSession owns its validated SSM plan, absent for other protocols.
const std::optional<SsmReadPlan>& LoggingSession::ssm_read_plan() const;
const std::vector<LoggingMeasurement>& LoggingRunSnapshot::measurements() const;
const LoggingMeasurement* LoggingRunSnapshot::find_measurement(
    LoggingMeasurementKind kind, std::string_view id) const;
```

Keep the existing `prepare_logging_run(model, protocol, filter, policy, target)` signature. This task adds read-plan validation and owned measurement metadata for the existing acquisition path. Task 4 changes the public factory to the ordered gauge/Digital/switch union, updates its tests, and removes obsolete offset/support masks together with consumers. Do not make the old protocol consume union channels before its response mapping is migrated.

- [ ] Add pure read-plan tests with literal expectations: two-byte A at `0x10` and one-byte B at `0x20` produce addresses `{0x10,0x11,0x20}` and positions `{{0,1},{2}}`; an explicit source order `{0x11,0x10}` retains positions `{{0,1}}` for that order. Reject overflow beyond `0xffffff`, missing bytes, conflicting lengths, and an out-of-range switch sample bit.
- [ ] Test SSM capacity by physical requested bytes: 21 distinct four-byte sources fit 84 entries; a 22nd source at a disjoint address fails. Repeated displays of one logical source do not consume additional entries. Only share physical sources across distinct definitions where Task 1 establishes protocol equivalence; do not merge speculative overlapping MUT/CDBG reads.
- [ ] Add snapshot metadata/ownership tests for an existing Digital measurement: captured name, unit, precision, support state, and identity survive destruction of the source model. Keep current factory behavior until Task 4; its union/strict-selection regressions belong to that migration.
- [ ] Run `bazel test --config=release //src/backend/logging:logging_read_plan_test //src/backend/logging:logging_run_snapshot_test`. Expected RED before the read-plan and captured metadata APIs exist.
- [ ] Implement normalized byte sources and validated read plans. Store physical response positions independently of display slots. Use stable internal IDs tagged by kind, such as `parameter:<id>` and `switch:<id>`; never expose those internal tags as user IDs or infer kind by parsing arbitrary user text later.
- [ ] Capture owned LoggingMeasurement metadata for each prepared channel and expose the snapshot accessors. Preserve the original selection lists for later ordered display/CSV binding; Task 4 introduces union deduplication and strict selection validation.
- [ ] Reuse channel/session validation for numeric bounds, expressions, precision, and aggregate physical layout. SSM counts requested addresses; MUT/CDBG retain their evidence-scoped width/capacity rules. The prepared object has private owned state and const accessors, and survives source-model destruction.
- [ ] Regenerate BUILD files and run `bazel test --config=release //src/backend/logging/...`. Expected: PASS for the prepared APIs. Commit named paths: `feat: prepare physical SSM read plans and owned measurement metadata`.

### Task 4: Bind Protocols, Convert Switches, and Resolve Complete Batches

**Files:** Snapshot factory/call sites, session/conversion/resolution sources/tests, all logging protocol suites, MUT codec/driver files authorized by Task 1, desktop adapters/runtime/registration tests, relevant BUILD files.

**Consumes:** Task 3 immutable measurements and validated SSM plan.

**Produces:** Existing LogSample/ProtocolSample pipeline with internal channel identities; extend `ResolvedLogSample` with `LoggingMeasurementKind kind`, retaining its user LoggerIdentity, numeric value, and precision. Add `bool DesktopLoggerValues::set_switch_value(const LoggerIdentity&, QString)` and a run-cache initializer `void DesktopLoggerValues::begin_run(const LoggingRunSnapshot&)` that starts selected values unavailable until sampled.

```cpp
SsmLoggingProtocol(IClock& clock, std::unique_ptr<ISsmTransport> transport,
                   std::vector<LoggingChannel> channels, SsmReadPlan plan,
                   bool target_is_ecu, bool use_openport2_adapter);
// Remove Digital-slot/sequential-offset constructors after migrating every caller.
```

- [ ] Add snapshot union tests for gauge-only and switch-only acquisition, a parameter repeated in two Digital slots and a gauge, and a parameter/switch sharing `same-id`. Assert one acquisition per `(kind, protocol, ID)`; repeated display positions succeed and duplicate definitions fail. Valid explicitly selected unknown-support MUT input succeeds; known-unsupported selections and unresolved IDs fail with contextual errors before any factory starts.
- [ ] Add scripted SSM regressions: the request for a two-byte value contains both byte addresses; reply bytes `{1,2}` become raw string `"12"`; a subsequent source maps to its own response position. Reject missing/truncated data without publishing any sample from that poll. Use literal independent request/checksum bytes and replace fixtures that return two data bytes for one requested address.
- [ ] Add switch conversion tests: raw byte 128 with sample bit 7 becomes numeric 1 and CSV/cache text `"1"`; raw byte 0 becomes 0; bit 8 fails configuration; parameter and switch sharing an ID update different caches. Parameter expression failures still terminate before any part of the polling batch is published.
- [ ] Add a MUT driver/protocol test containing a checksum-valid short payload for widths `{2,1}`. Expected: no valid zero values, no responded-success sample batch; malformed response is reported as BadResponse under existing retry policy. Include evidence-scoped request byte-order/capacity regressions from Task 1 before making those corrections.
- [ ] Run `bazel test --config=release //src/backend/logging/protocols:test_ssm_logging_protocol //src/backend/logging/protocols:test_mut_dma_logging_protocol //src/backend/protocol:test_driver //src/backend/logging:logging_conversion_test //src/backend/logging:logging_sample_resolution_test`. Expected RED for incorrect byte requests, switch extraction, and truncated replies.
- [ ] Switch the snapshot factory to ordered gauge/Digital/switch union preparation and migrate all production/test callers atomically. Deduplicate by `(kind, protocol, ID)`, capture names/units/precision, reject unresolved/known-unsupported selections, and accept explicitly selected unknown-support MUT sources subject to normal field/protocol validation. Factories consume the session's validated SSM plan. Remove original-Digital offsets and disabled-sample masks: unavailable selected sources fail startup instead of being silently skipped.
- [ ] Build SSM requests from plan addresses and gather every logical raw value from its complete ordered response positions. Validate framing/shape before publishing the poll. Switch channels read one byte; backend conversion extracts `(byte >> sample_bit) & 1` without changing the parameter expression evaluator or its precision.
- [ ] Resolve samples through captured LoggingMeasurement records, returning kind plus user identity. Desktop formatting/cache mutation remains desktop. Begin each run with fresh selected cache entries; a successful current-run sample populates them. Missing current samples never reuse a previous run's cache value.
- [ ] Regenerate BUILD files and run backend logging/protocol suites plus `//src/platform/desktop/common/logging:test_logging_adapters`, `//src/platform/desktop/common/logging/runtime:test_logging_engine`, `//src/platform/desktop/common/logging/runtime:test_logging_worker`, and `//src/platform/desktop/common/transport:test_desktop_logging_protocol_registration`. Expected: PASS. Temporarily restore each corrected bounds/reply/layout defect and verify its named regression fails, then restore the correction and rerun focused suites.
- [ ] Commit named paths: `fix: map logging replies to selected parameters and switches`. Do not clear MUT characterization debt or claim full delivery if its Task 1 gate is still unresolved.

### Task 5: Render Active Bindings and Save Pending Edits

**Files:** `mainwindow.{h,cpp}`, `menu_actions.cpp`, `logvalues.cpp`, `logbox.{h,cpp}`, `mainwindow_test.cpp`; regenerated widget BUILD.

**Consumes:** Captured measurements/selection and fresh parameter/switch caches from Tasks 3–4.

**Interfaces:** Keep existing chooser action signatures. Add private MainWindow helpers `const LoggerSelection& displayed_logging_selection() const` and `void update_logging_pending_state()`; they select the captured active lists while a run is active and the model lists otherwise. Rendering gets names/units from active bindings during a run, not mutable model lookup.

- [ ] Add MainWindow regressions for a known-unsupported saved ID and a missing ID: both remain represented as unavailable, startup shows a protocol/ID-specific plain-text error, no logging factory starts, and the operator can replace them. An unknown-support enabled MUT measurement remains offered and can start from valid XML without ROM matching.
- [ ] Add `SelectionEditsWaitForNextRun`: start on `rpm`, select `temperature`, assert model/persistence changed while active labels, acquisition, and CSV bindings still use `rpm`; a restart-needed indicator appears. Stop/start and assert the new run uses `temperature` and clears the pending indicator.
- [ ] Add gauge-only sample/cache coverage and switch widget coverage for 0/1, using separate sample and capability bits. A repeated measurement in several positions updates all relevant displays. User-defined names/units containing markup-like characters display literally. No new gauge renderer is constructed.
- [ ] Run `bazel test --config=release //src/ui/desktop/widgets:test_mainwindow --test_filter='*Logging*:*logging*:*Selection*:*Switch*'`. Expected RED for pending selection and switch value rendering; use the actual new named suites when filters would miss a test.
- [ ] Update chooser population to availability rather than conflated support. Show current unavailable selections with reasons; permit correction without silently deleting saved IDs. Keep existing immediate selection persistence and Cancel semantics, but call display rebuilding against active bindings when running.
- [ ] Bind Digital and switch widgets to captured slot identities. Give switch value labels stable slot names and render ON/OFF (CSV remains 0/1); show unavailable/pending values rather than a valid-looking zero before a sample. Add the restart-needed text to the existing logging UI/status area with a lifetime owned by MainWindow.
- [ ] Ensure session-end/reset/start-failure paths switch back to pending selection safely. Retain per-sample active-snapshot rechecks because error signals can synchronously end a run, plus existing worker generation fencing and stop/join ordering.
- [ ] Run the full MainWindow and desktop adapter/runtime/registration suites. Expected: PASS. Commit named paths: `feat: freeze active logging displays and show pending selections`.

### Task 6: Freeze CSV Columns and Create a New File for Every Run

**Files:** New portable CSV-record source/test; new desktop CSV-file source/test; `log_operations_ssm.cpp`, MainWindow lifecycle code/tests; generated backend/desktop/UI BUILD files.

**Interfaces:**

```cpp
// backend, namespace fastecu::logging: no Qt, files, or clock
std::string serialize_logging_csv_record(std::span<const std::string_view> fields);
// desktop, namespace fastecu::desktop::logging
// LoggingRunSnapshot below means fastecu::logging::LoggingRunSnapshot.
class LoggingCsvFile {
 public:
  fastecu::Result<QString> begin_run(const LoggingRunSnapshot& snapshot,
                            const QString& directory, const QString& name_stem);
  fastecu::Status append_row(const DesktopLoggerValues& values, std::chrono::milliseconds elapsed);
  fastecu::Status end_run();
  bool is_open() const;
};
```

The file helper owns QFile/QTextStream and captured ordered column descriptors. MainWindow supplies names/time and reports errors; backend serialization quotes fields and preserves trailing commas. Columns retain gauge → Digital → switch order and repeated display columns, while acquisition stays deduplicated.

- [ ] Add CSV-record golden tests: fields `{"Time","RPM","Flag"}` produce `"Time,RPM,Flag,\n"`; commas, quotes, CR/LF, and UTF-8 names round-trip with standard quoting; empty fields retain positions. Numeric fixed formatting and Time's established formatting remain explicit at desktop boundaries.
- [ ] Add desktop file tests in a real QTemporaryDir: two identical runs with the same `name_stem` create different files; the first file's bytes remain unchanged; a pre-existing file is preserved; each header matches captured columns. First valid sample is written after the header, not discarded. Missing current-run values are empty, never old-run cache data.
- [ ] Add MainWindow scenarios: pending edits do not change active headers/rows; Connect stops/closes the old run file; restarting with identical selections creates a new file; handshake failure, cancellation, adapter removal, and normal stop release file ownership; toggling file logging off closes its file. File-open/write failure reports an error and leaves existing outputs intact without stopping display updates or falsely marking the file open.
- [ ] Run `bazel test --config=release //src/backend/logging:logging_csv_record_test //src/platform/desktop/common/logging:logging_csv_file_test //src/ui/desktop/widgets:test_mainwindow`. Expected RED for shared filenames, mutable columns, or old-run append behavior.
- [ ] Implement portable record serialization with correct quoting and trailing commas. Build column descriptors from the captured selection and measurement metadata; never resolve their names or identities through the mutable model at write time.
- [ ] Implement exclusive file creation with `QIODevice::NewOnly | QIODevice::WriteOnly`. Try the supplied stem, then deterministic numeric suffixes on existing-name collisions; report other open errors instead of looping. Failed creation never truncates a file. End-run flush/close is idempotent and propagates write failures through existing Result/Status.
- [ ] Wire begin/append/end into logging lifecycle, including synchronous factory/start failures and worker/session ends. A new run never inherits the previous writer. File logging enabled partway through a run uses that run's captured columns; disabling/re-enabling file logging never appends a different run to an old file.
- [ ] Remove old `datalog_file`/mutable-column paths once all callers use the helper, migrating tests to returned file paths and real file contents. Keep UI dialogs outside the file helper and retain target-level UI-facing adapter visibility.
- [ ] Regenerate BUILD files and run the new suites plus the full MainWindow/desktop logging suites. Expected: PASS. Commit named paths: `feat: create an exclusive CSV file for each logging run`.

### Task 7: Extract Enduring Contracts and Verify the Complete Change

**Files:** Domain glossary, documentation index, logging contracts/design notes/debt, XML README, wire-evidence reference, applicable logging checklist, spec/plan lifecycle.

**Consumes:** Completed Tasks 1–6 and the resolved wire gate.

- [ ] Update owning contracts with support/default eligibility, complete selection acquisition, identity sharing, physical SSM read limits/mapping, sample freshness, active/pending bindings, and one-run-per-file lifetime. Replace the old current-selection CSV and Connect-append descriptions explicitly; link to the evidence owner and actual implementation files.
- [ ] Update the XML README's current-flow instructions for gauge/switch acquisition and imported address/bit forms now supported. Retain app restart after XML edits, stable IDs, native request-code input, first conversion, user trust, and the in-app editor/new gauge-renderer follow-ups. Add a valid switch example using the tested schema and distinct capability/sample metadata.
- [ ] Remove only debt demonstrably resolved by the delivered implementation. Keep unresolved MUT dialect/capacity/hardware evidence and unrelated logging follow-ups. CSV serialization debt can be retired only if all production resolution/serialization paths now use captured portable policy.
- [ ] Run `python3 scripts/gazelle_check.py --fix`, review generated dependencies, and run `bazel build --config=release //:fastecu`, `bazel test --config=release //...`, `prek run --all-files`, and `bazel run //:clang_tidy_report_changed`. Expected: zero failures; report every skipped platform target and blocked gate. Verify current-host packaging and require the applicable Linux/Windows/Android/Sonar PR CI gates. No warning suppression or visibility widening is acceptable.
- [ ] Review against the spec and every Review Focus case. Request one fresh whole-branch review under native execution; fix important findings with named RED→GREEN regressions and a green suite. Prepare a PR description around final behavior, intentional compatibility changes, evidence, and qualification limits.
- [ ] Stage named paths only and preserve unrelated worktrees/reports/proposals. Preserve original spec/plan in separately delivered history before removing completed artifacts; a squash that introduces and deletes them cannot preserve originals. Push/create PRs only within the user's authorization for this follow-up; authorization to publish the earlier two PRs does not silently publish this new work.

## Plan Self-Review and Handoff

- Spec coverage: Task 2 owns support/default distinctions; Task 3 owns physical read plans and metadata; Task 4 owns union, namespaces and switch conversion; Task 5 owns unavailable/pending selections and switch display; Task 6 owns frozen CSV and every-run file lifetime; Task 7 owns documentation/gates/delivery. Task 1 explicitly owns the prerequisite evidence and blocks unsupported MUT wire decisions.
- Interfaces: Task 3's LoggingMeasurement and SsmReadPlan are consumed by Task 4; ResolvedLogSample.kind and begin_run cache initialization feed Task 5; Task 6 consumes the same captured metadata and caches. No task chooses identities from labels or current mutable selection.
- No form authoring, new gauge rendering, live worker reconfiguration, automatic MUT support probing, speculative cross-definition read merging, or hardware qualification is included.

This plan has not been executed. Writing it requires Markdown link checks and a scoped diff review; application tests above are implementation steps.
