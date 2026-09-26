# Logging composition — bench verification checklist

Step 6f changes ownership of logging factories and captures ECU/TCU selection
in each session snapshot. It preserves protocol bytes, CDBG setup order,
MUT/DMA's 125000 baud initialization, and existing worker/serial threading.
Automated tests are regression evidence, not hardware qualification.

All scenarios below are **unrun**. Record each applicable adapter/OS/protocol
combination separately; record unavailable hardware or remote peers as unverified.
Run the host test suite (`bazel test -k --config=release //...`) first.
Windows/macOS/Linux CI and Windows/macOS packaging remain merge gates.

## Run record

| Field | Value |
| --- | --- |
| Build revision | Not yet tested |
| OS and version | Not yet tested |
| Adapter and driver version | Not yet tested |
| ECU/TCU and protocol | Not yet tested |
| Direct/remote connection and peer revision | Not yet tested |
| Operator and date | Not yet tested |
| Observed result and trace location | Not yet tested |

## Scenarios

- [ ] Application startup and immediate exit do not open a logging connection
      or hang. Repeat after an application restart.
- [ ] MUT/DMA: connect using the existing workflow, start logging, verify the
      configured channels update, stop cleanly, then start again. Initialization
      remains at 125000 baud with the existing setup/channel-list exchange.
- [ ] CDBG: verify the existing raw-CAN configuration and handshake, streamed
      values, and clean stop/restart. Settings remain ISO 14230 off, its header
      off, raw CAN on, ISO 15765 off, 11-bit IDs, 500000 baud, reply ID `0x631`.
- [ ] CDBG configuration rejection: with a controlled fault-capable adapter or
      driver harness, verify failure stops before later settings and opening;
      the existing start error appears and UI state recovers. If unavailable,
      record this hardware scenario unverified; the seven scripted setter
      failure tests provide automated coverage.
- [ ] CDBG open failure: unavailable/disconnected adapter reports the existing
      start error, restores UI state, and permits retry after reconnection.
- [ ] SSM: select ECU, start and stop; then select TCU and start again. Verify
      the outgoing target changes from `0x10` to `0x18` and each run uses its
      captured selection. Check channels and values against the prior build.
- [ ] Repeat applicable SSM checks with OpenPort and non-OpenPort adapters;
      framing/read behavior remains unchanged. Record unsupported combinations.
- [ ] Repeat applicable protocol start/stop checks through a remote peer.
      Without a peer, record remote operation unverified.
- [ ] Exit or restart with logging active. The worker stops before serial and
      clock services are released; no hang or callback into a destroyed window.

## Qualification status

No hardware scenario is signed off by this change. Attach completed run records
and traces before changing an item's status; retain failed observations too.
