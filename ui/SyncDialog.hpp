// One-way directory comparison and synchronization preview.
#pragma once

#include "SyncComparisonEngine.hpp"
#include "SyncTypes.hpp"

#include <QDialog>
#include <QString>
#include <QVector>

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
