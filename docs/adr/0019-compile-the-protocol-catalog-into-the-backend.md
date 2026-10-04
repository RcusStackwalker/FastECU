# ADR 0019: Protocol and Vehicle Data Are Compiled into the Backend

## Status

Accepted.

## Context

`protocols.cfg` held 63 flash and logging protocols and 65 vehicles. It was
bundled as a Qt resource, copied into each version's config directory when
absent, and parsed at startup and again on every flash and EEPROM read. It
looked like user configuration, but:

- Every flash family already declared each protocol's MCU, kernel load
  address and supported operations in its own plan, and rejected a file value
  that disagreed. Flash and checksum routing select behavior by protocol
  name, so a protocol added to the file reached no code.
- The checksum flag was the one flash-relevant field the code trusted:
  changing `yes` to `no` wrote an uncorrected image without a warning.
- Provisioning is per version and migrates only `fastecu.cfg`, so an edited
  file was replaced at every upgrade, and the saved selection -- a row number
  into the file -- could name a different vehicle after one.
- Nothing checked the data: a kernel name that matched its file only on
  case-insensitive filesystems, two vehicles pointing at renamed protocols,
  14 protocols no vehicle reached, and one EEPROM entry with the SH7055 kernel
  address on an SH7058.

## Decision

- Protocols and vehicles are `constexpr` data in `src/backend/config`
  (`builtin_catalog`), with no runtime override. Changes go through pull
  requests.
- Consumers receive a `Catalog` value. Only the desktop composition root names
  the built-in one; tests pass synthetic catalogs.
- Each hardware fact (MCU, kernel, load address, supported operations) stays
  declared both in the catalog and in its flash family. Tests prove the two
  agree, and the checksum flag agrees with checksum routing; plan validation
  keeps checking an independent declaration.
- The saved selection is a stable vehicle id. A saved id this build does not
  know selects nothing, and startup asks for a vehicle before the main window
  exists.

Rejected: making each flash family the single owner of its hardware facts (a
large hardware-facing refactor with no safety gain once the data is compiled
in), and making the catalog the single owner with plans validating against it
(the plans would compare a value with itself).

## Consequences

- An upstream `protocols.cfg` change is ported by hand into the catalog; it
  needed route and plan code to have any effect anyway.
- Upgrading resets the saved selection once.
- Kernels are unaffected: still bundled, provisioned to disk and read by name.
- `fastecu.cfg` and `logger.cfg` remain user files.
