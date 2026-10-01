# Static menu actions

First slice of the P1 "Separate UI from application logic" item in
[tech-debt.md](../../tech-debt.md). Later slices (write preflight, connection
orchestration, logging selection and log views, diagnostic-tool windows) get
their own specs.

## Intent

Make `MainWindow`'s menu handling declarative and typed, so presentation flows
can be tested without walking a live menu bar. Success: the menu structure is
compiled in, no `MainWindow` code finds an action by display text or id string,
and the runtime menu-building machinery is gone.

## Problem

The menu bar is built at runtime from `resources/shared/config/menu.cfg`
(45 items across 8 menus, plus a `<popup_menu_definitions>` section nothing
reads). `build_menus` creates each `QAction`, names it with the file's `id`, and
routes it through a `QSignalMapper` to `MainWindow::menu_action_triggered`,
which switches on `MenuCommand` (`menu_command_from_id`).

The file has been there since the first upstream commit (`1ddf30ac`, 2022) and
is copied into the per-version config directory only when absent
(`copy_bundle_if_absent`). No document records user customization as a goal.
Its costs are concrete:

- Six sites (`set_flash_arrow_state`, `restoreLoggingUiState`,
  `set_identification_in_progress`, `set_realtime_state`, `toggle_realtime`,
  `toggle_log_to_file`) re-find actions by scanning every menu and matching
  `action->text()`. A rename in the file silently breaks them.
- Dispatch, the id table, the file parser, the builder, the provisioning entry
  and a startup failure dialog exist only to rebuild what Qt's own `.ui`
  mechanism expresses declaratively.
- Menu text cannot be translated.

## Design

### Menu structure in `mainwindow.ui`

Every `menu.cfg` menu, submenu, separator and item becomes a `QMenu` / `QAction`
in `src/ui/desktop/mainwindow.ui`. `uic` generates a typed member per action
(`ui->actionToggleRealtime`). Per action the `.ui` carries text, tooltip,
checkable, icon and, for the toolbar items, membership in `toolBar`. Items that
are commented out in `menu.cfg` are not carried over.

The toolbar keeps its current order: menu-sourced actions first, then the
transport and port comboboxes that `MainWindow` appends in code.

### Wiring

`MainWindow` connects each action directly:
`connect(ui->actionOpenCalibration, &QAction::triggered, this, &MainWindow::...)`.
`QSignalMapper`, `menu_action_triggered`, `MenuCommand`, `menu_command_from_id`
and the unknown-id path are removed. Handlers keep their current behavior;
only the way they are reached changes.

The six text-scan sites become direct member access, for example
`ui->actionToggleRealtime->setChecked(state)`. A missing action is now a
compile error, so no null guards are needed.

### Shortcuts

- Where Qt has a platform standard, set it with `QKeySequence::StandardKey`:
  Open, Save, Save As, Copy, Paste. Designer stores only literal sequences, so
  these are applied by a small function in `MainWindow` setup rather than in
  the `.ui`.
- Quit gets both `QKeySequence::Quit` and `Ctrl+Q`. `QKeySequence::Quit` is
  unbound on Windows, where `Ctrl+Q` is bound today.
- Every other shortcut keeps its current literal value in the `.ui`: `F3`,
  `F4`, `Space`, `+`, `-`, `Ctrl++`, `Ctrl+-`, `S`, `Ctrl+H`, `Ctrl+J`,
  `Ctrl+I`, `Ctrl+L`. The current `shortcut="false"` on Settings is not
  carried over; it produced no binding.
- No user-editable shortcuts. If wanted later, a settings page storing
  `objectName → QKeySequence` overrides is a separate, small change.

### macOS roles

`QAction::menuRole` is set on Quit (`QuitRole`), About (`AboutRole`) and
Settings (`PreferencesRole`) so macOS places them in the application menu. No
effect on Windows or Linux.

### Removed

- `build_menus` and `menu_builder` (`src/ui/desktop/menu/`) with its test.
- `menu_definition` and its test (`src/backend/config/`), and
  `ConfigPaths::menu_file`.
- `MenuCommand` (`src/algorithms/menu/`) with its test, once nothing else
  references it.
- `menu.cfg` from `resources/shared/`, `config.qrc` and the related Bazel data
  and env wiring.
- The "Unable to load menu config file" startup dialog.
- The `menu_action_triggered` slot.

An existing `menu.cfg` in a user's config directory is left in place and
ignored.

## Behavior

Changed on purpose:

- Menu edits in `menu.cfg` no longer have any effect.
- Open, Save, Save As, Copy and Paste use the platform's standard keys (Cmd on
  macOS). Windows and Linux bindings are unchanged.
- About, Quit and Settings move to the application menu on macOS.
- Action tooltips are the item's own tooltip text. The legacy
  submenu-name-or-own-name prefix is dropped; an empty tooltip falls back to
  the action text.
- Menu text is marked translatable. No translation files are added.
- Broken icons are fixed. The six Testing-menu items point at
  `:/icons/icons/gtk-about.png`, which does not exist (doubled `icons/`, no
  such file in `icons.qrc`), so they show no icon today. Diagnostic Trouble
  Codes gets the bundled `utilities-system-monitor.png`. Hex Editor, Terminal,
  BIU communication, Get Encryption Key and WinOLS CSV to RomRaider XML get no
  icon: nothing in `icons.qrc` fits them, and 16 other items already have none.
  No new icon assets are added. The icon paths referenced only from
  commented-out items and the unused popup section are not carried over.

Pinned unchanged:

- Menu, submenu and separator order and item text; toolbar membership and
  order.
- Checkable state of Logging and Log to file.
- Every action reaches the same handler as before.
- `Disconnect` stays enabled during identification; the flash-arrow rules for
  read, test write and write are unchanged.
- Logging and log-to-file state propagation.
- The other 39 icon assignments (all that resolve in `icons.qrc` today) are
  carried over by path.

No ECU I/O, protocol or address-guard code is touched.

## Testing

Tests first, against the current runtime-built menu.

1. **Menu golden.** Build `MainWindow` and record, per menu path, each item's
   text, shortcut, checkable flag, toolbar position, tooltip and whether its
   icon is non-null. Extend the existing golden in `menu_builder_test` where
   it already covers this. Passing before any change.
2. **Handler reach.** Existing `mainwindow_test` cases that call
   `menu_action_triggered("<id>")` through `QMetaObject::invokeMethod` are
   rewritten to find the action by object name and call `trigger()`, still
   asserting the same handler effects. They pass before and after.
3. **Formerly text-keyed behavior.** Cases for the six sites above, run
   against the current build first.
4. **After migration** the golden is compared again. Each deliberate
   difference (standard keys per platform, tooltips, dropped
   `shortcut="false"`) is an explicit, commented expectation, not a loosened
   assertion.
5. **Shortcut collisions.** No two actions in the window share a key sequence.
6. **Icons resolve.** Every action that names an icon has a non-null `QIcon`
   from the bundled resources, so a mistyped path fails a test instead of
   showing nothing. Step 1 records the six known-null items; step 4 expects
   only Diagnostic Trouble Codes to gain an icon.

The `qt_layer` ratchet list gets no new entry. BUILD files are regenerated with
`python3 scripts/gazelle_check.py --fix`. Docs that name `menu.cfg`
(`docs/design-notes.md`) are updated.

## Staging

Single spec; the plan stages it so each step builds and passes:

1. Characterization tests (items 1 to 3 above).
2. Add the `.ui` actions and direct connections alongside the old path; switch
   the six lookups.
3. Remove the runtime builder, mapper, config parser, `MenuCommand`,
   `menu.cfg` and wiring; update docs and tests.

## Out of scope

- User-editable shortcuts or menus.
- Translation files.
- The popup (context) menus, which `menu.cfg` never built.
- The toolbar combobox, log-window and diagnostic-window `findChild` lookups
  (the latter belong to the diagnostic-tool-windows slice).
- The other four P1 slices.
