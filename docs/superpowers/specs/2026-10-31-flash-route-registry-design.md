# Flash route registry: design

Date: 2026-10-31. Terms (**flash family**, **flash route**, **plan admission**)
are defined in the [flash glossary](../../../src/backend/flash/GLOSSARY.md).
Governing decisions: [ADR 0019](../../adr/0019-compile-the-protocol-catalog-into-the-backend.md)
(catalog and families are separate sources joined by agreement tests),
[ADR 0017](../../adr/0017-generate-bazel-targets-with-gazelle.md) (Gazelle owns
BUILD targets) and [ADR 0024](../../adr/0024-wire-transcripts-and-goldens-are-the-test-contract.md)
(transcripts and goldens are the wire contract).

## Intent

Registering a flash family has no locality. A new or ported family today
needs about twelve edits across seven or more files, and the protocol ids it
accepts are written in up to three places. This design concentrates each
family's registration facts in the family's own module, and puts its flash
routes in one place in `workflow/`. The facts that are derived get one
table-driven test over the real catalog.

## Starting point (master `268b76cb`)

Adding a family touches:

| Where | What |
| --- | --- |
| `flash/flash_types.h` | the include, the `FlashFamily` value, its `kFlashFamilyNames` row, the `FamilyPlan` alternative, a `FamilyTraits` specialization, and (for 21 of 30 plan types) a `kFamilyRequiresKernel<…> = false` specialization |
| `ecu/<f>_plan.h` | the builder declaration and a forwarding `PlanValidator` specialization |
| `workflow/flash_workflow.cpp` | includes, a `using …Workflow` alias, a `CreateWorkflow` constant, and one or more `kRoutes` rows |
| BUILD files | `flash/`, `ecu/`, and `workflow/` |
| `workflow/flash_workflow_test.cpp` | the hand-written list in `recognizesEveryPortableFamilyPrefixAndLeavesLegacyAlone`, and the family's "routes only the N exact protocols" test |

The protocol ids a family accepts appear in `kRoutes` (prefix or exact, with
comments that depend on row order), in the family's own builder (a
`kProtocols` array in the 11 single-window plans, or the `.protocol` field of
a `FindCatalogEntry` table), and in the catalog. ADR 0019 sanctions only the
last pair.

The units involved are not one to one:

- `kSubaruHitachiSh7058` is one family with two plan types (K-Line reads, CAN
  writes) behind one workflow-creation function that picks by operation.
- The two Denso SH705x EEPROM families share one `EepromWorkflow`.
- The two Unisia JECS M32R boot-mode families share one boot-mode workflow.

The catalog lists 61 protocols. 60 read, test-write or write, and every one of
them routes today. The 61st, `mitsu_ecu_m32r_kline_mut_dma`, is logging only.
Each prefix row in `kRoutes` covers between one and four of those names.

## Decisions

1. **One flash route per workflow.** A route is a row in a registry in
   `workflow/`: the exact protocol ids that select one workflow, and how that
   workflow is created. The registry stays in `workflow/`, because `workflow/`
   depends on `ecu/` and the workflow templates are staging policy
   ([design notes](../../design-notes.md)). SH7058, EEPROM and boot-mode are
   one row each.
2. **Exact matching only.** No protocol id is matched by prefix. The
   order-sensitive comments in `kRoutes` go away. A name the catalog does not
   contain no longer routes (see Behaviour changes).
3. **The family module owns its protocol ids.** Each family exports one
   `constexpr` list of the protocol ids it accepts; the registry row refers to
   it, and the family's builder checks against it. Where a family already has
   a per-protocol table (`FindCatalogEntry` rows, `kVariants`), the list is
   projected from that table, so it is not written a second time. The catalog
   stays a separate source, joined by agreement tests (ADR 0019).
4. **The facts about a plan type sit next to it.** Its `FamilyTraits`
   specialization (family id, transport), its validator binding, and its
   kernel requirement move from `flash_types.h` and `<f>_plan.h` into the
   family's `<f>_types.h`, as one block. The validator is declared there
   against a forward-declared `FlashPlan`.
5. **The kernel requirement defaults to `false`.** The 9 plan types that upload
   a kernel opt in: the two Denso SH705x EEPROM plans, MC68HC16Y5 02, SH7055
   02, SH7058 CAN and SH7058 CAN diesel, SH705x DensoCAN, SH705x K-Line, and
   the Denso SH705x TCU CAN plan. Today the default is `true` and 21 plan types
   override it.
6. **`<f>_plan.h` folds into `<f>_types.h`.** After decision 4, each `_plan.h`
   holds only a builder declaration, and `Result<FlashPlan>` can be named in a
   declaration while `FlashPlan` is incomplete. Each family keeps one header,
   its `plan.cpp` and its executor. Gazelle regenerates the targets.
7. **`flash_types.h` keeps four entries per family.** These are the include,
   the enum value, the name row and the `FamilyPlan` alternative. All four are
   adjacent and checked at compile time; the name table already has a
   `consteval` order check. Generating the enum from the descriptors is not
   worth it in C++.

## Tests

One table-driven routing test over the real built-in catalog replaces the
hand-written route list and the per-family "routes only" tests. A flash
protocol is a catalog entry with `read`, `test_write` or `write` set. Four
things are checked:

- every flash protocol in the catalog has exactly one flash route;
- every protocol id in the registry is a flash protocol in the catalog;
- each row's builder accepts each of its protocol ids, and the row creates its
  workflow;
- each builder accepts exactly the operations its catalog entry flags.

The last check already exists:
`BuiltinCatalogCapability.EveryOfferedOperationIsAcceptedAndEveryOtherIsUnsupported`
drives every catalog entry with the bundled kernels and ROM-sized images, and
finds no mismatch. It stays the owner of that check. A mismatch fails CI; each
fix gets its own PR, because changing a flag changes what the UI offers.

The EEPROM builder keeps its own name rules and does not check the EEPROM
route's protocol list, because its tests use names outside the catalog.

Tests that prove wire behaviour or plan goldens are unchanged. Family suites
that prove a builder rejects a specific lookalike name keep that test when the
rejection is the builder's, not the route's.

## Behaviour changes

- A protocol name outside the catalog that matched a prefix row used to route
  and then fail preflight with `kInvalidConfig`. Under exact matching it no
  longer routes, and the desktop shows "Unknown flashmethod" instead. The
  flash workflow factory's one production caller,
  `FlashOperationController`, passes a `ProtocolSpec` from the catalog, so
  production cannot reach this path. `invalidColtSuffixIsRecognizedButFailsPreflight`
  is restated as a no-route case.
- No wire behaviour, prompt order or plan content changes.

## Delivery

Three PRs, each on its own feature branch, each green on the full CI:

1. **Exact routes.** Family modules export their protocol lists; the registry
   in `workflow/` uses exact matching; the routing test lands; the per-family
   route tests and the hand-written list go. Decisions 1–3.
2. **Plan-type facts.** `FamilyTraits`, validator binding and kernel
   requirement move into each `<f>_types.h`; the default flips to `false`.
   Decisions 4–5.
3. **One header per family.** `<f>_plan.h` folds into `<f>_types.h`; Gazelle
   regenerates targets. Decision 6.

PR 1 changes behaviour and adds the guarding test; PRs 2 and 3 are refactors
underneath it.

## Non-goals

- Moving per-family plan preparation (`PrepareMc68` and its placeholder kernel,
  `PrepareBdm`, the boot-mode `BuildPlans`) out of `flash_workflow.cpp`. The
  family module is where it would go later, but it changes the order of
  identity validation and file reads and needs its own tests.
- Stating each single-window family's wire identity once.
- Removing the `flash_types.h` include fan-out, which needs a type-erased
  `FamilyPlan`.
- Changing the catalog's shape, or letting catalog entries name a family.

## Done when

Adding a family touches its own module, one registry row with its workflow
alias, and the four `flash_types.h` entries. A protocol the catalog can flash
with no route, or a route to a protocol the catalog lacks, fails a test.
