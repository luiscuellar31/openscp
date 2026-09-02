# Keyboard shortcuts

Most OpenSCP shortcuts follow the familiar commander-style workflow: first
focus the panel you want to work with, then press the shortcut. File operations
such as copy, move, rename, and delete apply to that panel's selection.

Some remote actions may be unavailable when the connected protocol does not
support them. On macOS, a function-key shortcut may also require `Fn`, depending
on the keyboard configuration.

## Global shortcuts

| Action | Linux | macOS | Notes |
| --- | --- | --- | --- |
| Search in the current panel | `Ctrl+F` | `Cmd+F` | Searches the remote panel by default while connected; otherwise it searches the left panel. |
| Enter or choose a directory | `Ctrl+L` or `Ctrl+O` | `Ctrl+L`, `Cmd+L`, or `Cmd+O` | Opens the directory dialog for the current panel. |
| Show transfers | `F12` | `F12` | This default can be changed in Settings → Shortcuts. |
| Show navigation history | `Ctrl+Shift+H` | `Ctrl+Shift+H` | This default can be changed in Settings → Shortcuts. |
| Enter or leave full screen | `F11` | `Ctrl+Cmd+F` | Uses the standard shortcut for the platform. |
| Open settings | `Ctrl+,` | `Cmd+,` | Uses the standard preferences shortcut. |
| Quit OpenSCP | `Ctrl+Q` | `Cmd+Q` | Uses the standard quit shortcut. |

## When a file panel has focus

The following shortcuts act on the file list that currently has focus:

| Shortcut | Action | Availability |
| --- | --- | --- |
| `F2` | Rename the selected item | Available locally and when the remote protocol supports renaming. |
| `F5` | Copy the selection to the other panel | Availability depends on the source and destination. |
| `F6` | Move the selection to the other panel | Availability depends on the source and destination. |
| `F7` | Download the remote selection to the local panel | Remote panel only. |
| `F8` | Choose files or folders to upload | Remote panel only. |
| `F9` | Create a folder | Available locally and when the remote protocol supports it. |
| `F10` | Create a file | Available locally and when the remote protocol supports it. |
| `Delete` | Delete the selection | Available locally and when the remote protocol supports deletion. |

## Working with paths

The path bar keeps the complete current path visible in a conventional field.
Click the name of any parent directory to go back to it. From the keyboard,
focus the field with `Tab`, choose a segment with `Left`, `Right`, `Home`, or
`End`, and activate it with `Space` or `Enter`. Activating the current directory
opens the **Open Directory** dialog.

`Tab` and `Shift+Tab` also move through every enabled action in the main, left,
and right toolbars and through both file panels. The first traversal after the
main window opens starts at **Connect**. A system-colored outline identifies the
focused action or panel only while navigating with the keyboard. Press `Space`
or `Enter` to activate a toolbar action.

From that dialog you can type or paste a path, return to a recent location, or
open one of the favorites saved for the current local panel or remote session.

## Changing shortcuts

The shortcuts for Transfers and History can be changed under
**Settings → Shortcuts**. The remaining shortcuts are currently fixed.

The complete keyboard and screen-reader audit is documented in
[ACCESSIBILITY.md](ACCESSIBILITY.md).
