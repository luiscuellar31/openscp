// One-way directory comparison, filtering, preview, and execution planning.
#include "SyncDialog.hpp"
#include "UiFormatters.hpp"

#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <functional>
#include <utility>

namespace {

QString actionText(SyncAction action) {
    switch (action) {
    case SyncAction::Keep:
        return QCoreApplication::translate("SyncDialog", "Keep");
    case SyncAction::Copy:
        return QCoreApplication::translate("SyncDialog", "Copy");
    case SyncAction::CreateDirectory:
        return QCoreApplication::translate("SyncDialog", "Create folder");
    case SyncAction::DeleteFile:
        return QCoreApplication::translate("SyncDialog", "Delete file");
    case SyncAction::DeleteDirectory:
        return QCoreApplication::translate("SyncDialog", "Delete folder");
    case SyncAction::Conflict:
        return QCoreApplication::translate("SyncDialog", "Conflict");
    case SyncAction::Unknown:
        return QCoreApplication::translate("SyncDialog", "Unknown");
    }
    return {};
}

QString entryTypeText(SyncEntryType type) {
    switch (type) {
    case SyncEntryType::File:
        return QCoreApplication::translate("SyncDialog", "File");
    case SyncEntryType::Directory:
        return QCoreApplication::translate("SyncDialog", "Folder");
    case SyncEntryType::SymbolicLink:
        return QCoreApplication::translate("SyncDialog", "Symbolic link");
    case SyncEntryType::Other:
        return QCoreApplication::translate("SyncDialog", "Other");
    }
    return {};
}

QString entryMetadataText(const std::optional<SyncSnapshotEntry> &entry) {
    if (!entry)
        return QString::fromUtf8("—");

    QStringList details{entryTypeText(entry->type)};
    if (entry->type == SyncEntryType::File) {
        details.push_back(entry->size ? formatByteSize(*entry->size)
                                      : QCoreApplication::translate("SyncDialog", "unknown size"));
    }
    if (entry->modifiedMs) {
        details.push_back(QDateTime::fromMSecsSinceEpoch(*entry->modifiedMs)
                              .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    } else if (entry->type == SyncEntryType::File) {
        details.push_back(QCoreApplication::translate("SyncDialog", "unknown date"));
    }
    if (!entry->metadataReliable)
        details.push_back(QCoreApplication::translate("SyncDialog", "unreliable metadata"));
    if (!entry->checksumAlgorithm.isEmpty() && !entry->checksum.isEmpty())
        details.push_back(QCoreApplication::translate("SyncDialog", "checksum available"));
    return details.join(QStringLiteral(" · "));
}

QString selectionKey(const SyncComparisonItem &item) {
    return item.relativePath + QChar(0x1f) +
           QString::number(static_cast<int>(item.action));
}

} // namespace

class SyncComparisonTableModel final : public QAbstractTableModel {
    public:
    enum Column {
        PathColumn = 0,
        SourceColumn = 1,
        DestinationColumn = 2,
        ActionColumn = 3,
        ReasonColumn = 4,
        ColumnCount = 5,
    };

    explicit SyncComparisonTableModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex &parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : items_.size();
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : ColumnCount;
    }

    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override {
        if (!index.isValid() || index.row() < 0 ||
            index.row() >= items_.size()) {
            return {};
        }
        const SyncComparisonItem &item = items_.at(index.row());

        if (role == Qt::CheckStateRole && index.column() == ActionColumn &&
            isSyncActionable(item.action)) {
            return item.selected ? Qt::Checked : Qt::Unchecked;
        }
        if (role == Qt::DisplayRole) {
            switch (index.column()) {
            case PathColumn:
                return item.relativePath;
            case SourceColumn:
                return entryMetadataText(item.source);
            case DestinationColumn:
                return entryMetadataText(item.destination);
            case ActionColumn:
                return actionText(item.action);
            case ReasonColumn:
                return item.reason;
            default:
                return {};
            }
        }
        if (role == Qt::ToolTipRole) {
            return QStringLiteral("%1\n%2: %3\n%4: %5\n%6")
                .arg(item.relativePath, QCoreApplication::translate("SyncDialog", "Source"),
                     entryMetadataText(item.source), QCoreApplication::translate("SyncDialog", "Destination"),
                     entryMetadataText(item.destination), item.reason);
        }
        if (role == Qt::TextAlignmentRole && index.column() == ActionColumn) {
            return static_cast<int>(Qt::AlignCenter);
        }
        if (role == Qt::UserRole) {
            if (index.column() == ActionColumn)
                return static_cast<int>(item.action);
            return item.relativePath;
        }
        return {};
    }

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return QAbstractTableModel::headerData(section, orientation, role);
        switch (section) {
        case PathColumn:
            return QCoreApplication::translate("SyncDialog", "Relative path");
        case SourceColumn:
            return QCoreApplication::translate("SyncDialog", "Source metadata");
        case DestinationColumn:
            return QCoreApplication::translate("SyncDialog", "Destination metadata");
        case ActionColumn:
            return QCoreApplication::translate("SyncDialog", "Action");
        case ReasonColumn:
            return QCoreApplication::translate("SyncDialog", "Reason");
        default:
            return {};
        }
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override {
        Qt::ItemFlags result = QAbstractTableModel::flags(index);
        if (index.isValid() && index.column() == ActionColumn &&
            isSyncActionable(items_.at(index.row()).action)) {
            result |= Qt::ItemIsUserCheckable;
        }
        return result;
    }

    bool setData(const QModelIndex &index, const QVariant &value,
                 int role = Qt::EditRole) override {
        if (!index.isValid() || index.row() < 0 ||
            index.row() >= items_.size() || index.column() != ActionColumn ||
            role != Qt::CheckStateRole ||
            !isSyncActionable(items_.at(index.row()).action)) {
            return false;
        }

        SyncComparisonItem &item = items_[index.row()];
        const bool selected =
            static_cast<Qt::CheckState>(value.toInt()) == Qt::Checked;
        if (item.selected == selected)
            return true;
        item.selected = selected;
        selectionOverrides_.insert(selectionKey(item), selected);
        emit dataChanged(index, index, {Qt::CheckStateRole, Qt::DisplayRole});
        if (selectionChanged_)
            selectionChanged_();
        return true;
    }

    void setItems(QVector<SyncComparisonItem> items) {
        for (SyncComparisonItem &item : items) {
            if (!isSyncActionable(item.action))
                continue;
            const auto override =
                selectionOverrides_.constFind(selectionKey(item));
            if (override != selectionOverrides_.cend())
                item.selected = *override;
        }

        beginResetModel();
        items_ = std::move(items);
        endResetModel();
        if (selectionChanged_)
            selectionChanged_();
    }

    const QVector<SyncComparisonItem> &items() const { return items_; }

    void setAllActionable(bool selected) {
        if (items_.isEmpty())
            return;
        bool changed = false;
        for (SyncComparisonItem &item : items_) {
            if (!isSyncActionable(item.action) || item.selected == selected)
                continue;
            item.selected = selected;
            selectionOverrides_.insert(selectionKey(item), selected);
            changed = true;
        }
        if (!changed)
            return;
        emit dataChanged(index(0, ActionColumn),
                         index(items_.size() - 1, ActionColumn),
                         {Qt::CheckStateRole});
        if (selectionChanged_)
            selectionChanged_();
    }

    void setSelectionChangedCallback(std::function<void()> callback) {
        selectionChanged_ = std::move(callback);
    }

    private:
    QVector<SyncComparisonItem> items_;
    QHash<QString, bool> selectionOverrides_;
    std::function<void()> selectionChanged_;
};

SyncDialog::SyncDialog(QWidget *parent) : QDialog(parent) {
    qRegisterMetaType<SyncExecutionPlan>("SyncExecutionPlan");
    buildUi();
    loadPresets();
    applyOptionsToControls();
    rebuildComparison();
}

void SyncDialog::buildUi() {
    setWindowTitle(tr("Compare and synchronize directories"));
    resize(1120, 680);

    auto *root = new QVBoxLayout(this);

    auto *directionGroup = new QGroupBox(tr("Direction"), this);
    auto *directionLayout = new QFormLayout(directionGroup);
    directionCombo_ = new QComboBox(directionGroup);
    directionCombo_->addItem(tr("Local → Remote"),
                             static_cast<int>(SyncDirection::LocalToRemote));
    directionCombo_->addItem(tr("Remote → Local"),
                             static_cast<int>(SyncDirection::RemoteToLocal));
    directionLayout->addRow(tr("Copy direction"), directionCombo_);
    sourceRootLabel_ = new QLabel(directionGroup);
    sourceRootLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    destinationRootLabel_ = new QLabel(directionGroup);
    destinationRootLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    directionLayout->addRow(tr("Source"), sourceRootLabel_);
    directionLayout->addRow(tr("Destination"), destinationRootLabel_);
    root->addWidget(directionGroup);

    auto *filterGroup = new QGroupBox(tr("Filters"), this);
    auto *filterLayout = new QFormLayout(filterGroup);
    auto *presetRow = new QWidget(filterGroup);
    auto *presetLayout = new QHBoxLayout(presetRow);
    presetLayout->setContentsMargins(0, 0, 0, 0);
    presetCombo_ = new QComboBox(presetRow);
    savePresetButton_ = new QPushButton(tr("Save preset…"), presetRow);
    deletePresetButton_ = new QPushButton(tr("Delete preset"), presetRow);
    presetLayout->addWidget(presetCombo_, 1);
    presetLayout->addWidget(savePresetButton_);
    presetLayout->addWidget(deletePresetButton_);
    filterLayout->addRow(tr("Preset"), presetRow);

    includeEdit_ = new QLineEdit(filterGroup);
    includeEdit_->setPlaceholderText(tr("For example: **/*.cpp; assets/**"));
    includeEdit_->setToolTip(
        tr("Semicolon-separated glob patterns. *, ? and ** are supported."));
    filterLayout->addRow(tr("Include"), includeEdit_);

    excludeEdit_ = new QPlainTextEdit(filterGroup);
    excludeEdit_->setMaximumHeight(72);
    excludeEdit_->setPlaceholderText(
        tr("One glob per line, or separate patterns with semicolons"));
    excludeEdit_->setToolTip(
        tr("Matching files, folders, and their descendants are omitted."));
    filterLayout->addRow(tr("Exclude"), excludeEdit_);

    auto *filterFlags = new QWidget(filterGroup);
    auto *filterFlagsLayout = new QHBoxLayout(filterFlags);
    filterFlagsLayout->setContentsMargins(0, 0, 0, 0);
    includeHiddenCheck_ =
        new QCheckBox(tr("Include hidden files"), filterFlags);
    mirrorCheck_ = new QCheckBox(
        tr("Mirror destination (preview extra-item deletions)"), filterFlags);
    mirrorCheck_->setToolTip(
        tr("Destination-only items are kept unless mirror is enabled. "
           "Executing mirror deletions always requires confirmation."));
    filterFlagsLayout->addWidget(includeHiddenCheck_);
    filterFlagsLayout->addWidget(mirrorCheck_);
    filterFlagsLayout->addStretch(1);
    filterLayout->addRow(QString(), filterFlags);
    root->addWidget(filterGroup);

    auto *previewActions = new QHBoxLayout;
    auto *selectAllButton = new QPushButton(tr("Select all changes"), this);
    auto *selectNoneButton = new QPushButton(tr("Select none"), this);
    checksumButton_ = new QPushButton(tr("Compare checksums…"), this);
    checksumButton_->setVisible(false);
    checksumButton_->setToolTip(
        tr("Calculate checksums on demand for selected files."));
    previewActions->addWidget(selectAllButton);
    previewActions->addWidget(selectNoneButton);
    previewActions->addWidget(checksumButton_);
    previewActions->addStretch(1);
    summaryLabel_ = new QLabel(this);
    summaryLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    previewActions->addWidget(summaryLabel_);
    root->addLayout(previewActions);

    comparisonModel_ = new SyncComparisonTableModel(this);
    sortProxy_ = new QSortFilterProxyModel(this);
    sortProxy_->setSourceModel(comparisonModel_);
    sortProxy_->setSortRole(Qt::UserRole);
    sortProxy_->setDynamicSortFilter(false);
    previewTable_ = new QTableView(this);
    previewTable_->setModel(sortProxy_);
    previewTable_->setAlternatingRowColors(true);
    previewTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    previewTable_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    previewTable_->setSortingEnabled(true);
    previewTable_->sortByColumn(SyncComparisonTableModel::PathColumn,
                                Qt::AscendingOrder);
    previewTable_->verticalHeader()->setVisible(false);
    previewTable_->horizontalHeader()->setStretchLastSection(true);
    previewTable_->horizontalHeader()->setSectionResizeMode(
        SyncComparisonTableModel::PathColumn, QHeaderView::Stretch);
    previewTable_->horizontalHeader()->setSectionResizeMode(
        SyncComparisonTableModel::SourceColumn, QHeaderView::ResizeToContents);
    previewTable_->horizontalHeader()->setSectionResizeMode(
        SyncComparisonTableModel::DestinationColumn,
        QHeaderView::ResizeToContents);
    previewTable_->horizontalHeader()->setSectionResizeMode(
        SyncComparisonTableModel::ActionColumn, QHeaderView::ResizeToContents);
    root->addWidget(previewTable_, 1);

    buttonBox_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonBox_->button(QDialogButtonBox::Ok)->setText(tr("Synchronize"));
    root->addWidget(buttonBox_);

    rebuildTimer_ = new QTimer(this);
    rebuildTimer_->setSingleShot(true);
    rebuildTimer_->setInterval(180);
    connect(rebuildTimer_, &QTimer::timeout, this,
            &SyncDialog::rebuildComparison);

    connect(directionCombo_, &QComboBox::currentIndexChanged, this,
            [this](int) {
                if (!applyingControls_)
                    scheduleRebuild();
                updateRootLabels();
            });
    connect(includeEdit_, &QLineEdit::textEdited, this,
            [this](const QString &) {
                markPresetAsCustom();
                scheduleRebuild();
            });
    connect(excludeEdit_, &QPlainTextEdit::textChanged, this, [this] {
        if (applyingControls_)
            return;
        markPresetAsCustom();
        scheduleRebuild();
    });
    connect(includeHiddenCheck_, &QCheckBox::toggled, this, [this](bool) {
        if (applyingControls_)
            return;
        markPresetAsCustom();
        scheduleRebuild();
    });
    connect(mirrorCheck_, &QCheckBox::toggled, this, [this](bool) {
        if (!applyingControls_)
            scheduleRebuild();
    });
    connect(presetCombo_, &QComboBox::activated, this,
            &SyncDialog::applySelectedPreset);
    connect(presetCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        deletePresetButton_->setEnabled(presetCombo_->currentIndex() > 0);
    });
    connect(savePresetButton_, &QPushButton::clicked, this,
            &SyncDialog::saveCurrentPreset);
    connect(deletePresetButton_, &QPushButton::clicked, this,
            &SyncDialog::deleteCurrentPreset);
    connect(selectAllButton, &QPushButton::clicked, this,
            [this] { selectAllActionable(true); });
    connect(selectNoneButton, &QPushButton::clicked, this,
            [this] { selectAllActionable(false); });
    connect(checksumButton_, &QPushButton::clicked, this,
            &SyncDialog::requestChecksums);
    connect(buttonBox_, &QDialogButtonBox::accepted, this,
            &SyncDialog::acceptRequested);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    comparisonModel_->setSelectionChangedCallback([this] { updateSummary(); });
}

void SyncDialog::setSnapshots(QVector<SyncSnapshotEntry> localSnapshot,
                              QVector<SyncSnapshotEntry> remoteSnapshot) {
    localSnapshot_ = std::move(localSnapshot);
    remoteSnapshot_ = std::move(remoteSnapshot);
    rebuildComparison();
}

void SyncDialog::setRootPaths(const QString &localRoot,
                              const QString &remoteRoot) {
    localRoot_ = localRoot;
    remoteRoot_ = remoteRoot;
    updateRootLabels();
}

void SyncDialog::setComparisonOptions(const SyncComparisonOptions &options) {
    options_ = options;
    if (options_.modifiedToleranceMs < 0)
        options_.modifiedToleranceMs = 0;
    applyOptionsToControls();
    rebuildComparison();
}

void SyncDialog::setChecksumAvailable(bool available) {
    checksumAvailable_ = available;
    checksumButton_->setVisible(available);
    checksumButton_->setEnabled(available && !checksumBusy_);
}

void SyncDialog::setChecksumBusy(bool busy) {
    checksumBusy_ = busy;
    checksumButton_->setEnabled(checksumAvailable_ && !busy);
    checksumButton_->setText(
        busy ? tr("Calculating checksums…") : tr("Compare checksums…"));
    if (buttonBox_ && buttonBox_->button(QDialogButtonBox::Ok))
        buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(!busy);
}

SyncComparisonOptions SyncDialog::comparisonOptions() const {
    SyncComparisonOptions current = options_;
    if (!directionCombo_)
        return current;

    current.direction =
        static_cast<SyncDirection>(directionCombo_->currentData().toInt());
    current.includePatterns =
        SyncComparisonEngine::parsePatterns(includeEdit_->text());
    if (current.includePatterns.isEmpty())
        current.includePatterns.push_back(QStringLiteral("**"));
    current.excludePatterns =
        SyncComparisonEngine::parsePatterns(excludeEdit_->toPlainText());
    current.includeHidden = includeHiddenCheck_->isChecked();
    current.mirror = mirrorCheck_->isChecked();
    return current;
}

QVector<SyncComparisonItem> SyncDialog::comparisonItems() const {
    return comparisonModel_ ? comparisonModel_->items()
                            : QVector<SyncComparisonItem>{};
}

SyncExecutionPlan SyncDialog::executionPlan() const {
    return SyncComparisonEngine::makeExecutionPlan(comparisonItems(),
                                                   comparisonOptions());
}

void SyncDialog::rebuildComparison() {
    syncOptionsFromControls();
    comparisonModel_->setItems(SyncComparisonEngine::compare(
        localSnapshot_, remoteSnapshot_, options_));
    updateRootLabels();
    updateSummary();
}

void SyncDialog::scheduleRebuild() {
    if (!applyingControls_ && rebuildTimer_)
        rebuildTimer_->start();
}

void SyncDialog::updateRootLabels() {
    if (!sourceRootLabel_ || !destinationRootLabel_ || !directionCombo_)
        return;
    const SyncDirection direction =
        static_cast<SyncDirection>(directionCombo_->currentData().toInt());
    const QString local =
        localRoot_.isEmpty() ? tr("(current local folder)") : localRoot_;
    const QString remote =
        remoteRoot_.isEmpty() ? tr("(current remote folder)") : remoteRoot_;
    const QString source =
        direction == SyncDirection::LocalToRemote ? local : remote;
    const QString destination =
        direction == SyncDirection::LocalToRemote ? remote : local;
    sourceRootLabel_->setText(source);
    sourceRootLabel_->setToolTip(source);
    destinationRootLabel_->setText(destination);
    destinationRootLabel_->setToolTip(destination);
}

void SyncDialog::updateSummary() {
    if (!summaryLabel_ || !comparisonModel_)
        return;

    qsizetype selectedChanges = 0;
    qsizetype selectedCopies = 0;
    qsizetype conflicts = 0;
    qsizetype unknown = 0;
    quint64 knownBytes = 0;
    qsizetype unknownSizes = 0;
    for (const SyncComparisonItem &item : comparisonModel_->items()) {
        if (item.action == SyncAction::Conflict)
            ++conflicts;
        else if (item.action == SyncAction::Unknown)
            ++unknown;
        if (!item.selected || !isSyncActionable(item.action))
            continue;
        ++selectedChanges;
        if (item.action != SyncAction::Copy)
            continue;
        ++selectedCopies;
        if (!item.source || !item.source->size) {
            ++unknownSizes;
        } else if (*item.source->size >
                   std::numeric_limits<quint64>::max() - knownBytes) {
            knownBytes = std::numeric_limits<quint64>::max();
        } else {
            knownBytes += *item.source->size;
        }
    }

    QString bytesText = formatByteSize(knownBytes);
    if (unknownSizes > 0)
        bytesText += tr(" + unknown sizes");
    summaryLabel_->setText(
        tr("%1 files · %2 · %3 selected operations · %4 conflicts · %5 "
           "unknown")
            .arg(selectedCopies)
            .arg(bytesText)
            .arg(selectedChanges)
            .arg(conflicts)
            .arg(unknown));
}

void SyncDialog::syncOptionsFromControls() { options_ = comparisonOptions(); }

void SyncDialog::applyOptionsToControls() {
    if (!directionCombo_)
        return;

    applyingControls_ = true;
    const int directionIndex =
        directionCombo_->findData(static_cast<int>(options_.direction));
    directionCombo_->setCurrentIndex(directionIndex >= 0 ? directionIndex : 0);
    QStringList includes = options_.includePatterns;
    if (includes.isEmpty())
        includes.push_back(QStringLiteral("**"));
    includeEdit_->setText(includes.join(QStringLiteral("; ")));
    excludeEdit_->setPlainText(
        options_.excludePatterns.join(QLatin1Char('\n')));
    includeHiddenCheck_->setChecked(options_.includeHidden);
    mirrorCheck_->setChecked(options_.mirror);
    applyingControls_ = false;
    updateRootLabels();
}

void SyncDialog::loadPresets() {
    applyingControls_ = true;
    presetCombo_->clear();
    presetCombo_->addItem(tr("Custom"));

    QSettings settings(QStringLiteral("OpenSCP"), QStringLiteral("OpenSCP"));
    const QVariantList presets =
        settings.value(QStringLiteral("SyncDialog/filterPresetsV1")).toList();
    for (const QVariant &value : presets) {
        const QVariantMap preset = value.toMap();
        const QString name = preset.value(QStringLiteral("name")).toString();
        if (!name.trimmed().isEmpty())
            presetCombo_->addItem(name, preset);
    }
    presetCombo_->setCurrentIndex(0);
    deletePresetButton_->setEnabled(false);
    applyingControls_ = false;
}

void SyncDialog::saveCurrentPreset() {
    bool accepted = false;
    QString defaultName;
    if (presetCombo_->currentIndex() > 0)
        defaultName = presetCombo_->currentText();
    const QString name =
        QInputDialog::getText(this, tr("Save filter preset"), tr("Preset name"),
                              QLineEdit::Normal, defaultName, &accepted)
            .trimmed();
    if (!accepted || name.isEmpty())
        return;

    QVariantMap saved;
    saved.insert(QStringLiteral("name"), name);
    saved.insert(QStringLiteral("include"), includeEdit_->text());
    saved.insert(QStringLiteral("exclude"), excludeEdit_->toPlainText());
    saved.insert(QStringLiteral("hidden"), includeHiddenCheck_->isChecked());

    QSettings settings(QStringLiteral("OpenSCP"), QStringLiteral("OpenSCP"));
    QVariantList presets =
        settings.value(QStringLiteral("SyncDialog/filterPresetsV1")).toList();
    bool replaced = false;
    for (QVariant &value : presets) {
        const QVariantMap existing = value.toMap();
        if (existing.value(QStringLiteral("name"))
                .toString()
                .compare(name, Qt::CaseInsensitive) == 0) {
            value = saved;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        presets.push_back(saved);
    settings.setValue(QStringLiteral("SyncDialog/filterPresetsV1"), presets);

    loadPresets();
    const int savedIndex = presetCombo_->findText(
        name, Qt::MatchFixedString | Qt::MatchCaseSensitive);
    if (savedIndex >= 0)
        presetCombo_->setCurrentIndex(savedIndex);
}

void SyncDialog::deleteCurrentPreset() {
    if (presetCombo_->currentIndex() <= 0)
        return;
    const QString name = presetCombo_->currentText();
    if (QMessageBox::question(this, tr("Delete filter preset"),
                              tr("Delete the preset “%1”?").arg(name),
                              QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    QSettings settings(QStringLiteral("OpenSCP"), QStringLiteral("OpenSCP"));
    QVariantList presets =
        settings.value(QStringLiteral("SyncDialog/filterPresetsV1")).toList();
    for (qsizetype index = presets.size(); index-- > 0;) {
        if (presets.at(index)
                .toMap()
                .value(QStringLiteral("name"))
                .toString()
                .compare(name, Qt::CaseInsensitive) == 0) {
            presets.removeAt(index);
        }
    }
    settings.setValue(QStringLiteral("SyncDialog/filterPresetsV1"), presets);
    loadPresets();
}

void SyncDialog::applySelectedPreset(int index) {
    if (index <= 0)
        return;
    const QVariantMap preset = presetCombo_->itemData(index).toMap();
    if (preset.isEmpty())
        return;

    applyingControls_ = true;
    includeEdit_->setText(
        preset.value(QStringLiteral("include"), QStringLiteral("**"))
            .toString());
    excludeEdit_->setPlainText(
        preset.value(QStringLiteral("exclude")).toString());
    includeHiddenCheck_->setChecked(
        preset.value(QStringLiteral("hidden"), false).toBool());
    applyingControls_ = false;
    rebuildComparison();
}

void SyncDialog::markPresetAsCustom() {
    if (!presetCombo_ || applyingControls_ ||
        presetCombo_->currentIndex() == 0) {
        return;
    }
    applyingControls_ = true;
    presetCombo_->setCurrentIndex(0);
    applyingControls_ = false;
}

void SyncDialog::selectAllActionable(bool selected) {
    comparisonModel_->setAllActionable(selected);
}

void SyncDialog::requestChecksums() {
    QSet<QString> paths;
    const QModelIndexList selectedRows =
        previewTable_->selectionModel()->selectedRows();
    for (const QModelIndex &proxyIndex : selectedRows) {
        const QModelIndex sourceIndex = sortProxy_->mapToSource(proxyIndex);
        if (!sourceIndex.isValid() ||
            sourceIndex.row() >= comparisonModel_->items().size()) {
            continue;
        }
        const SyncComparisonItem &item =
            comparisonModel_->items().at(sourceIndex.row());
        if (item.source && item.destination &&
            item.source->type == SyncEntryType::File &&
            item.destination->type == SyncEntryType::File) {
            paths.insert(item.relativePath);
        }
    }

    if (paths.isEmpty()) {
        for (const SyncComparisonItem &item : comparisonModel_->items()) {
            if (item.source && item.destination &&
                item.source->type == SyncEntryType::File &&
                item.destination->type == SyncEntryType::File) {
                paths.insert(item.relativePath);
            }
        }
    }
    if (!paths.isEmpty()) {
        QStringList sortedPaths = paths.values();
        std::sort(sortedPaths.begin(), sortedPaths.end());
        emit checksumRequested(sortedPaths);
    }
}

void SyncDialog::acceptRequested() {
    if (checksumBusy_)
        return;
    if (rebuildTimer_->isActive()) {
        rebuildTimer_->stop();
        rebuildComparison();
    }
    const SyncExecutionPlan plan = executionPlan();
    if (plan.empty()) {
        QMessageBox::information(
            this, tr("Nothing to synchronize"),
            tr("Select at least one proposed change before continuing."));
        return;
    }

    if (plan.requiresMirrorConfirmation) {
        QMessageBox confirmation(
            QMessageBox::Warning, tr("Confirm mirror deletions"),
            tr("Mirror will permanently delete %1 destination items shown "
               "in the preview. Copies do not protect deleted extras.")
                .arg(plan.deletes.size()),
            QMessageBox::NoButton, this);
        QPushButton *executeButton = confirmation.addButton(
            tr("Execute mirror"), QMessageBox::AcceptRole);
        QPushButton *cancelButton = confirmation.addButton(QMessageBox::Cancel);
        confirmation.setDefaultButton(cancelButton);
        confirmation.exec();
        if (confirmation.clickedButton() != executeButton)
            return;
    }

    emit executionRequested(plan);
    QDialog::accept();
}
