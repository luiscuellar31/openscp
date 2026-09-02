# Accessibility testing

OpenSCP relies on Qt's platform accessibility bridge and standard widgets.
Custom interactions must still expose a name, role, state, and action, remain
usable from the keyboard, and follow the system palette.

## Automated baseline

Run the widget accessibility checks with the rest of the unit suite:

```bash
ctest --test-dir build-ci-local \
  -R openscp_accessibility_widget_tests --output-on-failure
```

The test verifies the Qt accessibility tree, actions, focusable states, table
semantics, and palette inheritance. It cannot verify how every screen reader
speaks those objects, so complete the platform check below for changes to a
custom widget or important workflow.

## Keyboard-only check

Disconnect pointing devices while checking these paths:

1. Move focus through both file panels, their path fields, and toolbar actions
   with `Tab` and `Shift+Tab`. The first `Tab` in the main window must focus
   **Connect**. Focus outlines must appear around toolbar actions and complete
   file panels only for keyboard traversal, and disappear when focus leaves or
   the mouse is used. Enabled actions in the main, left, and right toolbars must
   all participate in the traversal and respond to both `Space` and `Enter`.
2. In a path field, use `Left`, `Right`, `Home`, and `End` to select a segment.
   Press `Space` or `Enter`; a parent must navigate to that folder and the
   current segment must open path entry.
3. Open the transfer queue, focus the table, and move through rows with the
   arrow keys. The current cell or row must always have a visible focus cue.
4. Change a queue filter with the keyboard and confirm that its checked state
   is visible. Open the row context menu with the platform Menu key or
   `Shift+F10` and invoke an action without using the pointer.
5. Trigger the drag-preparation overlay. Focus must move to **Cancel**, `Esc`
   must cancel it, and focus must return to the file view when it closes.

## Screen-reader matrix

Use a build for the platform being tested; a successful check on one platform
does not validate another accessibility bridge.

| Platform | Tool | Expected result |
| --- | --- | --- |
| macOS | VoiceOver | The complete path is announced as read-only text together with its keyboard instructions and selected target. The transfer list is announced as a table and exposes selected rows. |
| Windows | Narrator | The path field, checked queue filters, disabled/enabled actions, table cells, progress values, and cancel action are available. |
| Linux | Orca and Accerciser | Accerciser shows names, roles, states, and actions for every interactive element; Orca can operate the same keyboard workflow. |

Also confirm that changing the path or transfer status does not leave stale
objects or text in the accessibility tree.

## Contrast and appearance

Repeat the important screens with the operating system's light, dark, and high
contrast or increased-contrast appearance where available. Focus indicators
must remain visible, labels must use the system foreground/background palette,
and status must be communicated in text rather than color alone.

Do not add a stylesheet that suppresses `:focus`, fixed foreground/background
colors, or translucent RGBA overlays to an interactive widget. Prefer standard
Qt controls, palette roles, `QFrame` shapes, and explicit accessible names and
descriptions.

Qt's accessibility overview and QWidget guidance are the implementation
reference:

- <https://doc.qt.io/qt-6/accessible.html>
- <https://doc.qt.io/qt-6/accessible-qwidget.html>
