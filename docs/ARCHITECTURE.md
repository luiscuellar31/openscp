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

The WebDAV listing parser checks XML resource budgets before building
TinyXML2 documents. An allocation-free linear scan bounds payload bytes,
nesting, and node/attribute work, including non-DAV content; attributes per
element are capped separately to bound duplicate-attribute checks. The scan
hands each `response` record to TinyXML2 on its own as soon as it closes, so
the node budget bounds each record, and separately the content around the
records, instead of the whole listing, and memory stays proportional to one
record. TinyXML2 validates each record; the scan checks that the elements
around them nest and close. The existing response-record and path-byte budgets
apply independently, and rejected listings expose no partial results.

WebDAV servers apply DELETE, and MOVE with `Overwrite: T`, to whole
collections. The WebDAV backend therefore checks the target with PROPFIND
first: `removeDir` deletes only empty collections, `removeFile` refuses
collections, and uploads and overwriting renames replace only files, sending
`Overwrite: F` when nothing is at the destination.

Requests on a collection the client knows, such as the connection probe of the
base, a listing, or the DELETE of an empty collection, use its URL with the
trailing slash. Redirects are never followed, but a request that the server
redirects to the same path plus a slash, as Apache's mod_dir does by default,
is repeated once with that collection URL.

The transfer queue treats NotFound from a delete as done, so a backend must
report a missing path as NotFound. FTP answers 550 both for a missing path and
for refused access, so after a failed DELE, RMD or RNFR the FTP backend looks
the path up before reporting PermissionDenied.

SFTP and SCP uploads give the remote file the local file's permission bits,
as OpenSSH's sftp and scp do. SFTP writes the `.part` with mode 0600 and
applies those bits just before the rename; FTP and WebDAV leave permissions to
the server.

`SessionController` owns the active connection. `RemoteOperationController`
runs control-connection jobs one at a time and returns completed results.
Its bulk cancellation methods interrupt matching active I/O before marking
the remaining registrations because both share the active job's cancel flag.
They count newly canceled jobs once; generation-specific cancellation leaves
other generations untouched. Interrupting can drop the control connection, as
SFTP does, so work that is only replaced (a newer listing, a search at its
match limit, a sync scan over its size limit) cancels without interrupting
and lets the active call finish.
`RemoteTreeWalker` and `LocalTreeDiscovery` scan directories outside the UI
thread and can be canceled. Deleting a remote folder removes the links inside
it as entries and never follows them.
Local discovery enumerates without sorting or materializing directory listings.
Its depth-first traversal retains one nonrecursive Qt iterator per ancestor and
emits each directory before its children, including empty directories, so
upload batches can resolve parent-task dependencies. Enumeration takes O(K)
work for K directory entries; pending traversal state is bounded by depth and
the configured batch size rather than directory width. Batch acknowledgement,
queue backpressure and large-tree confirmation pause enumeration. The depth
limit checks only whether a first entry exists, using the same hidden/system
filters; links are never followed and the existing omission counters remain.
Sibling order follows the filesystem and is not guaranteed.

Models store results; they do not fetch them. Dialogs collect input; they do not
call a remote client. Host-key prompts, health checks, and connection timing are
kept separate from blocking network operations.

Each initial connection has a runtime-only stop token owned by
`SessionController`. `ClientFactory` interrupts the pending client when it is
stopped and unregisters that callback before releasing the client. SSH also
registers interruption for direct backend connections, publishes its socket
under the state mutex before proxy negotiation, and uses nonblocking TCP
connects with a shared 20-second deadline across resolved addresses. The
platform's synchronous DNS resolver remains outside that TCP deadline.
libssh2's blocking handshake/authentication timeout is set explicitly. That
20-second timeout also covers each blocking SFTP write, so an upload passes
about two seconds of data per write at the rate it has reached, from 32 KiB up
to its 2 MiB buffer.
SSH interruption and the initial disconnect shutdown hold the state mutex
through the socket syscall, preventing a concurrent disconnect from closing
and reusing the descriptor. Disconnect releases that mutex before waiting for
the I/O mutex, so shutdown can unblock an active operation without reversing
the I/O-to-state lock order. Session and socket ownership are then detached
under both locks; only the caller that detaches them performs cleanup.

FTPS and HTTPS WebDAV connections that skip certificate verification pin the
server's TLS public key. Before the control connection, the connection worker
reads the key through `FetchServerPublicKeyPin`, a handshake without
credentials, and `TlsKeyTrust` compares it with the pin remembered for that
protocol, host and port. A new or changed key goes through the host-key prompt;
once trusted, the session's options carry the pin, so libcurl refuses any other
key on the control, worker and reopened connections. The pin is remembered
after the connection succeeds.

Keyboard-interactive prompts have one classifier, `classifyKbdIntPrompt`, which
uses whole words and the prompt's echo flag. The SSH backend answers the account
name and, in the first round only, the account password; every other prompt,
one-time codes included, goes to the connection's prompt callback, which asks
the user with the server's echo setting. Connections without that callback,
such as transfer workers, fail on those prompts instead of guessing.

`AuthenticationInput` opens a window-modal input dialog without a nested event loop and
waits on shared reply state with the connection stop token. Pending and visible
prompts can be canceled without the worker requiring UI event dispatch; a
visible dialog closes on cancellation. Replies use `SecureString` until copied
to the existing authentication callback. Host-key decisions use the same stop
token, including requests arriving after cancellation. The main window cancels
its connection before joining workers; queued connection results retain their
client ownership until delivered or discarded and recheck cancellation before
establishing a session.

## Transfers and synchronization

`TransferManager` is the entry point for the transfer queue. It coordinates
worker connections, task state, retries, conflicts, path reservations,
persistence, and notifications. `enqueueTasks` adds many tasks under one lock
with one `tasksAdded` signal and one worker wake-up; a request can depend on an
earlier request of the same call. Synchronization plans, local discovery
batches and remote prescan batches enter the queue through it. Before dispatching a download or upload, it
reserves both the final destination and its deterministic `.part` path under
the queue mutex; both remain reserved through publication and are released
together. Conflict renaming checks and reserves the same pair. Local reservation
keys conservatively normalize Unicode and fold case on every volume, including
for upload sources; this may serialize case-distinct local names on a
case-sensitive volume. Remote keys retain case. Partial-file cleanup applies
only to uploads and downloads that have not finished their transfer. For a
removed download it acquires the same reservations and leaves the file intact
while another task, queued or running, can still use it. A removed upload
queues its remote cleanup unless another upload of the session targets the
same destination or the session uses classic SCP, which writes the
destination directly. For a local upload move, it defers source
cleanup while another queued task needs that path and reserves the source
during cleanup. A runtime local-path index shares the reservation keys and
tracks upload sources plus local destinations and download partial paths. It
is maintained under the queue mutex on insertion, removal, restore, conflict
renaming and centralized phase changes. Only transfer-phase tasks are indexed;
status and active ownership are checked when consulting it, so canceled tasks
still in flight remain protected and status retries need no reindexing. Cleanup
examines only users of its source path and memoizes dependency ancestry within
each query to avoid repeatedly walking shared chains. After normalizing the
source path, a query takes expected O(C + A) work for C indexed candidates and
A distinct ancestors. Direct and transitive dependents of the move and waiters
in its own batch are excluded to prevent deadlocks. It keeps
pending and failed task counts per batch so worker selection does not rescan a
batch for each waiting task. Reverse dependency and batch-waiter indexes are
maintained under the same mutex on insertion, removal and restore. Failure
propagation visits only indexed dependents, expands each affected batch once,
and marks a task skipped before visiting its dependents to
avoid duplicate work and cycles. Active and terminal tasks stop that branch of
propagation. Relationships stay indexed until removal, independently of task
status, so retries retain them. Retrying a task walks the same indexes the
other way: each task skipped for it comes back once none of its other
prerequisites or batch tasks still fails, and a skipped task counts as a
failure, not as completed work. The indexes are runtime-only and do not change
the persisted format.

A worker that cannot open a connection returns its task to the queue without
spending its attempts. If other workers are connected, the server is taken to
accept fewer connections, and that worker stays idle until the session
changes. If none is, three failures in a row stop dispatching: pending tasks
of the session wait for a connection until they are resumed or the session
connects again, and the main window reports it. Disconnecting also leaves the
session's queued and running work waiting; a new connection factory for the
current session, which reconnecting to the same site sets without changing the
session key, queues that work again. Switching sessions leaves paused tasks
paused. The connection factory gets a stop token of the manager's that
clearing or replacing the session, or shutting down, requests, so a worker
still connecting gives up instead of making them wait. The supporting classes
have narrower jobs:

- `TransferQueue` stores tasks and selects work fairly.
- `TransferExecutor` runs a selected transfer.
- `BandwidthLimiter` applies speed limits.
- `TransferQueuePersistence` saves unfinished tasks safely. A queue too large
  to save removes the previous file, so a stale queue is never restored, and
  `TransferQueueWriter` reports a failed save once until a save succeeds.

The transfer queue dialog keeps a separate table model indexed by task ID.
Contiguous removals use Qt row-removal notifications. Discontinuous removals
use a single layout change, compact the task vector once in survivor order,
and rebuild the ID index and summary counts before notifying completion.
Persistent indexes are captured after the initial layout notification and
remapped by task ID, invalidating removed tasks while preserving surviving
selection and current-task identity through the filter proxy. For R requested
task IDs, N tasks and P persistent indexes, model work is expected
O(R log R + N + P), with O(R + P) temporary storage; Qt proxy/view work is
additional. No model reset is needed for either removal path.

`SafeLocalFile` in the core owns local download publication and descriptor-based
path operations for transfer tasks. The queue uses it to create or remove local
entries without following symbolic links in user-writable parent directories.
On POSIX, partial-file writers open with `O_NONBLOCK` so a FIFO cannot wait
for a reader before type validation. They check the opened descriptor's type,
ownership, and link count, then restore blocking mode before modifying the
file or creating its stdio stream; append and close-on-exec flags are retained.
Upload moves also use it to capture local file identity before transfer and
inspect the source during cleanup. Existing local sources are preserved:
POSIX provides no atomic identity-conditional unlink. The completed upload
retains its `DeleteSource` phase with a warning for manual removal; retrying
after removal finishes cleanup without uploading again. The identity is
process-local; a restored cleanup without it requires manual review.
`SyncCoordinator` resolves the selected local root before scanning and queuing
paths, so a root reached through a system link keeps one physical location.

There is one allowed nested lock order inside the manager:

```text
connFactoryMutex_ -> mtx_
```

Release manager locks before calling a remote client, invoking a callback, or
emitting a Qt signal.

The automatic cleanup of finished tasks belongs to `TransferManager`, which
applies the rule from `TransferAutoClear` every minute; the transfer queue
window only edits that rule, so the cleanup also runs while it stays closed.

Dragging remote items out of the window first downloads them into a batch
folder under the staging root. `StagingStore` names the batches, checks one
before it is deleted, and deletes them. Only a drop of that drag on the local
panel deletes its batch, once its copy finishes without errors and only if
`stagingBatchPathForCleanup()` confirms it is a batch folder in the staging
root. Any other target, such as another application, may read the files long
after the drop, so its batch stays; the startup cleanup removes batches older
than the retention setting. Both deletions, and the removal of a staging root
left empty, run outside the UI thread.

Copies, moves and deletions between local folders run on a pool of two
threads that `MainWindow` owns, apart from the global pool the connection and
the disconnect cleanup use. They stop at the next entry when canceled,
removing what they staged; closing the window while one runs asks first.

Synchronization has three steps:

1. `SyncCoordinator` collects bounded local and remote snapshots.
2. `SyncComparisonEngine` creates a data-only plan.
3. The accepted actions enter `TransferManager` as one ordered batch, in one
   `enqueueTasks` call.

When the scans pass 100,000 items or 100 GiB of known data, the coordinator
pauses both, the remote traversal through `RemoteOperationController::setPaused`
and the local scan on a condition variable, and asks before going on.
Accepting resumes them with what they already found; declining cancels them.

`SyncDialog` cancels superseded comparisons when options or snapshots change.
The engine checks cancellation while building and classifying the preview; the
dialog applies only the newest completed result.

The coordinator records paths that either scan could not verify. The comparison
keeps destination-only entries under unscanned source paths, and plan creation
checks that coverage again before accepting mirror deletions. Both scans mark
FIFOs, sockets and devices as entries that are never copied or deleted, and the
local scan leaves out the names that uploads refuse, as the remote scan does.
An entry present on one side only whose name matches the other side except for
letter case or Unicode normalization is a conflict, because many destinations
treat both names as one entry.

## Application updates

`UpdateController` owns bounded, asynchronous HTTPS checks against the latest
stable GitHub release. Checks are manual or explicitly enabled by the user;
automatic checks run at most once a day. Update preferences and skipped versions
use the centralized `AppSettings` keys. No credentials or saved-site data enter
update requests. Drafts, prereleases and downgrades are excluded.

`UpdateMetadata` verifies AppImage manifests with an embedded Ed25519 public key
before accepting a version, architecture, size, hash and minimum glibc baseline.
`AppImageInstaller` copies and hashes the download through descriptors, locks the
installation, preserves the previous inode, syncs staged data and atomically
publishes the replacement. The worker belongs to `UpdateController`; destruction
joins it before releasing its temporary download. No elevated helper is used.

`MacUpdater` is the optional Objective-C++ bridge to Sparkle. Sparkle owns signed
feeds, archive verification, replacement and relaunch. `MainWindowUpdates`
owns prompts and the idle-work gate, persists state before installation, blocks
new file operations during the installation flow and waits for connection
cleanup before a native relaunch. AppImage users restart explicitly. Flatpak
and Snap installations delegate replacement to their package manager; other
builds open the release page. Missing signing configuration permits manual
downloads only. See [UPDATES.md](UPDATES.md) for user-facing update instructions.

## Application icon

`AppIcon` owns the icon styles, the platform default and the embedded artwork.
`SettingsDialog` saves the chosen style under its `AppSettings` key, and
`MainWindow` applies it to the running application in `applyPreferences`; the
About dialog shows the application's current icon. The installed icons are
generated by `scripts/icons` from the same drawing and keep the platform
default, since a running application cannot change them. See
[BUILDING.md](BUILDING.md#application-icon).

## Saved data

Each kind of persisted data has one owner:

| Owner | Data |
| --- | --- |
| `AppSettings` | Application identity, settings keys, and defaults that several readers share. |
| `SavedSitesPersistence` | Current saved-site format and validation. |
| `SiteCredentialRepository` | Stable credential keys and secure storage. |
| `SecretStore` | Keychain, Secret Service/libsecret, and platform encryption. |
| `NavigationStore` | Local history and session-scoped remote history and favorites. |
| `TransferQueuePersistence` | Versioned, atomic storage for unfinished transfers. |

Use these classes instead of reading or writing their settings directly.
`SettingsDialog` writes only the values the user changed, so every other
setting keeps following its default. Saving an edited site removes the
credentials whose fields are empty, except those that could not be read when
the editor opened.
Changes to persisted formats require explicit decisions about schema and
versioning, and corrupt or unsupported data must not be silently overwritten.

Saved sites record their format version under `Sites/formatVersion`. A
versioned record that lacks fields takes their defaults, while incomplete
records in a store without the version come from releases before 1.0 and are
discarded. Records with a protocol this version does not know stay hidden, and
saving keeps them unchanged.

Saving rewrites the whole saved-site list, so the Site Manager applies each
change to the list as stored at that moment rather than to the copy it loaded.
The main window keeps one Site Manager open at a time and reloads it after
saving a site.

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
