# OpenPort ISO15765 Flow-Control Configuration

## Goal

Reduce ISO15765 transfer delays on OpenPort J2534 connections while retaining a conservative compatibility fallback for adapters or ECUs that reject the fastest settings.

## Design

During ISO15765 channel initialization, FastECU will include three parameters in the existing `SET_CONFIG` request:

- `LOOPBACK = 0`
- `ISO15765_STMIN = 0`
- `ISO15765_BS = 0`

`STMIN = 0` requests no added separation delay. `BS = 0` permits the sender to transmit all consecutive frames without waiting for another flow-control frame.

If that request fails, FastECU will retry once with:

- `LOOPBACK = 0`
- `ISO15765_STMIN = 1`
- `ISO15765_BS = 16`

If the retry also fails, initialization will report the J2534 error and fail. Raw CAN initialization will retain its existing loopback-only configuration and will not receive ISO15765 parameters.

## Scope

The change belongs in `SerialPortActionsDirect::set_j2534_can_timings()`, after the ISO15765 channel has connected and before message filters are installed. It applies to both native Windows J2534 DLL calls and the existing 32-bit bridge because both consume the same `SCONFIG_LIST` interface.

No user-facing preference or adapter-specific detection is added.

## Testing

Focused tests will exercise the real timing-configuration logic through a controllable J2534 boundary and verify:

1. ISO15765 first sends `LOOPBACK=0`, `ISO15765_STMIN=0`, and `ISO15765_BS=0`.
2. A failed first request retries with `LOOPBACK=0`, `ISO15765_STMIN=1`, and `ISO15765_BS=16`.
3. Two failed requests produce `STATUS_ERROR`.
4. Raw CAN sends only `LOOPBACK=0` and does not retry with ISO15765 parameters.
