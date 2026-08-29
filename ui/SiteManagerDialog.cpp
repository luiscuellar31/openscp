// Manages saved sites and delegates credential storage to its repository.
#include "SiteManagerDialog.hpp"

#include "ConnectionDialog.hpp"
#include "SavedSitesPersistence.hpp"
#include "SiteCredentialRepository.hpp"
#include "UiAlerts.hpp"
#include "openscp/KnownHostsUtils.hpp"

#include <QAbstractTableModel>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QUuid>
#include <QVBoxLayout>

class SiteListModel final : public QAbstractTableModel {
    public:
    explicit SiteListModel(const QVector<SiteEntry> *sites,
                           QObject *parent = nullptr)
        : QAbstractTableModel(parent), sites_(sites) {}

    int rowCount(const QModelIndex &parent = QModelIndex()) const override {
        return parent.isValid() || !sites_ ? 0 : sites_->size();
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : 4;
    }

    QVariant data(const QModelIndex &index,
                  int role = Qt::DisplayRole) const override {
        if (!index.isValid() || !sites_ || index.row() < 0 ||
            index.row() >= sites_->size()) {
            return {};
        }
        const SiteEntry &site = sites_->at(index.row());
        QString text;
        switch (index.column()) {
        case 0:
            text = site.name;
            break;
        case 1:
            text = QString::fromLatin1(
                openscp::protocolDisplayName(site.opt.protocol));
            break;
        case 2:
            text = QString::fromStdString(site.opt.host);
            break;
        case 3:
            text = QString::fromStdString(site.opt.username);
            break;
        default:
            return {};
        }
        if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
            return text;
        if (role == Qt::UserRole)
            return index.row();
        return {};
    }

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        switch (section) {
        case 0:
            return QObject::tr("Name");
        case 1:
            return QObject::tr("Protocol");
        case 2:
            return QObject::tr("Host");
        case 3:
            return QObject::tr("User");
        default:
            return {};
        }
    }

    void reload() {
        beginResetModel();
        endResetModel();
    }

    private:
    const QVector<SiteEntry> *sites_ = nullptr;
};

static void showPersistIssues(QWidget *parent, const QStringList &issues) {
    if (issues.isEmpty())
        return;
    UiAlerts::warning(parent, QObject::tr("Credentials not saved"),
                      QObject::tr("Could not save one or more credentials "
                                  "in the secure backend:\n%1")
                          .arg(issues.join("\n")));
}

static QString normalizedSiteName(const QString &name) {
    return name.trimmed();
}

static bool hasDuplicateSiteName(const QVector<SiteEntry> &sites,
                                 const QString &candidate,
                                 int ignoreIndex = -1) {
    const QString normalizedCandidate = normalizedSiteName(candidate);
    if (normalizedCandidate.isEmpty())
        return false;
    for (int siteIndex = 0; siteIndex < sites.size(); ++siteIndex) {
        if (siteIndex == ignoreIndex)
            continue;
        if (normalizedSiteName(sites[siteIndex].name)
                .compare(normalizedCandidate, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

static void showDuplicateNameIssue(QWidget *parent, const QString &name) {
    UiAlerts::warning(
        parent, QObject::tr("Duplicate name"),
        QObject::tr("A site named \"%1\" already exists. Use a different name.")
            .arg(name));
}

static void showMissingNameIssue(QWidget *parent) {
    UiAlerts::warning(
        parent, QObject::tr("Name required"),
        QObject::tr("Enter a site name to save this connection."));
}

static QString newSiteId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

SiteManagerDialog::SiteManagerDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Site Manager"));
    resize(720, 480); // compact default; view will elide/scroll as needed
    auto *mainLayout = new QVBoxLayout(this);

    search_ = new QLineEdit(this);
    search_->setClearButtonEnabled(true);
    search_->setPlaceholderText(tr("Search by name, protocol, host, or user…"));
    search_->setAccessibleName(tr("Search saved sites"));
    mainLayout->addWidget(search_);

    model_ = new SiteListModel(&sites_, this);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterKeyColumn(-1);
    proxy_->setSortCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setDynamicSortFilter(true);

    table_ = new QTableView(this);
    table_->setModel(proxy_);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table_->horizontalHeader()->setMinimumSectionSize(80);
    // Elide long text on the right to avoid oversized cells
    table_->setTextElideMode(Qt::ElideRight);
    table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    table_->setWordWrap(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setSortingEnabled(true);
    table_->sortByColumn(0, Qt::AscendingOrder);
    mainLayout->addWidget(table_);

    auto *dialogButtons = new QDialogButtonBox(this);
    btAdd_ = dialogButtons->addButton(tr("Add"), QDialogButtonBox::ActionRole);
    btEdit_ =
        dialogButtons->addButton(tr("Edit"), QDialogButtonBox::ActionRole);
    btDuplicate_ =
        dialogButtons->addButton(tr("Duplicate"), QDialogButtonBox::ActionRole);
    btDel_ =
        dialogButtons->addButton(tr("Delete"), QDialogButtonBox::ActionRole);
    btConn_ =
        dialogButtons->addButton(tr("Connect"), QDialogButtonBox::AcceptRole);
    btClose_ = dialogButtons->addButton(QDialogButtonBox::Close);
    if (btClose_)
        btClose_->setText(tr("Close"));
    mainLayout->addWidget(dialogButtons);
    connect(btAdd_, &QPushButton::clicked, this, &SiteManagerDialog::onAdd);
    connect(btEdit_, &QPushButton::clicked, this, &SiteManagerDialog::onEdit);
    connect(btDuplicate_, &QPushButton::clicked, this,
            &SiteManagerDialog::onDuplicate);
    connect(btDel_, &QPushButton::clicked, this, &SiteManagerDialog::onRemove);
    connect(btConn_, &QPushButton::clicked, this,
            &SiteManagerDialog::onConnect);
    connect(dialogButtons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    loadSites();
    refresh();

    // Initial state: disable Edit/Delete/Connect if there is no selection
    updateButtons();
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &SiteManagerDialog::updateButtons);
    connect(search_, &QLineEdit::textChanged, proxy_,
            &QSortFilterProxyModel::setFilterFixedString);
    // Double-click: primary action (connect) for faster workflow.
    connect(table_, &QTableView::doubleClicked, this,
            [this](const QModelIndex &index) {
                if (index.isValid())
                    onConnect();
            });
}

void SiteManagerDialog::reloadFromSettings() {
    loadSites();
    refresh();
}

void SiteManagerDialog::loadSites() {
    const SavedSitesPersistence::LoadResult loaded =
        SavedSitesPersistence::loadSites({
            .trimSiteNames = false,
            .createNewId = [] { return newSiteId(); },
        });
    sites_ = loaded.sites;
    const SiteCredentialMigrationResult migration =
        SiteCredentialRepository::migrateLegacyPlaintext(loaded);
    legacySecretMigrationBlocked_ = !migration.complete;
    showPersistIssues(this, migration.issues);
    if (loaded.needsSave && migration.complete)
        (void)saveSites();
}

bool SiteManagerDialog::saveSites() {
    if (legacySecretMigrationBlocked_) {
        UiAlerts::warning(
            this, tr("Sites not saved"),
            tr("OpenSCP could not migrate one or more legacy credentials to "
               "the secure backend. Saved-site changes were not written, so "
               "the existing credentials remain recoverable."));
        return false;
    }
    // Keep Site Manager save semantics (no forced sync).
    SavedSitesPersistence::saveSites(sites_, false);
    return true;
}

void SiteManagerDialog::refresh() {
    if (model_)
        model_->reload();
    updateButtons();
}

int SiteManagerDialog::selectedSiteIndex() const {
    if (!table_ || !table_->selectionModel() || !proxy_)
        return -1;
    const QModelIndexList rows = table_->selectionModel()->selectedRows();
    if (rows.isEmpty())
        return -1;
    const QModelIndex source = proxy_->mapToSource(rows.constFirst());
    return source.isValid() ? source.row() : -1;
}

void SiteManagerDialog::selectSiteIndex(int siteIndex) {
    if (!table_ || !model_ || !proxy_ || siteIndex < 0 ||
        siteIndex >= sites_.size()) {
        return;
    }
    const QModelIndex source = model_->index(siteIndex, 0);
    const QModelIndex view = proxy_->mapFromSource(source);
    if (!view.isValid())
        return;
    table_->setCurrentIndex(view);
    table_->selectRow(view.row());
    table_->scrollTo(view, QAbstractItemView::PositionAtCenter);
    table_->setFocus(Qt::OtherFocusReason);
}

void SiteManagerDialog::onAdd() {
    ConnectionDialog dlg(this);
    dlg.setWindowTitle(tr("Add site"));
    dlg.setSiteNameVisible(true);
    if (dlg.exec() != QDialog::Accepted)
        return;
    auto sessionOptions = dlg.options();
    QString name = normalizedSiteName(dlg.siteName());
    if (name.isEmpty()) {
        name = normalizedSiteName(QString("%1@%2").arg(
            QString::fromStdString(sessionOptions.username),
            QString::fromStdString(sessionOptions.host)));
    }
    if (name.isEmpty()) {
        showMissingNameIssue(this);
        return;
    }
    if (hasDuplicateSiteName(sites_, name)) {
        showDuplicateNameIssue(this, name);
        return;
    }
    SiteEntry newEntry;
    newEntry.siteId = newSiteId();
    newEntry.name = name;
    newEntry.opt = sessionOptions;
    SiteCredentialRepository::clearCredentialFields(newEntry.opt);
    newEntry.initialLocalPath = dlg.initialLocalPath();
    newEntry.initialRemotePath = dlg.initialRemotePath();
    newEntry.rememberLastPaths = dlg.rememberLastPaths();
    sites_.push_back(newEntry);
    if (!saveSites()) {
        sites_.removeLast();
        refresh();
        return;
    }
    refresh();
    SiteCredentialRepository credentials;
    showPersistIssues(
        this, credentials.save(newEntry, sessionOptions).issueMessages());
}

void SiteManagerDialog::onEdit() {
    const int modelIndex = selectedSiteIndex();
    if (modelIndex < 0 || modelIndex >= sites_.size())
        return;
    SiteEntry editedEntry = sites_[modelIndex];
    ConnectionDialog dlg(this);
    dlg.setWindowTitle(tr("Edit site"));
    dlg.setSiteNameVisible(true);
    dlg.setSiteName(editedEntry.name);
    dlg.setInitialLocalPath(editedEntry.initialLocalPath);
    dlg.setInitialRemotePath(editedEntry.initialRemotePath);
    dlg.setRememberLastPaths(editedEntry.rememberLastPaths);
    SiteCredentialRepository credentials;
    openscp::SessionOptions sessionOptions = editedEntry.opt;
    showPersistIssues(
        this, credentials.load(editedEntry, sessionOptions).issueMessages());
    dlg.setOptions(sessionOptions);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const openscp::SessionOptions editedOptions = dlg.options();
    editedEntry.opt = editedOptions;
    SiteCredentialRepository::clearCredentialFields(editedEntry.opt);
    editedEntry.initialLocalPath = dlg.initialLocalPath();
    editedEntry.initialRemotePath = dlg.initialRemotePath();
    editedEntry.rememberLastPaths = dlg.rememberLastPaths();
    QString name = normalizedSiteName(dlg.siteName());
    if (name.isEmpty()) {
        showMissingNameIssue(this);
        return;
    }
    if (hasDuplicateSiteName(sites_, name, modelIndex)) {
        showDuplicateNameIssue(this, name);
        return;
    }
    const SiteEntry previousEntry = sites_[modelIndex];
    const QString oldName = previousEntry.name;
    editedEntry.name = name;
    sites_[modelIndex] = editedEntry;
    if (!saveSites()) {
        sites_[modelIndex] = previousEntry;
        refresh();
        selectSiteIndex(modelIndex);
        return;
    }
    refresh();
    selectSiteIndex(modelIndex);
    showPersistIssues(
        this, credentials.save(editedEntry, editedOptions).issueMessages());
    if (!oldName.isEmpty() && oldName != name)
        credentials.removeLegacyNameKeys(oldName);
}

void SiteManagerDialog::onDuplicate() {
    const int modelIndex = selectedSiteIndex();
    if (modelIndex < 0 || modelIndex >= sites_.size())
        return;

    const SiteEntry source = sites_.at(modelIndex);
    SiteEntry duplicate = source;
    duplicate.siteId = newSiteId();
    duplicate.opt.password.reset();
    duplicate.opt.private_key_passphrase.reset();
    duplicate.opt.proxy_password.reset();

    const QString sourceName = source.name.trimmed().isEmpty()
                                   ? tr("Unnamed site")
                                   : source.name.trimmed();
    const QString baseName = tr("%1 copy").arg(sourceName);
    duplicate.name = baseName;
    for (int suffix = 2;
         hasDuplicateSiteName(sites_, duplicate.name) && suffix < 10000;
         ++suffix) {
        duplicate.name = tr("%1 copy %2").arg(sourceName).arg(suffix);
    }
    if (hasDuplicateSiteName(sites_, duplicate.name)) {
        duplicate.name =
            QString("%1 %2").arg(baseName, duplicate.siteId.left(6));
    }

    sites_.push_back(duplicate);
    if (!saveSites()) {
        sites_.removeLast();
        refresh();
        selectSiteIndex(modelIndex);
        return;
    }
    refresh();
    selectSiteIndex(sites_.size() - 1);

    const auto copyChoice =
        UiAlerts::question(this, tr("Copy credentials?"),
                           tr("The site was duplicated with a new identity.\n\n"
                              "Copy its saved credentials to the duplicate?"),
                           QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (copyChoice != QMessageBox::Yes)
        return;

    SiteCredentialRepository credentials;
    showPersistIssues(this,
                      credentials.copy(source, duplicate).issueMessages());
}

void SiteManagerDialog::onRemove() {
    const int modelIndex = selectedSiteIndex();
    if (modelIndex < 0 || modelIndex >= sites_.size())
        return;
    // Capture fields before removing for optional cleanup
    const SiteEntry removed = sites_[modelIndex];
    const QString removedHost = QString::fromStdString(removed.opt.host);
    const std::uint16_t removedPort = removed.opt.port;
    const QString removedKh =
        removed.opt.known_hosts_path
            ? QString::fromStdString(*removed.opt.known_hosts_path)
            : QString();
    sites_.remove(modelIndex);
    if (!saveSites()) {
        sites_.insert(modelIndex, removed);
        refresh();
        selectSiteIndex(modelIndex);
        return;
    }
    // Optionally delete stored credentials and known_hosts entry for this site
    QSettings settings("OpenSCP", "OpenSCP");
    const bool deleteSecrets =
        settings.value("Sites/deleteSecretsOnRemove", false).toBool();
    if (deleteSecrets) {
        SiteCredentialRepository().removeAll(removed);
        // Also remove known_hosts entry if we know the file and host
        // Derive effective known_hosts path from the entry we just removed (if
        // available), falling back to ~/.ssh/known_hosts.
        QString khPath = removedKh;
        if (khPath.isEmpty()) {
            khPath = QDir::homePath() + "/.ssh/known_hosts";
        }
        QFileInfo khInfo(khPath);
        if (khInfo.exists() && khInfo.isFile()) {
            std::string rmerr;
            (void)openscp::RemoveKnownHostEntry(khPath.toStdString(),
                                                removedHost.toStdString(),
                                                removedPort, rmerr);
        }
    }
    refresh();
}

void SiteManagerDialog::onConnect() {
    accept();
}

bool SiteManagerDialog::selectedOptions(openscp::SessionOptions &out) const {
    const int modelIndex = selectedSiteIndex();
    if (modelIndex < 0 || modelIndex >= sites_.size())
        return false;
    out = sites_[modelIndex].opt;
    // Apply global security preferences
    {
        QSettings settings("OpenSCP", "OpenSCP");
        out.known_hosts_hash_names =
            settings.value("Security/knownHostsHashed", true).toBool();
        out.show_fp_hex = settings.value("Security/fpHex", false).toBool();
    }
    const SiteEntry &selected = sites_[modelIndex];
    SiteCredentialRepository credentials;
    showPersistIssues(
        const_cast<SiteManagerDialog *>(this),
        credentials.load(selected, out, true, modelIndex).issueMessages());
    return true;
}

bool SiteManagerDialog::selectedSite(SiteEntry &out) const {
    const int modelIndex = selectedSiteIndex();
    if (modelIndex < 0 || modelIndex >= sites_.size())
        return false;
    out = sites_.at(modelIndex);
    openscp::SessionOptions options;
    if (!selectedOptions(options))
        return false;
    out.opt = std::move(options);
    return true;
}

void SiteManagerDialog::updateButtons() {
    bool hasSelection = table_ && table_->selectionModel() &&
                        table_->selectionModel()->hasSelection();
    if (btEdit_)
        btEdit_->setEnabled(hasSelection);
    if (btDuplicate_)
        btDuplicate_->setEnabled(hasSelection);
    if (btDel_)
        btDel_->setEnabled(hasSelection);
    if (btConn_)
        btConn_->setEnabled(hasSelection);
}
