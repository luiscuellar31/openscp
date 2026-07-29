// One-way directory comparison and synchronization preview.
#pragma once

#include <QByteArray>
#include <QDialog>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSortFilterProxyModel;
class QTableView;
class QTimer;
class SyncComparisonTableModel;

enum class SyncDirection {
    LocalToRemote = 0,
    RemoteToLocal = 1,
};

enum class SyncEntryType {
    File = 0,
    Directory = 1,
    SymbolicLink = 2,
    Other = 3,
};

// A caller-provided, root-relative snapshot entry. modifiedMs is expressed in
// milliseconds since the Unix epoch. File size and mtime may be absent when a
// backend could not retrieve trustworthy metadata.
struct SyncSnapshotEntry {
    QString relativePath;
    SyncEntryType type = SyncEntryType::File;
    std::optional<quint64> size;
    std::optional<qint64> modifiedMs;
    bool metadataReliable = true;

    // Optional, on-demand checksum metadata. It is only compared when both
    // sides provide the same non-empty algorithm name.
    QString checksumAlgorithm;
    QByteArray checksum;
};

enum class SyncAction {
    Keep = 0,
    Copy = 1,
    CreateDirectory = 2,
    DeleteFile = 3,
    DeleteDirectory = 4,
    Conflict = 5,
    Unknown = 6,
};

struct SyncComparisonOptions {
    SyncDirection direction = SyncDirection::LocalToRemote;
    QStringList includePatterns{QStringLiteral("**")};
    QStringList excludePatterns;
    bool includeHidden = false;
    bool mirror = false;
    qint64 modifiedToleranceMs = 2000;
};

struct SyncComparisonItem {
    QString relativePath;
    std::optional<SyncSnapshotEntry> source;
    std::optional<SyncSnapshotEntry> destination;
    SyncAction action = SyncAction::Keep;
    QString reason;
    bool selected = false;
};

struct SyncCopyOperation {
    QString relativePath;
    std::optional<quint64> size;
    bool overwritesExisting = false;
};

struct SyncDeleteOperation {
    QString relativePath;
    SyncEntryType type = SyncEntryType::File;
};

// This plan deliberately contains root-relative operations only. The caller
// resolves them against the roots tied to the current session, then enqueues
// them as one persistent transfer batch.
struct SyncExecutionPlan {
    SyncDirection direction = SyncDirection::LocalToRemote;
    bool mirror = false;
    // Execute in field order. Folder creation is shallow-first and deletions
    // are already sorted in postorder (children before parent directories).
    QVector<QString> directoriesToCreate;
    QVector<SyncCopyOperation> copies;
    QVector<SyncDeleteOperation> deletes;
    quint64 knownCopyBytes = 0;
    qsizetype unknownSizeCopies = 0;
    bool requiresMirrorConfirmation = false;
    QStringList warnings;

    [[nodiscard]] bool empty() const {
        return directoriesToCreate.isEmpty() && copies.isEmpty() &&
               deletes.isEmpty();
    }
};

Q_DECLARE_METATYPE(SyncExecutionPlan)

// UI-independent comparison helpers. Keeping this logic separate lets tests
// cover filtering and planning without creating a QApplication.
class SyncComparisonEngine {
    public:
    [[nodiscard]] static QVector<SyncComparisonItem>
    compare(const QVector<SyncSnapshotEntry> &localSnapshot,
            const QVector<SyncSnapshotEntry> &remoteSnapshot,
            const SyncComparisonOptions &options = {});

    [[nodiscard]] static SyncExecutionPlan
    makeExecutionPlan(const QVector<SyncComparisonItem> &items,
                      const SyncComparisonOptions &options);

    [[nodiscard]] static QString
    normalizeRelativePath(const QString &relativePath);
    [[nodiscard]] static QStringList parsePatterns(const QString &text);
    [[nodiscard]] static bool globMatches(const QString &relativePath,
                                          const QString &pattern);
};

class SyncDialog final : public QDialog {
    Q_OBJECT

    public:
    explicit SyncDialog(QWidget *parent = nullptr);

    void setSnapshots(QVector<SyncSnapshotEntry> localSnapshot,
                      QVector<SyncSnapshotEntry> remoteSnapshot);
    void setRootPaths(const QString &localRoot, const QString &remoteRoot);
    void setComparisonOptions(const SyncComparisonOptions &options);
    void setChecksumAvailable(bool available);
    void setChecksumBusy(bool busy);

    [[nodiscard]] SyncComparisonOptions comparisonOptions() const;
    [[nodiscard]] QVector<SyncComparisonItem> comparisonItems() const;
    [[nodiscard]] SyncExecutionPlan executionPlan() const;

    signals:
    // Emitted immediately before the dialog is accepted. The receiver can
    // enqueue the plan or simply call executionPlan() after exec().
    void executionRequested(const SyncExecutionPlan &plan);

    // The controller may calculate these asynchronously, update the supplied
    // snapshots with checksum metadata, and call setSnapshots() again.
    void checksumRequested(const QStringList &relativePaths);

    private:
    void buildUi();
    void rebuildComparison();
    void scheduleRebuild();
    void updateRootLabels();
    void updateSummary();
    void syncOptionsFromControls();
    void applyOptionsToControls();
    void loadPresets();
    void saveCurrentPreset();
    void deleteCurrentPreset();
    void applySelectedPreset(int index);
    void markPresetAsCustom();
    void selectAllActionable(bool selected);
    void requestChecksums();
    void acceptRequested();

    QVector<SyncSnapshotEntry> localSnapshot_;
    QVector<SyncSnapshotEntry> remoteSnapshot_;
    SyncComparisonOptions options_;
    QString localRoot_;
    QString remoteRoot_;
    bool applyingControls_ = false;
    bool checksumAvailable_ = false;
    bool checksumBusy_ = false;

    QComboBox *directionCombo_ = nullptr;
    QLabel *sourceRootLabel_ = nullptr;
    QLabel *destinationRootLabel_ = nullptr;
    QComboBox *presetCombo_ = nullptr;
    QLineEdit *includeEdit_ = nullptr;
    QPlainTextEdit *excludeEdit_ = nullptr;
    QCheckBox *includeHiddenCheck_ = nullptr;
    QCheckBox *mirrorCheck_ = nullptr;
    QPushButton *savePresetButton_ = nullptr;
    QPushButton *deletePresetButton_ = nullptr;
    QPushButton *checksumButton_ = nullptr;
    QDialogButtonBox *buttonBox_ = nullptr;
    QLabel *summaryLabel_ = nullptr;
    QTableView *previewTable_ = nullptr;
    SyncComparisonTableModel *comparisonModel_ = nullptr;
    QSortFilterProxyModel *sortProxy_ = nullptr;
    QTimer *rebuildTimer_ = nullptr;
};
