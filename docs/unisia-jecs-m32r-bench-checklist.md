# Subaru Unisia Jecs M32R K-Line bench checklist

## 0. STOP — not hardware-qualified

Do not treat this family as proven until every section below has passed on
real hardware. Record the adapter (make, model, firmware; OpenPort 2.0 or
not), the VPP supply when the adapter does not provide it, the ECU part
number and ECU ID, the protocol selected, date, operator and result.

## 1. Read

- Read the full ROM with each protocol the bench ECU supports and compare it
  byte-for-byte with a trusted dump of the same ECU.
- Confirm the saved ROM ID is the ECU ID followed by `_`.
- Repeat with the ECU already in read mode (a second read without cycling
  ignition) and confirm the cold init is skipped.
- Pull the K-Line cable mid-read and confirm the operation fails instead of
  returning a short image.

## 2. Write (`_20` / `_30` only, on a bench ECU with a recovery path)

- Without an OpenPort 2.0: confirm the "apply VPP" prompt appears before the
  connection opens, and that declining it stops before any K-Line traffic.
- Capture a trace of one full write. Record the exact bytes the ECU sends
  after the final `AF 69` block, or confirm it sends nothing. The executor
  currently accepts `EF 52` or silence; tighten it to what the trace shows.
- Confirm the `EF 42` (erase started) and `EF 52` (erased) replies arrive
  within the 20- and 40-round poll budgets, and record how long each took.
- Read the ROM back and compare it with the written image.
- Confirm the "remove VPP" notice appears after success, and after a write
  cancelled during programming.
