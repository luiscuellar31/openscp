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

The path field also works as a navigation control. Click the name of any parent
directory to go back to it. Clicking the current directory, or the unused area
of the field, opens the **Open Directory** dialog.

From that dialog you can type or paste a path, return to a recent location, or
open one of the favorites saved for the current local panel or remote session.

## Changing shortcuts

The shortcuts for Transfers and History can be changed under
**Settings → Shortcuts**. The remaining shortcuts are currently fixed.
