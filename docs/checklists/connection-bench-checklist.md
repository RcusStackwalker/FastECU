# Connection and identification -- bench verification checklist

`MainWindow`'s connection handling lives in `AdapterConnection`, and SSM ECU
identification is the portable `identify_ssm_ecu`, run off the UI thread by
`SsmIdentifyWorker`. Several wire-level behaviors differ deliberately from the
original code (see the [design notes](../design-notes.md#connection-and-identification)
for the pinned quirks). Malformed-frame handling (short or malformed init
frames fail as `BadResponse`, SSM1's trailing drain stops after 100 reads)
has automated coverage only and no bench row.
Automated tests (`ssm_identify_test.cpp`, `ssm_identify_worker_test.cpp`,
`adapter_connection_test.cpp`, `serial_diagnostic_link_test.cpp`,
`mainwindow_test.cpp`) are regression evidence, not hardware qualification.

No row below is qualified until it is run on a bench and signed off. This
checklist does not affect the [flash qualification matrix](../flash-qualification-matrix.md);
the only flash-path interaction is that a flash operation stops a running
identification first.

Run `bazel test --config=release //...` first.

## Prerequisites

- A Subaru bench ECU and TCU reachable over K-Line (SSM2), through OpenPort
  2.0 and through a direct K-Line cable.
- A Subaru bench ECU on one of the two `SSM`-transport (SSM1) entries in
  the built-in catalog.
- A Subaru bench ECU reachable over iso15765.
- A Mitsubishi bench ECU with MUT/DMA logging.
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
| 1 | Connect, Subaru, K-Line, ECU selected, OpenPort 2.0 | Status bar shows the ECU ID; logboxes reflect the ECU's capability bits; the window stays responsive during identification | Not yet tested |
| 2 | Connect, Subaru, K-Line, ECU selected, direct K-Line cable | Same as #1 | Not yet tested |
| 3 | Connect, Subaru, K-Line, TCU selected | Status bar shows the TCU ID; request target byte is `0x18` | Not yet tested |
| 4 | Connect, Subaru, K-Line, with the wrong unit selected (TCU radio on an ECU-only bench) | Each attempt logs a `target id` validation failure; after five attempts the connection is dropped and the controls are re-enabled | Not yet tested |
| 5 | Connect, Subaru, `SSM` transport (SSM1) | Status bar shows the ECU ID, with the now echo-checked wake-up writes; record the wire trace | Not yet tested |
| 6 | Connect, Subaru, iso15765 | Status bar shows the `F182` ID | Not yet tested |
| 7 | Start logging, Mitsubishi MUT/DMA, not yet connected | Logging starts with no 2.5 s pause beforehand; values update | Not yet tested |
| 8 | Start logging, Subaru K-Line, not yet connected | Logging starts after identification completes | Not yet tested |
| 9 | Disconnect during identification (#1 with the ECU unpowered) | Identification stops within a second; controls re-enabled; no ECU ID appears afterwards | Not yet tested |
| 10 | Close the window during identification | The application exits promptly without hanging | Not yet tested |
| 11 | BIU window | The link opens at 10400 on the selected port; BIU commands answer | Not yet tested |
| 12 | DTC and DataTerminal windows | Both open on the selected port and communicate as before | Not yet tested |
| 13 | DTC, BIU, and DataTerminal with no adapter connected (empty port list) | Each shows "No serial port selected!" and nothing crashes | Not yet tested |
| 14 | Port refresh and log-transport switching, then connect | The newly chosen port and transport are used | Not yet tested |
| 15 | Read ROM on an OpenPort 2.0 | Battery voltage updates in the flash window during the operation | Not yet tested |
| 16 | SSM1 connect, then a DTC read over K-Line | The DTC session opens with no parity (previously it could inherit SSM1's even parity) | Not yet tested |
| 17 | Connect while Mitsubishi MUT/DMA logging is running | Logging stops cleanly (Logging action unchecks, no error dialog), then the connect runs; with "Log to file" on, the datalog file stays open and the next logging session appends to it | Not yet tested |
| 18 | Connect while Subaru K-Line logging is running | Logging stops cleanly, then identification runs and completes; the status bar shows the ECU ID | Not yet tested |
| 19 | ECU/TCU radio buttons during #1, and during #8 until logging has started | Both radio buttons are disabled; they re-enable when identification finishes | Not yet tested |
| 20 | Battery voltage on an OpenPort 2.0 during #1 | The voltage display stops updating while identification runs and resumes afterwards; identification is not disturbed | Not yet tested |
| 21 | Start logging, Subaru K-Line with the ECU unpowered, then open the DTC window before the five attempts finish | Identification stops; "Unable to connect to ECU" appears; the DTC window opens; after closing it the port list and refresh button are enabled | Not yet tested |
| 22 | As #21, but with a port refresh, a log-transport change, the BIU window, and the terminal window in place of DTC | Same as #21 for each | Not yet tested |
| 23 | BIU window on a newly chosen port, then restart the application | The BIU port is the one selected after restart | Not yet tested |
| 24 | Connect, Subaru, iso15765 (#6), then check the system log | The log shows `ECU ID:` followed by the `F182` ID (the ID is recorded for iso15765) | Not yet tested |
