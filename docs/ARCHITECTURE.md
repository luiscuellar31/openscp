# OpenSCP Architecture

OpenSCP is split into protocol, reusable UI logic, widget, and composition
layers. Dependencies point inward: protocol and data types do not depend on
the application window, while `MainWindow` composes the reusable controllers
and presents translated UI.

## Build targets

| Target | Responsibility |
| --- | --- |
| `openscp_core` | Protocol clients, session options, secure values, safe local files, remote-path normalization, and runtime logging. |
| `openscp_sync_logic` | Data-only synchronization comparison and execution planning. |
| `openscp_ui_logic` | Models, persistence, transfer scheduling, remote operations, and connection coordinators. |
| `openscp_ui_widgets` | Dialogs and reusable widgets that present `openscp_ui_logic`. |
| `openscp_hello` | Application composition, menus, panels, translations, and platform resources. |

Tests link the production target that owns the behavior. They must not compile
copies of production `.cpp` files unless a small utility is intentionally
tested without its owning target.

## Core and protocols

`openscp::RemoteClient` is the stable virtual protocol contract. SFTP and SCP
use libssh2. FTP, FTPS, and WebDAV use libcurl while retaining their distinct
protocol semantics.

The curl backends share:

- `CurlClientState`, which owns the connected session, immutable
  `std::shared_ptr<const SessionOptions>`, interruption state, and operation
  serialization;
- common curl-handle configuration and error classification;
- `openscp::normalizeRemotePath(std::string_view)`, with Qt adapters at the UI
  boundary;
- FTP logical-root mapping from the server `PWD` and WebDAV base-path mapping.

`SessionOptions` stores passwords and passphrases as `SecureString`. Local
`FILE*` ownership uses `UniqueFile`. Code must not introduce parallel manual
ownership paths for those resources.

## Remote operations

`RemoteOperationController` serializes use of the control connection and
publishes job IDs plus immutable result values. Models never perform network
I/O. `RemoteModel` receives completed listings, precomputes permissions, and
uses a bounded icon cache.

`RemoteActionController`, `PaneController`, and `SyncCoordinator` receive
callbacks for presentation or application-specific policy. This keeps their
behavior testable without constructing `MainWindow`.

## Connection coordination

`SessionController` owns connection/disconnection lifecycle and the active
client. Two focused coordinators own state that previously lived in
`MainWindow`:

- `HostKeyPromptCoordinator` serializes host-key prompts, preserves pending
  prompt data, publishes a presentation callback, and wakes all waiters on
  cancellation.
- `SessionHealthMonitor` owns its timer, activity timestamps, application
  resume detection, active probe ID, overlap prevention, and cancellation on
  stop.

`MainWindow` provides translated strings, dialogs, status messages, and
connection-loss alerts through callbacks. It does not own either
coordinator's synchronization or probe state.

## Transfer queue

`TransferManager` owns stable task nodes in `TransferQueueStore`, worker-slot
connections, scheduling, persistence, retries, and destination reservations.
Hot-path consumers use `tasksAdded`, `tasksUpdated`, and `tasksRemoved`, then
request snapshots only for those IDs. A complete `tasksSnapshot()` is reserved
for initialization, persistence, or explicit full refreshes.

`TransferUiController` is initialized once from a full snapshot and then
observes upserts and removed IDs. Direct manager queries such as
`hasActiveTaskForSource`, `hasActiveTaskForDestination`, and `isBatchTerminal`
avoid copying large queues for simple decisions.

The permitted nested mutex order in `TransferManager` is:

```text
connFactoryMutex_ -> mtx_
```

Worker-client, retry, persistence, and performance mutexes are independent.
They must be released before acquiring the chain above. External client calls
and Qt signal emissions occur without these locks held.

## Persistence and settings

`AppSettings` centralizes the QSettings organization/application identity,
keys, synchronization, and owner-only file permissions. Saved sites preserve
their existing on-disk format; credentials are delegated to `SecretStore`.
Queue persistence uses an explicit versioned format and fails closed for
unknown future schemas or corrupt input.

## Validation boundaries

Unit tests cover data models and coordinators. Integration binaries exercise
real SFTP, SCP, FTP, FTPS, and WebDAV services when their environment is
available. Linux CI additionally verifies RELRO, immediate binding, PIE, and a
non-executable stack.

See [CONVENTIONS.md](CONVENTIONS.md) for enforceable coding and validation
rules.
