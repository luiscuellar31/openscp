# OpenSCP Architecture

OpenSCP separates protocol code from application policy and widgets. Dependency
arrows point toward the protocol-neutral layers; network operations never run
inside item models or dialogs.

```text
openscp
  -> openscp_ui_widgets
       -> openscp_ui_logic
            -> openscp_sync_logic
            -> openscp_core
```

## Build targets

| Target | Owns |
| --- | --- |
| `openscp_core` | Remote-client contract, protocol backends, secure values, remote paths, and safe local files. |
| `openscp_sync_logic` | Data-only comparison and synchronization planning. |
| `openscp_ui_logic` | Sessions, persistence, navigation, remote jobs, and transfer orchestration. |
| `openscp_ui_widgets` | Dialogs and reusable visual components. |
| `openscp` | Startup, `MainWindow`, translations, menus, and application resources. |

Tests link the target that owns the behavior. Integration tests use the same
protocol implementations as the application.

## Protocol boundary

`openscp::RemoteClient` is the common contract. SFTP and SCP use libssh2; FTP,
FTPS, and WebDAV use libcurl. `ClientFactory` is the only place that selects a
backend from `SessionOptions`.

Backends must:

- normalize logical remote paths before mapping them to a server root;
- expose supported operations through `ProtocolCapabilities`;
- return structured errors for retry and presentation policy;
- keep secrets in `SecureString` and local `FILE*` ownership in `UniqueFile`;
- make cancellation unblock network waits promptly.

The cURL implementations share connection state, bounded response handling,
proxy/TLS configuration, and upload/download lifecycle code. Protocol files own
only their request and response semantics.

## Remote operations and sessions

`SessionController` owns the active client and connection lifecycle.
`RemoteOperationController` serializes control-connection jobs and publishes
immutable results. `RemoteTreeWalker` and `LocalTreeDiscovery` perform bounded,
cancelable traversal outside the UI thread.

`RemoteModel` only stores completed listings. Dialogs and models never call a
remote client directly.

Host-key prompts, health checks, and connection-status timing are coordinated
independently so blocking network work cannot own presentation state.

## Transfers and synchronization

`TransferManager` is the transfer queue boundary. It owns worker connections,
task state, retries, conflict policy, destination reservations, persistence,
and notifications. `TransferQueue` keeps stable task storage and round-robin
selection; `TransferExecutor`, `BandwidthLimiter`, and
`TransferQueuePersistence` each own their narrower policy.

The only permitted nested manager lock order is:

```text
connFactoryMutex_ -> mtx_
```

External client calls, callbacks, and Qt signal emissions occur after releasing
manager locks.

Synchronization follows three steps:

1. `SyncCoordinator` gathers bounded local and remote snapshots.
2. `SyncComparisonEngine` produces a data-only execution plan.
3. Accepted actions enter `TransferManager` as one ordered batch.

## Persistence

Each persisted domain has one owner:

| Owner | Data |
| --- | --- |
| `AppSettings` | Application identity and settings keys. |
| `SavedSitesPersistence` | Saved-site records and schema migration. |
| `SiteCredentialRepository` | Credential keys and migration into secure storage. |
| `SecretStore` | Keychain, Secret Service/libsecret, and platform encryption. |
| `NavigationStore` | Local history plus session-scoped remote history/favorites. |
| `TransferQueuePersistence` | Versioned, atomic storage of non-terminal tasks. |

Presentation code must use these boundaries instead of writing raw settings or
secret keys. Unknown future schemas and corrupt data must fail without
overwriting recoverable files.

## Application composition

`MainWindow` creates the models, controllers, and dialogs, then connects their
signals and presentation callbacks. It owns translated text and visible
application policy, but reusable controllers own their operational state.

When adding behavior, prefer extending the component that already owns the
data. Add a new class only when it gains a clear lifetime, invariant, or testable
policy; do not create one merely to shorten another file.

Contributor rules, validation, and translation commands live in
[CONTRIBUTING.md](../CONTRIBUTING.md).
