# Architecture

This page is a map for contributors. It explains where a change belongs and
records a few rules that are easy to miss when reading one class at a time.

OpenSCP is split into layers:

```text
openscp
  -> openscp_ui_widgets
       -> openscp_ui_logic
            -> openscp_sync_logic
            -> openscp_core
```

An arrow means "depends on." Dependencies should continue downward through the
diagram. Protocol code should not know about windows or dialogs, and network
work should never run inside a model or widget.

## Where code belongs

| Target | What belongs there |
| --- | --- |
| `openscp_core` | The remote-client interface, protocol backends, remote paths, secure values, and safe local-file helpers. |
| `openscp_sync_logic` | File comparison and synchronization plans that do not need widgets or network connections. |
| `openscp_ui_logic` | Sessions, navigation, saved data, remote jobs, and transfer coordination. |
| `openscp_ui_widgets` | Dialogs and reusable visual components. |
| `openscp` | Startup, `MainWindow`, menus, translations, and application resources. |

Tests should link the closest target that owns the behavior. Integration tests
use the same protocol implementations as the application.

## Connections and remote work

All protocols implement `openscp::RemoteClient`. `ClientFactory` chooses the
backend from `SessionOptions`: libssh2 handles SFTP and SCP, while libcurl
handles FTP, FTPS, and WebDAV.

A backend is responsible for validating remote paths, reporting the operations
it supports, protecting secrets with `SecureString`, and making cancellation
unblock network waits promptly. The libcurl backends share their connection,
proxy, TLS, and transfer plumbing; each protocol file keeps only its own request
and response rules.

`SessionController` owns the active connection. `RemoteOperationController`
runs control-connection jobs one at a time and returns completed results.
`RemoteTreeWalker` and `LocalTreeDiscovery` scan directories outside the UI
thread and can be canceled.

Models store results; they do not fetch them. Dialogs collect input; they do not
call a remote client. Host-key prompts, health checks, and connection timing are
kept separate from blocking network operations.

## Transfers and synchronization

`TransferManager` is the entry point for the transfer queue. It coordinates
worker connections, task state, retries, conflicts, destination reservations,
persistence, and notifications. The supporting classes have narrower jobs:

- `TransferQueue` stores tasks and selects work fairly.
- `TransferExecutor` runs a selected transfer.
- `BandwidthLimiter` applies speed limits.
- `TransferQueuePersistence` saves unfinished tasks safely.

There is one allowed nested lock order inside the manager:

```text
connFactoryMutex_ -> mtx_
```

Release manager locks before calling a remote client, invoking a callback, or
emitting a Qt signal.

Synchronization has three steps:

1. `SyncCoordinator` collects bounded local and remote snapshots.
2. `SyncComparisonEngine` creates a data-only plan.
3. The accepted actions enter `TransferManager` as one ordered batch.

## Saved data

Each kind of persisted data has one owner:

| Owner | Data |
| --- | --- |
| `AppSettings` | Application identity and settings keys. |
| `SavedSitesPersistence` | Saved sites and schema migrations. |
| `SiteCredentialRepository` | Credential keys and secure-storage migration. |
| `SecretStore` | Keychain, Secret Service/libsecret, and platform encryption. |
| `NavigationStore` | Local history and session-scoped remote history and favorites. |
| `TransferQueuePersistence` | Versioned, atomic storage for unfinished transfers. |

Use these classes instead of reading or writing their settings directly. A new
schema needs an explicit migration, and corrupt or newer data must not be
silently overwritten.

## Adding a change

`MainWindow` connects the application pieces and owns visible UI policy. The
controllers own the state and behavior behind it.

When deciding where new code belongs, start with the component that already
owns the data. Extract a new class when it has a clear lifetime, invariant, or
testable responsibility—not just to make another file shorter.

Build, test, and translation instructions are in
[CONTRIBUTING.md](../CONTRIBUTING.md).
