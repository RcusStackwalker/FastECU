# Diagnostic tools -- bench verification checklist

Step 6g moved the BIU, DataTerminal, and DTC dialogs off `SerialPortActions`
onto the `IDiagnosticLink` port, with the OBD-II DTC session now running as
a portable, cancellable `DtcWorker` off the UI thread. No wire framing
changed: `IDiagnosticLink::open()` applies the same setters in one canonical
order that the dialogs applied individually, five-baud and fast init use
the same bytes, and the OpenPort five-baud ASCII comparison and the K-Line
unframing heuristics are pinned exactly as they were (see the
[design notes](design-notes.md#diagnostic-tools)). Automated tests
(`serial_diagnostic_link_test.cpp`, `dtc_session_test.cpp`,
`obd_frames_test.cpp`, `dtc_worker_test.cpp`) are regression evidence, not
hardware qualification.

No row below is qualified until it is run on a bench and signed off. This
checklist does not affect the [flash qualification matrix](flash-qualification-matrix.md);
none of step 6g's changes touch a flash path.

Run `bazel test --config=release //...` first.

## Prerequisites

- A bench ECU reachable over OpenPort 2.0 and, separately, over a direct
  K-Line cable, for the protocols each supports.
- A bench ECU or CAN harness reachable over iso15765.
- Build revision, OS, and adapter/driver version recorded in the run record
  below before starting.

## Run record

| Field | Value |
| --- | --- |
| Build revision | Not yet tested |
| OS and version | Not yet tested |
| Adapter and driver version | Not yet tested |
| ECU/TCU and protocol | Not yet tested |
| Operator and date | Not yet tested |
| Observed result and trace location | Not yet tested |

## Checks

| # | Check | Expected | Result |
| --- | --- | --- | --- |
| 1 | DTC read, iso9141, OpenPort 2.0 | Five-baud init succeeds; vehicle info lists supported-PID pages, including `0x81`-`0xA0` when the ECU supports them; stored and pending DTCs are logged | Not yet tested |
| 2 | DTC read, iso9141, direct K-Line cable | Same as #1, via the OBD-framed (`read_obd`) path | Not yet tested |
| 3 | DTC read, iso14230, fast init succeeds | Fast init accepted (`83 F1 10 C1 E9 8F`); vehicle info and DTC read proceed as in #1 | Not yet tested |
| 4 | DTC read, iso14230, fast init rejected | Falls back to five-baud init, which succeeds; vehicle info and DTC read proceed as in #1 | Not yet tested |
| 5 | DTC clear, each protocol that read succeeded on in #1-#4 | Log shows "Diagnostic trouble codes succesfully cleared!" | Not yet tested |
| 6 | DTC read, iso15765, OpenPort 2.0 | CAN init accepted (`0x41` at response index 4); vehicle info and DTC read proceed as in #1 | Not yet tested |
| 7 | Close the DTC dialog during vehicle info | Dialog closes promptly (worker is stopped and joined, not left running); the next DTC run on the same connection works | Not yet tested |
| 8 | BIU connect and keep-alive | Connects and exchanges keep-alive messages for at least 30 s with no dropped connection | Not yet tested |
| 9 | DataTerminal: SSM K-Line send, CAN iso15765 send, ordering with DTC | K-Line SSM send gets a response; CAN iso15765 send gets a response; after running DTC first, DataTerminal still sends with no header (its `open()` resets and sets every flag, so it does not inherit flags DTC left behind) | Not yet tested |

## Sign-off

- [ ] Date: ________________
- [ ] Adapter / OS: ________________
- [ ] Operator initials: ________________
