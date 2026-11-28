# Using OpenSCP

[Leer en español](USAGE_ES.md)

This guide covers how OpenSCP behaves once it is running: moving between
folders, what connections, transfers, and synchronization do in unusual cases,
and how to get diagnostic output. See also:

- [Keyboard shortcuts](KEYBOARD_SHORTCUTS.md)
- [Updating OpenSCP](UPDATES.md)
- [Platform and protocol compatibility](PLATFORM_COMPATIBILITY.md)

## Path navigation

Each panel presents its current location as one conventional path field. Click
any parent directory within the path to open it, or click the current directory
to open the Open Directory dialog. You can also use `Ctrl+L` (`Cmd+L` is
additionally supported on macOS) or the platform Open shortcut: `Ctrl+O` on
Linux and Windows and `Cmd+O` on macOS.

The dialog accepts a path directly and provides recent paths and the favorites
for the selected local panel or remote session. See
[Keyboard shortcuts](KEYBOARD_SHORTCUTS.md) for the complete shortcut
reference and panel-specific behavior.

## Connections

Canceling a connection or closing the window also cancels pending SSH
authentication and fingerprint prompts. SSH tries the resolved addresses as
Happy Eyeballs does: when one has not answered within 250 ms, or refuses, the
next one starts, alternating IPv6 and IPv4, and all attempts share a 20-second
limit. Name resolution uses the system resolver and may take longer
independently of that TCP limit.

## Transfers

Local downloads reject symbolic links in user-writable destination parent
folders. Choose the physical destination folder when a download path contains
a link.

Downloads require regular `.part` files. If an existing `.part` entry is a
FIFO (named pipe), the download reports a local I/O error without waiting for
a reader or replacing that entry. Choose another destination or inspect the
conflicting entry before retrying.

Transfers sharing a destination or its `.part` path run sequentially. Local
names differing only in case or Unicode normalization also run sequentially,
even on case-sensitive volumes. Removing a task with its partial data keeps the
`.part` file while another task in the queue can still resume from it, and
removes nothing for finished tasks or for classic SCP uploads, which write the
destination directly.

For a local move upload on macOS and Linux, OpenSCP uploads the file and
preserves the local source with a warning. Automatic removal cannot safely
exclude another process replacing the source after verification. Review the
completed upload and the local source before removing it manually, then retry
the task to finish cleanup without uploading again. After a restart, cleanup
requires manual review because the original file identity is no longer
available. On Windows, OpenSCP removes the local source after the upload if it
is unchanged: it holds the file open against changes while it checks and
deletes it. Remote source cleanup for move downloads is unchanged.

## Synchronization

Synchronization resolves a linked local root to its physical path and rejects
linked folders below that root.

Synchronization never copies FIFOs, sockets, or devices, and leaves out names
with a backslash or control characters, which uploads also refuse. A name that
differs from one on the other side only in letter case or Unicode normalization
appears as a conflict and is neither copied nor deleted.

Mirror proposes deletions only where the source scan verified that items are
absent. Skipped links, unreadable folders, and depth limits keep destination
items in those unverified subtrees.

## Diagnostics

These optional environment variables are useful when diagnosing a problem:

- `OPENSCP_LOG_LEVEL=off|error|warn|info|debug`
- `OPENSCP_TRANSFER_INTEGRITY=off|optional|required`
- `OPENSCP_KNOWNHOSTS_PLAIN=1|0`
- `OPENSCP_FP_HEX_ONLY=1`
- `OPENSCP_ENV=dev|prod` selects the runtime environment
- `OPENSCP_LOG_SENSITIVE=1` permits sensitive diagnostic details only together
  with `OPENSCP_ENV=dev`
- `OPENSCP_ENABLE_INSECURE_FALLBACK=1` only when the build permits it

Messages go to the standard error stream. On Windows, which shows no console
for OpenSCP, they also go to the debugger output, which
[DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview)
shows as they come. To keep them in a file instead, start OpenSCP from
PowerShell with the error stream saved to one:

```powershell
$env:OPENSCP_LOG_LEVEL = 'debug'
Start-Process OpenSCP.exe -RedirectStandardError openscp.log
```

Sensitive logging is disabled by default and should only be used temporarily in
a controlled development environment.
