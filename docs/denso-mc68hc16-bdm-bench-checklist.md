# Subaru Denso MC68HC16Y5 BDM bench checklist

## 0. STOP — not hardware-qualified

Do not treat this family as proven until every section below has passed on
real hardware. Record the BDM bridge (make, model, firmware), the serial
adapter and port, the ECU part number and identifier, date, operator and
result.

## 1. Bridge protocol

- Capture a serial trace of one read and one kernel bootstrap.
- Confirm the bridge accepts commands with no line terminator
  (`rpmem 0x00000000 0x00000400`). If it needs CR or LF, stop and record it.
- Record the exact bytes the bridge sends after `wpcsp` and after `go`. A
  later change will gate them; until then they are only logged.

## 2. Read

- Read the full ROM and compare `0x00000–0x1FFFF` and `0x28000–0x2FFFF`
  byte-for-byte with a trusted K-Line dump of the same ECU.
- Confirm `0x20000–0x27FFF` is `0xFF` in the saved image.
- Pull the bridge cable mid-read and confirm the operation fails instead of
  returning a short image.

## 3. Kernel bootstrap

- Run Write with the stock `ssmk_mc68hc916y5.bin` kernel. Confirm every chunk
  is acknowledged with `ACK_WR` and the operation ends after `go`.
- Confirm the loaded ROM in FastECU is unchanged afterwards.
- Confirm the kernel is running: connect with the
  `sub_ecu_denso_mc68hc16y5_02` K-Line protocol and read the ROM.
