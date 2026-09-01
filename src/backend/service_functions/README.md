# Service functions

Operator-gated, non-flash ECU/TCU routines that exchange bytes over
`ISsmTransport`.

That sentence is the membership rule. A routine belongs here when it needs an
operator to act on the vehicle partway through the sequence, or when it is a
maintenance routine rather than a flash operation — and when it needs nothing
from the transport beyond raw request/response bytes.

Flash operations do not belong here: they build and validate a `FlashPlan`,
collect every confirmation before execution, and run through
`IFlashExecutor`. See `src/backend/flash`.

Today the package holds one family's three operations, ported from
`FlashTcuSubaruDensoSH705xCanOperation`'s `tcuAction` 2, 3 and 4. Their
hardware status and the legacy defects corrected by the port are recorded in
the [flash qualification matrix](../../../docs/flash-qualification-matrix.md#service-functions).
