# Configure logger measurements with XML

Use a RomRaider-format logger definition to supply measurement names, request
IDs or addresses, widths, scaling, units, and display precision. FastECU loads
this file; the measurement chooser selects entries from it. An in-app definition
editor is [follow-up work](../../../docs/tech-debt.md#p2-logging-engine-follow-ups).

## Create a definition

Save a user-owned `.xml` file. Start with the structure below or adapt the
[MUT/DMA example](logger_mut_dma_example.xml) or [CDBG example](logger_cdbg_example.xml).
The MUT example's addresses are placeholders, and its parameters need
`enabled="1"` for default/chooser eligibility. Select the explicit wire dialect
that matches the ECU; omitted dialect retains the maintained format.

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
| `enabled="1"` | Default/chooser eligibility when ECU support is unknown; not capability evidence |
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

### Select a MUT wire dialect explicitly

Use `<protocol id="MUT_DMA" dialect="oem-33520003">` for the statically analyzed
33520003 OEM format: little-endian request codes and two/four-byte stream values,
with at most 96 request entries and 96 response data bytes. Omitted dialect or
`dialect="legacy-be"` retains the maintained big-endian format and its 255-entry
count-field guard. The latter is not an ECU capacity claim. Unknown explicit
names prevent startup; neither setting requires ROM matching or probes support.
The [wire evidence](../../../docs/reference/logging-wire-evidence.md) owns the
scope and qualification boundaries. The bundled example's 33520003-labelled
placeholders do not automatically select this dialect; add the setting when it
matches your intended ECU format.

### SSM byte sources and switches

A nested parameter `<address length="2">0x10</address>` reads byte addresses
`0x10` and `0x11`. Legacy parameter-level `length` remains accepted; conflicting
lengths are rejected. Multiple one-byte `<address>` elements describe an explicit
ordered SSM byte list. Its 84-entry budget counts requested byte addresses.

A switch keeps sample metadata separate from capability metadata:

```xml
<logger><protocols><protocol id="SSM">
    <switches>
        <switch id="USER_FLAG" name="Custom flag" enabled="1"
                ecubyteindex="5" ecubit="1">
            <address bit="7">0x20</address>
        </switch>
    </switches>
</protocol></protocols></logger>
```

This is a structure example; supply your actual byte address and capability
metadata, or omit capability attributes when support is unknown. Sample bit 7
extracts the flag from the byte read at `0x20`; capability bit 1 is a distinct
identification fact. Legacy `byte="..." bit="..."` sample fields are also accepted;
use `ecubit` separately for capability evidence. The indicator shows ON/OFF and
CSV stores 1/0. Switch-only definitions can initialize selection slots.

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
   protocol. Right-click the logging panels and use the **Gauges**, **Digital**
   or **Switches** tabs to select measurement names for existing slots.
6. Turn on **Logging**. Enable **Log to file** when CSV output is wanted.

All selected gauges, Digital values and switches participate in acquisition.
Repeated display positions for the same measurement share acquisition; parameter
and switch IDs remain distinct. Gauge values are cached/exported; a gauge renderer
is [follow-up work](../../../docs/tech-debt.md#p2-logging-engine-follow-ups).

A run freezes its acquisition, display bindings and CSV columns. Chooser edits
save pending choices and show that logging must restart to apply them. Every new
run creates a fresh CSV if file logging is enabled, including an identical restart
or restart after Connect. Existing files are preserved with collision-safe names.
The [run and output contracts](../../../docs/reference/logging-contracts.md)
own these behaviors.

## Definitions and saved selections are different files

Your XML contains the measurement definitions. The active `logger.cfg` stores
per-ECU display selections by ID; it does not store request codes, scaling, or
units. Choosing another XML file does not replace existing saved selection IDs.

If an entry is missing from the chooser, check the selected protocol, parameter
ID, and `enabled="1"` for MUT/DMA, then restart after file edits. A known-unsupported or missing saved ID stays visible as unavailable and prevents
startup; replace it through the chooser. Explicit unknown-support measurements
remain subject to ordinary field/protocol validation.

If you need to add/remove slots, close FastECU and back up its active
`logger.cfg`. Update only the relevant ECU's
`protocol/parameters/gauges/parameter`,
`protocol/parameters/lower_panel/parameter`, or `protocol/switches/switch` IDs to
reference your definitions, then restart. Preserve the other ECU entries. The
[selection persistence contract](../../../docs/reference/logging-contracts.md#selection-persistence-and-csv)
describes the file's ownership and compatibility rules.

## Startup errors and protocol prerequisites

Unreadable or malformed XML produces a warning during application startup.
Malformed participating fields or an oversized request prevent logging startup
and report an error. Correct the definition or selection and retry.

Successful parsing and acknowledgements do not establish ECU measurement
support. In particular, the current 255-entry MUT host guard is a count-field
limit, not a qualified ECU capacity; the
[wire evidence](../../../docs/reference/logging-wire-evidence.md) records the
implemented dialect boundaries and outstanding qualification.

XML configuration does not activate an ECU logging mode. The current MUT/DMA
binding assumes the ECU is already in the required 125000-baud mode. Consult the
[logging composition checklist](../../../docs/checklists/logging-composition-bench-checklist.md)
for transport and qualification prerequisites.
