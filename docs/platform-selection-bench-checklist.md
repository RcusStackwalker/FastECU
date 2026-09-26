# Platform selection -- bench verification checklist

Step 6e moved the serial layer's direct/remote choice into the desktop
composition root and split the direct backend's Unix and Windows J2534 code
into per-OS source files. No wire bytes changed, but the J2534 open path was
reorganized on both operating systems and the Windows half is covered only
by CI. None of the items below is qualified until it is run and signed off.
Run `bazel test --config=release //...` first.

## macOS / Linux (OpenPort 2.0)

- [ ] `fastecu-bench ports` lists the OpenPort 2.0 entry.
- [ ] `fastecu-bench connect` succeeds against a bench ECU.
- [ ] In the desktop app, select the OpenPort 2.0 and start MUT/DMA logging;
      gauges update, and Stop ends the session cleanly.
- [ ] With a Bluetooth or debug-console port listed first, the app still
      opens the adapter, not the first port.

## Windows (J2534 DLL)

- [ ] The port list shows the installed J2534 vendor names after the serial
      ports.
- [ ] Selecting the OpenPort 2.0 vendor connects ("J2534: Interface opened
      succesfully!" in the log).

## Remote session

- [ ] `fastecu --host <peer> --password <pw>` shows the network splash and
      connects to a running remote peer. If no peer is available, record
      "unverified" here.

## Sign-off

- [ ] Date: ________________
- [ ] Adapter / OS: ________________
- [ ] Operator initials: ________________
