# Typed menu actions

First slice of the P1 "Separate UI from application logic" item in
[tech-debt.md](../../tech-debt.md). Later slices (write preflight, connection
orchestration, logging selection and log views, diagnostic-tool windows) get
their own specs.

## Intent

Make `MainWindow` presentation flows testable without walking a live menu bar.
Success: no `MainWindow` code finds a menu action by its display text; actions
are reached by `MenuCommand`, and the lookup is unit-testable on its own.

## Problem

`build_menus` (`src/ui/desktop/menu/menu_builder.cpp`) sets each action's
`objectName` to its `menu.cfg` id, and `menu_command_from_id` maps that id to a
`MenuCommand`. Dispatch already uses it. Six enable/check/read sites instead
re-scan every menu and match `action->text()` against `"Logging"`,
`"Log to file"`, `"Connect"`, `"Read from ecu"`, `"Test write to ecu"` and
`"Write to ecu"`:

- `mainwindow.cpp`: `set_flash_arrow_state()`, `restoreLoggingUiState()`
- `menu_actions.cpp`: `set_identification_in_progress()`,
  `set_realtime_state()`, `toggle_realtime()`, `toggle_log_to_file()`

`menu.cfg` is user-editable. Renaming "Logging" makes `toggle_realtime()` leave
`logging_state` stale and `restoreLoggingUiState()` stop un-checking the action.

## Design

### `MenuActions`

New UI-owned class in `src/ui/desktop/menu/menu_actions.{h,cpp}`.

- Holds `QHash<MenuCommand, QPointer<QAction>>`.
- `QAction *get(MenuCommand) const` returns null when the menu file does not
  define that id.
- `set_enabled(MenuCommand, bool)` and `set_checked(MenuCommand, bool)` are
  no-ops when the action is absent.
- A repeated id: the first action wins, later ones are ignored.
- `build_menus` fills it while creating actions and returns the mapper together
  with the `MenuActions`. Ids resolve through `menu_command_from_id`; no second
  id table.

### MainWindow

`MainWindow` owns one `MenuActions`, set right after `build_menus`. Each
text-scan site becomes a lookup:

| Site | Command(s) |
|---|---|
| `restoreLoggingUiState`, `set_realtime_state`, `toggle_realtime` | `ToggleRealtime` |
| `toggle_log_to_file` | `LogToFile` |
| `set_identification_in_progress` | `ConnectToEcu`, `ToggleRealtime` |
| `set_flash_arrow_state` | `ReadRomFromEcu`, `TestWriteRomToEcu`, `WriteRomToEcu` |

A missing action leaves state reads at their previous value and makes writes
no-ops.

### Behavior

Changed on purpose:

- Renamed menu text keeps working.
- Each lookup matches exactly one action per id, not every action sharing the
  text.

Pinned unchanged:

- `Disconnect` stays enabled during identification.
- Flash-arrow enable/disable rules for read, test write and write.
- Logging and log-to-file state propagation.

No ECU I/O, protocol or address-guard code is touched.

## Testing

Tests first.

1. Characterization tests in `mainwindow_test.cpp` for the pinned behaviors,
   against the default `menu.cfg`, passing before any refactor.
2. `menu_actions_test` (package-owned): lookup by command, missing id returns
   null, duplicate ids, no-op helpers on a missing action.
3. A `mainwindow_test` case with a menu file whose item text is renamed:
   logging toggle and arrow state still work.

The `qt_layer` ratchet list gets no new entry. BUILD files are regenerated with
`python3 scripts/gazelle_check.py --fix`.

## Out of scope

- Replacing `QSignalMapper` dispatch or `menu_action_triggered`.
- Menu text localization.
- The toolbar combobox, log-window and diagnostic-window `findChild` lookups
  (the latter belong to the diagnostic-tool-windows slice).
- The other four P1 slices.
