# Checksum correction dialog -- bench notes

`docs/logging-engine-bench-checklist.md` covers the LoggingEngine/Worker/Protocol
refactor and isn't the right home for a checksum-correction UI change, so this
note lives on its own.

## Checksum correction dialog — INTENTIONAL CHANGE (step 4, 2026-07-20)

Before: one QMessageBox per corrected checksum family, each titled with that
family's name. A multi-family correction stacked several dialogs.

After: one dialog titled "Checksum Correction", listing every corrected family.

A tester seeing one dialog where they previously saw three is observing the
intended change, not a regression. Verify the listed family names match the
families the ROM actually required correcting.

Disabled-checksum and error outcomes (ROM has all checksums disabled / ROM too
small / malformed checksum table) are unchanged: they still raise their own
immediate dialog, separate from the aggregated "Checksum Correction" summary,
since they need the user's attention on their own rather than being folded
into a list of successful corrections.
