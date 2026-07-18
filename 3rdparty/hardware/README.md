# Hardware programmer sketches

Reference Arduino sketches for the MC68HC16 BDM (background debug mode)
programmer used to read/write Subaru Denso MC68HC16Y5 ECUs on the bench.
These are not compiled by the FastECU build; they are flashed to an
Arduino/Nano with the Arduino IDE.

- `FastECU-mc68hc16-bdm.ino` — BDM programmer sketch (Uno/Mega).
- `NANOFastECU-mc68hc16bdm.ino` — BDM programmer sketch (Nano).
- `MC68HC16Y5_BDM_TP_800x600.jpg` — BDM test-point wiring diagram.

Previously lived under `external/`; moved to avoid colliding with the
Bazel/Hedron generation-time `external` symlink at the workspace root.
