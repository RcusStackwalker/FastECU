# Configure logger measurements with XML

Use a RomRaider-format logger definition to supply measurement names, request
IDs or addresses, widths, scaling, units, and display precision. FastECU loads
this file; the measurement chooser selects entries from it. An in-app definition
editor is [follow-up work](../../../docs/tech-debt.md#p2-logging-engine-follow-ups).

## Create a definition

Save a user-owned `.xml` file. Start with the structure below or adapt the
[MUT/DMA example](logger_mut_dma_example.xml) or [CDBG example](logger_cdbg_example.xml).
The MUT example's addresses are placeholders, and its parameters need
`enabled="1"` to participate in the current MUT logging implementation.

This example demonstrates the file structure. Replace its name, request code,
width, conversion, and unit with your own measurement definition; `0x4010` is
not a claim that a particular ECU stores the example value there.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<logger version="experimental">
    <protocols>
        <protocol id="MUT_DMA">
            <parameters>
                <parameter id="USER_VALUE" name="Custom value"
                           desc="User-defined measurement" length="1" enabled="1">
                    <address>0x4010</address>
                    <conversions>
                        <conversion units="units" expr="x*0.5-10" format="0.00"
                                    gauge_min="0" gauge_max="100" gauge_step="10"/>
                    </conversions>
                </parameter>
            </parameters>
        </protocol>
    </protocols>
</logger>
```

| Field | Meaning |
| --- | --- |
| Protocol `id` | Matches the selected logging protocol; use `MUT_DMA` for this example |
| Parameter `id` | Stable application identity stored in the saved selection; keep it unique within the protocol |
| `name` | Name offered in the measurement chooser |
| `enabled="1"` | Makes the MUT parameter eligible in the current implementation; it does not prove ECU support |
| `<address>` | Hexadecimal request code sent to the ECU |
| `length` | MUT/DMA read width: 1, 2, or 4 bytes |
| `expr` | Conversion of raw `x` using arithmetic, for example `x/4` or `x*0.5-10` |
| `units` | Unit text; `units=""` is valid for a value without a unit |
| `format` | Fixed precision: `0` for integer display, `0.00` for two decimal places |

For MUT/DMA, `<address>` holds a **16-bit native request code**. Firmware can
interpret it as a MUT-table ID or a compact RAM-address selector. A full absolute
RAM address such as `0x804010` cannot be entered directly in this field. Supply
the code your ECU expects; the application does not establish its measurement
meaning from a successful response.

The first conversion is used for logging. The accepted numeric, expression, and
format rules are owned by the [logging contracts](../../../docs/reference/logging-contracts.md#definition-input-validation).
Keep parameter IDs stable when changing names, scaling, or units so saved
selections continue to refer to the intended measurements.

## Load and select measurements

1. Open **Settings → Files → RomRaider Logger File**.
2. Use the browse button to select your XML file. The picker is named
   **Add RomRaider logger file**.
3. Close Settings to save the path. If saving fails, correct the reported error
   before continuing.
4. **Restart FastECU.** Definitions load when the main window is constructed;
   browsing to a file or editing its contents does not reload the active model.
5. Select a connection configuration whose logging protocol matches the XML
   protocol. Right-click the lower logging panel, open the **Digital** tab, and
   select the measurement names for its existing slots.
6. Turn on **Logging**. Enable **Log to file** when CSV output is wanted.

In the current implementation, Digital selections determine acquisition.
Gauge-only and switch selections do not add polling requests. The
[current run contract](../../../docs/reference/logging-contracts.md#per-run-snapshots)
owns that behavior; acquisition for all selected displays is separate follow-up
work.

A run captures its request configuration at startup. Stop and start logging to
apply changed selections; restart the application after editing XML definitions.
Current preparation rejects repeated participating IDs in the Digital list.

## Definitions and saved selections are different files

Your XML contains the measurement definitions. The active `logger.cfg` stores
per-ECU display selections by ID; it does not store request codes, scaling, or
units. Choosing another XML file does not replace existing saved selection IDs.

If an entry is missing from the chooser, check the selected protocol, parameter
ID, and `enabled="1"` for MUT/DMA, then restart after file edits. If a saved ID
is missing from the new definition, replace it through the chooser.

If there are no usable Digital slots, close FastECU and back up its active
`logger.cfg`. Update only the relevant ECU's
`protocol/parameters/lower_panel/parameter` IDs to reference your definitions,
then restart. Preserve the other ECU entries. The
[selection persistence contract](../../../docs/reference/logging-contracts.md#selection-persistence-and-csv)
describes the file's ownership and compatibility rules.

## Startup errors and protocol prerequisites

Unreadable or malformed XML produces a warning during application startup.
Malformed participating fields or an oversized request prevent logging startup
and report an error. Correct the definition or selection and retry.

Successful parsing and acknowledgements do not establish ECU measurement
support. In particular, the current 255-entry MUT host guard is a count-field
limit, not a qualified ECU capacity; the
[MUT/DMA capacity investigation](../../../docs/tech-debt.md#p1-resolve-known-correctness-gaps)
records the known firmware discrepancy.

XML configuration does not activate an ECU logging mode. The current MUT/DMA
binding assumes the ECU is already in the required 125000-baud mode. Consult the
[logging composition checklist](../../../docs/checklists/logging-composition-bench-checklist.md)
for transport and qualification prerequisites.
