# Denso ISO-15765 family deviations

Where each flash family in the
[Denso ISO-15765 bootloader dialect](../../src/backend/flash/GLOSSARY.md)
departs from its siblings. The [sharing contract](flash-contracts.md#denso-iso-15765-bootloader-dialect)
says what the dialect shares. This page lists what each family keeps for itself,
so the four can be compared side by side.

Every deviation was transcribed from the family's legacy operation source,
`src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_<family>_can_operation.cpp`
as of `20892df`. Legacy line numbers below refer to that file. None of these
routes is bench-qualified, so each deviation has the status *transcribed,
unverified*: it is preserved byte for byte, and nobody has checked whether the
ECU needs it. Collapsing one waits on bench evidence (see
[technical debt](../tech-debt.md#p3-flash-protocol-behaviors-awaiting-external-evidence)).

## N83M 1.5M

| Deviation | Legacy | Executor | Status |
|---|---|---|---|
| Image base `0x08F9C000`: byte 0 of the plan image is `fblocks[0].start` | 1225 | `kImageStart` | transcribed, unverified |
| Bench kernel jump reads once before the continuation; legacy's `delay(50)` before the loop precedes a read, so it is subsumed | 770-805 | `DensoIso15765JumpToKernel(…, false)` | transcribed, unverified |
| Checksum verify reads 500 ms, and 500 ms again after `7F 31 78` | 1308, 1329 | `kChecksumPolicy` | transcribed, unverified |

## N83M 4M

| Deviation | Legacy | Executor | Status |
|---|---|---|---|
| Image base `0x08F9C000` | 1241, 1153 | `kImageStart` | transcribed, unverified |
| Read setup tolerates a wrong **and** an absent reply: four `return STATUS_ERROR` lines are commented out. The siblings abort | 876, 883, 917, 924 | `TolerantSetup` | transcribed, unverified |
| The three tolerant connect probes also have their `return STATUS_ERROR` commented out. The siblings have no return there either, so this is a record that the check was removed, not a behaviour difference | 305, 335, 369 | `DensoProgrammingBranchIsInCar`, `ConnectInCar` | no wire difference |
| Bench kernel jump discards a matching first reply, waits 50 ms and reads again | 788-791 | `DensoIso15765JumpToKernel(…, true)` | transcribed, unverified |
| Checksum verify reads 500 ms, then 3000 ms (`serial_read_extra_long_timeout`) after `7F 31 78` | 1322, 1341 | `kChecksumPolicy` | transcribed, unverified |

## SH72531

| Deviation | Legacy | Executor | Status |
|---|---|---|---|
| Image base 0 | 1140, 1225 | `kImageStart` | transcribed, unverified |
| In-car run of its own: `10 C0` to 0x7A2, and session `0x03` where the N83M run sends `0x63` | 373-492 | `ConnectInCar` | transcribed, unverified |
| Bench kernel jump discards a matching first reply, waits 50 ms and reads again (same as N83M 4M) | 782-785 | `DensoIso15765JumpToKernel(…, true)` | transcribed, unverified |
| Read setup reads at 2000 ms (`serial_read_timeout`); N83M and diesel read it at 200 ms | 856, 893 | `ReadMemory` | transcribed, unverified |
| Read pages paced by 1 ms | 1007 | `DensoIso15765ReadDump(…, pace_pages)` | transcribed, unverified |
| Checksum verify reads 2000 ms both times | 1310, 1330 | `kChecksumPolicy` | transcribed, unverified |

## SH72543 diesel

| Deviation | Legacy | Executor | Status |
|---|---|---|---|
| Image base 0; legacy offsets the whole-ROM buffer by `fblocks[0].start` = `0x8000` and indexes by block counter | 1129, 1217 | `kImageStart` | transcribed, unverified |
| ECU ID read by DID `F1 82` instead of vendor SID `AA` | 131-156 | `NonFatalDidQuery` | transcribed, unverified |
| All four tolerant probes read at 2000 ms (`serial_read_timeout`); the siblings use 200 ms | 297, 324, 357, 663 | `kProbeTimeout` | transcribed, unverified |
| Bench diagnostic session is a tolerant probe, with one commented-out `return STATUS_ERROR`. The siblings abort on a wrong `50 43` | 655-681, 672 | `ConnectBench` | transcribed, unverified |
| In-car run of its own: like SH72531's, but the 0x7E0 `10 63` reply is read at 2000 ms and the other nine at 200 ms | 375-483, 394 | `ConnectInCar` | transcribed, unverified |
| Kernel jump reads once at 2000 ms before the loop; the loop reads at 2000 ms in car and 200 ms on the bench | 629, 643, 770, 785 | `DensoIso15765JumpToKernel` timeouts | transcribed, unverified |
| Bench arm waits 50 ms before its first write; the siblings' bench arms wait 500 ms | 653 | `ConnectBench` | transcribed, unverified |
| Read pages paced by 1 ms | 1001 | `DensoIso15765ReadDump(…, pace_pages)` | transcribed, unverified |
| Checksum verify reads 2000 ms both times | 1328, 1348 | `kChecksumPolicy` | transcribed, unverified |
