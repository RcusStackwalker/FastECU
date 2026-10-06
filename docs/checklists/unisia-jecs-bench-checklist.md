# Subaru Unisia Jecs M3779x/M3775x bench checklist

## 0. STOP — full-ROM read is not hardware-qualified

Do not treat this family as proven until both M3779x and M3775x hardware have
confirmed 1953-baud even-parity startup, the wake-up/flush timing, raw byte
address rollover, and an exact 65,536-byte dump against a trusted image.

Record adapter make, model, firmware, interface mode, ECU part number, ECU serial
or other unique identifier, MCU variant, date, operator, and result. Keep separate
records for the M3779x and M3775x runs; evidence from one does not qualify the
other.

## 1. Adapter and fixture setup

- Confirm the adapter can produce raw K-Line traffic at 1953 baud with even
  parity; record every adapter setting and any required voltage or pull-up.
- Use a current-limited, logged bench supply and record idle and peak voltage and
  current. Verify the ECU pinout, grounds, ignition feed, and K-Line before power.
- Capture the complete serial trace, application log, and supply log from before
  opening the port until after it closes.

## 2. Startup and timing

- Confirm the wake frame `78 12 34 00` uses the echo-checked write path and that
  the ECU responds after the 500 ms wait.
- Verify the normal 1000 ms read/flush consumes startup traffic without dropping
  the first raw address response.
- Repeat cold and warm starts on each MCU variant and record timing margins and
  every unexpected byte.

## 3. Cancellation and unblock

- Cancel before startup, during the wake wait, and during the address loop. Each
  run must stop promptly, close the transport, leave the ECU responsive after a
  power cycle, and save no apparently complete ROM.
- Disconnect or block the adapter during a raw read, then restore it. Confirm the
  operation keeps its 500 ms retransmission cadence and remains cancellable; it
  must never manufacture a byte or advance to the next address.

## 4. Address boundaries and recovery

- Inspect requests and returned bytes at `00ff`, `0100`, and `ffff`; confirm raw
  two-byte address rollover and exact byte placement in the saved image.
- Inject silence, short replies, echo/noise, and overlapping false sync tuples.
  Confirm parser resynchronization and retransmission after every 100 empty 5 ms
  reads. Because recovery has no attempt limit, cancellation or a transport error
  must terminate a stream that never contains the requested address.
- Verify that retrying an address neither duplicates nor skips a byte and that
  parser synchronization remains correct across read-call boundaries.

## 5. Dump integrity and evidence

- Require the completed saved file to be exactly 65,536 bytes. Record its SHA-256
  hash and compare every byte with a trusted programmer or independently acquired
  image from the same ECU.
- Repeat at least twice per ECU without power cycling and once after a cold power
  cycle; all hashes for an unchanged ECU must match.
- Attach the trace, logs, saved image hash, trusted-image provenance, discrepancies,
  and signed pass/fail result to each variant's separate qualification record.
- Only after both variant records pass every item may the matrix hardware status
  be considered for promotion from `experimental`.
