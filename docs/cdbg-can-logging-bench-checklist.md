# Colt CZT (Z37A, 47110032) Cdbg CAN logging — bench qualification checklist

Gate before any real-vehicle use of Cdbg live-data logging (`log_protocol`
`CDBG`, `MitsuColtCanCdbg::CdbgLogDriver`). Same convention as this
project's other bench-qualification gates (e.g.
`docs/colt_czt_47110032_can_bench_checklist.md` for the CAN reflash port).

1. **Raw CAN connect.** Power a bench/spare Z37A ECU, select "Mitsubishi /
   Colt CZT / Z37A 5MT" in FastECU, enable "Logging". Confirm the raw CAN
   connection opens (`set_is_can_connection(true)`, destination filter
   `0x631`) without error.
2. **Session handshake.** Confirm `CdbgLogDriver::startFreeFormLog()`'s
   init command gets a reply on `0x631` at all. If there is no reply, the
   "stock/built-in feature" assumption behind this port is wrong - stop
   and revisit before investing further here.
3. **Security access.** Confirm the seed/key exchange grants access
   (security-flags byte nonzero in the key-response reply).
4. **Streaming.** With the placeholder channels from
   `resources/shared/config/logger_cdbg_example.xml`, confirm frames arrive on
   `0x631` at the configured interval and decode to *some* value (even if the
   placeholder address doesn't mean anything meaningful yet).
5. **Only after 1-4 pass repeatably**, invest in deriving real channel
   addresses via `rom-analyzer` against `47110032` for a first useful
   default channel set - a follow-up, not part of the initial port.
