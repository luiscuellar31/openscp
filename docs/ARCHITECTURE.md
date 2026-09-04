# Architecture

This page is a map for contributors. It explains where a change belongs and
records a few rules that are easy to miss when reading one class at a time.

## Project layers

Read the table from top to bottom. A target may use the OpenSCP targets listed
below it, but lower layers must not depend on the UI layers.

| Layer | Target | Uses | Responsibility |
| --- | --- | --- | --- |
| Application | `openscp` | `openscp_ui_widgets` | Starts the application and assembles the main window. |
| Widgets | `openscp_ui_widgets` | `openscp_ui_logic` | Dialogs and reusable visual components. |
| Application logic | `openscp_ui_logic` | `openscp_core`, `openscp_sync_logic` | Sessions, navigation, transfers, and saved data. |
| Foundation | `openscp_sync_logic` | — | File comparison and synchronization plans. |
| Foundation | `openscp_core` | — | Protocols, remote paths, secure values, and safe local-file helpers. |

Protocol code must not know about windows or dialogs, and network work must
never run inside a model or widget.

Tests should link the closest target that owns the behavior. Integration tests
use the same protocol implementations as the application.

## Core source layout

The core keeps its public API small and puts everything else close to the
backend or helper that owns it:

| Directory | Built into | What belongs here |
| --- | --- | --- |
| `core/include/openscp/` | Public interface of `openscp_core` | Contracts and types shared with other targets, such as `RemoteClient`, `ClientFactory`, protocol values, remote errors, session options, and shared services. |
| `core/src/` | `openscp_core` | Backend composition that does not belong to one protocol family, currently `ClientFactory.cpp`. |
| `core/src/common/` | `openscp_core` | Private helpers shared by more than one backend. |
| `core/src/libssh2/` | `openscp_core` | SFTP and SCP implementations, with lower-level helpers under `detail/`. |
| `core/src/curl/` | `openscp_core` | FTP, FTPS, and WebDAV implementations and their shared curl plumbing. |
| `core/src/mock/` | `openscp_core` in test builds | The in-memory remote client used by unit tests. |

Public includes use the `openscp/` prefix, for example
`#include "openscp/RemoteClient.hpp"`. Production targets should create clients
through `ClientFactory` and work against `RemoteClient`; they should not include
a concrete backend. Tests may include `core/src/` when they are deliberately
testing an implementation detail, but that private include path must not leak
into production targets.

When adding a core file, first ask whether another target truly needs to include
it. If it does, it belongs in `core/include/openscp/`; otherwise, keep it under
the most specific `core/src/` directory. List both headers and sources in
`core/CMakeLists.txt` so they also appear correctly in IDE project trees.

## UI source layout

The UI follows the same ownership rule, first by target layer and then by
product domain or widget role:

| Directory | Built into | What belongs here |
| --- | --- | --- |
| `ui/app/` | `openscp`, `openscp_main_window` | Entry point and application composition. |
| `ui/widgets/` | `openscp_ui_widgets` | Dialogs and reusable visual components, grouped by widget role. |
| `ui/logic/` | `openscp_ui_logic` | UI-facing behavior, grouped by product domain. |
| `ui/sync/` | `openscp_sync_logic` | Data-only comparison and synchronization planning. |

UI includes name the owning directory, for example
`#include "logic/transfers/TransferManager.hpp"` or
`#include "widgets/dialogs/ConnectionDialog.hpp"`. The shared include root is
`ui/`; do not add every domain directory separately. Keeping the path visible
makes ownership clear and avoids ambiguous filenames. The generated
`AppVersion.hpp` is the only flat UI include.

When adding a UI file, start with the target that should own it, then choose the
closest product domain or widget role. List both headers and sources in that
target's `CMakeLists.txt` entry so they also appear correctly in IDE project
trees.

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

## Path navigation

Path navigation is split by responsibility:

- `PathNavigationModel` normalizes local and remote paths into testable segments
  and parent targets without depending on widgets.
- `PathNavigationBar` preserves a conventional read-only path field while
  using the model segments for mouse and keyboard navigation, and emits
  navigation or open-dialog requests.
- `OpenPathDialog` collects a typed path and exposes recent paths and favorites;
  it does not read settings or navigate by itself.
- `MainWindow` selects the local or remote presentation, performs navigation,
  and persists history and favorites through `NavigationStore`.

## Adding a change

`MainWindow` connects the application pieces and owns visible UI policy. The
controllers own the state and behavior behind it.

When deciding where new code belongs, start with the component that already
owns the data. Extract a new class when it has a clear lifetime, invariant, or
testable responsibility—not just to make another file shorter.

Build, test, and translation instructions are in
[CONTRIBUTING.md](../CONTRIBUTING.md).
