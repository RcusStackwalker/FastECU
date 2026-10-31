# Denso ISO-15765 family deviations

Where each flash family in the
[Denso ISO-15765 bootloader dialect](../../src/backend/flash/GLOSSARY.md)
departs from its siblings. The [sharing contract](flash-contracts.md#denso-iso-15765-bootloader-dialect)
says what the dialect shares. This page lists what each family keeps for itself,
so the four can be compared side by side. Each deviation is a field of the
family's [dialect profile](../../src/backend/flash/GLOSSARY.md), the
`DensoIso15765BootloaderProfileOf<Plan>` specialisation in
`src/backend/flash/ecu/subaru_denso_<family>_plan.h`; the order of operations
belongs to `DensoIso15765BootloaderExecutor`.

Every deviation was transcribed from the family's legacy operation source,
`src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_<family>_can_operation.cpp`
as of `20892df`. Legacy line numbers below refer to that file. None of these
routes is bench-qualified, so each deviation has the status *transcribed,
unverified*: it is preserved byte for byte, and nobody has checked whether the
ECU needs it. Collapsing one waits on bench evidence (see
[technical debt](../tech-debt.md#p3-flash-protocol-behaviors-awaiting-external-evidence)).

## N83M 1.5M

| Deviation | Legacy | Profile field | Status |
|---|---|---|---|
| Image base `0x08F9C000`: byte 0 of the plan image is `fblocks[0].start` | 1225 | `image_start` | transcribed, unverified |
| Bench kernel jump reads once before the continuation; legacy's `delay(50)` before the loop precedes a read, so it is subsumed | 770-805 | `bench.discard_first_reply = false` | transcribed, unverified |
| Checksum verify reads 500 ms, and 500 ms again after `7F 31 78` | 1308, 1329 | `checksum_policy` | transcribed, unverified |

## N83M 4M

| Deviation | Legacy | Profile field | Status |
|---|---|---|---|
| Image base `0x08F9C000` | 1241, 1153 | `image_start` | transcribed, unverified |
| Read setup tolerates a wrong **and** an absent reply: four `return STATUS_ERROR` lines are commented out. The siblings abort | 876, 883, 917, 924 | `read_setup_reply = kTolerantReply` | transcribed, unverified |
| The three tolerant connect probes also have their `return STATUS_ERROR` commented out. The siblings have no return there either, so this is a record that the check was removed, not a behaviour difference | 305, 335, 369 | none (shared `DensoProgrammingBranchIsInCar`, `DensoIso15765ConnectInCar`) | no wire difference |
| Bench kernel jump discards a matching first reply, waits 50 ms and reads again | 788-791 | `bench.discard_first_reply = true` | transcribed, unverified |
| Checksum verify reads 500 ms, then 3000 ms (`serial_read_extra_long_timeout`) after `7F 31 78` | 1322, 1341 | `checksum_policy` | transcribed, unverified |

## SH72531

| Deviation | Legacy | Profile field | Status |
|---|---|---|---|
| Image base 0 | 1140, 1225 | `image_start` | transcribed, unverified |
| In-car run asks 0x7E1 for session `0x03` where the N83M run asks for `0x63` | 409-419 | `in_car_run = kSh` | transcribed, unverified |
| Bench kernel jump discards a matching first reply, waits 50 ms and reads again (same as N83M 4M) | 782-785 | `bench.discard_first_reply = true` | transcribed, unverified |
| Read setup reads at 2000 ms (`serial_read_timeout`); N83M and diesel read it at 200 ms | 856, 893 | `read_setup_policy` | transcribed, unverified |
| Read pages paced by 1 ms | 1007 | `pace_pages = true` | transcribed, unverified |
| Checksum verify reads 2000 ms both times | 1310, 1330 | `checksum_policy` | transcribed, unverified |

## SH72543 diesel

| Deviation | Legacy | Profile field | Status |
|---|---|---|---|
| Image base 0; legacy offsets the whole-ROM buffer by `fblocks[0].start` = `0x8000` and indexes by block counter | 1129, 1217 | `image_start` | transcribed, unverified |
| ECU ID read by DID `F1 82` instead of vendor SID `AA` | 131-156 | `identity_query = kDidF182` | transcribed, unverified |
| All four tolerant probes read at 2000 ms (`serial_read_timeout`); the siblings use 200 ms | 297, 324, 357, 663 | `probe_timeout`, `bench.session_timeout` | transcribed, unverified |
| Bench diagnostic session is a tolerant probe, with one commented-out `return STATUS_ERROR`. The siblings abort on a wrong `50 43` | 655-681, 672 | `bench.session_reply = kTolerantProbe` | transcribed, unverified |
| In-car run is SH72531's, but the 0x7E0 `10 63` reply is read at 2000 ms and the other nine at 200 ms | 375-483, 394 | `in_car_timeout` | transcribed, unverified |
| Kernel jump reads once at 2000 ms before the loop; the loop reads at 2000 ms in car and 200 ms on the bench | 629, 643, 770, 785 | `in_car_timeout`, `bench.jump_first_read`, `bench.jump_loop_read` | transcribed, unverified |
| Bench arm waits 50 ms before its first write; the siblings' bench arms wait 500 ms | 653 | `bench.settle` | transcribed, unverified |
| Read pages paced by 1 ms | 1001 | `pace_pages = true` | transcribed, unverified |
| Checksum verify reads 2000 ms both times | 1328, 1348 | `checksum_policy` | transcribed, unverified |
