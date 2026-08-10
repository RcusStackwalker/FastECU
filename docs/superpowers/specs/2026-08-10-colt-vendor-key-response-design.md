# Colt vendor-key response handling

## Problem

The Colt vendor-extension handshake currently treats an echoed key
subfunction (`63 27 42`) as success. The ECU implementation instead writes
`0x34` into reply byte 2 after it verifies the submitted key, producing the
success payload `63 27 34`. FastECU therefore rejects a valid authorization.

## Design

Change only the vendor-key success predicate in `MitsuColtM32rCanExecutor`.
A response succeeds when it contains the four-byte CAN reply ID followed by
exactly the expected response prefix `63 27 34`; the existing minimum-length
guard remains in force. Do not continue accepting `63 27 42`, because there is
no ECU-side evidence that it grants access.

Keep the request unchanged as `23 27 42 <four-byte key>`. Keep existing error
handling for short or mismatched responses, including stopping before the
diagnostic-session request.

## Verification

Update the executor tests first so that a valid vendor handshake queues
`63 27 34` and proceeds to the diagnostic session. Preserve a rejection test
using `63 27 42` to prove that the former assumption is no longer accepted.
Run the focused Colt executor test target, then the relevant protocol tests.
