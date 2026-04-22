// Manages saved sites with QSettings and SecretStore for credentials.
#include "SiteManagerDialog.hpp"
#include "ConnectionDialog.hpp"
#include "SavedSitesPersistence.hpp"
#include "SecretStore.hpp"
#include "UiAlerts.hpp"
#include "openscp/KnownHostsUtils.hpp"
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>

static QString persistStatusText(SecretStore::PersistStatus status) {
    switch (status) {
    case SecretStore::PersistStatus::Stored:
        return QObject::tr("stored");
    case SecretStore::PersistStatus::Unavailable:
        return QObject::tr("unavailable");
    case SecretStore::PersistStatus::PermissionDenied:
        return QObject::tr("permission denied");
    case SecretStore::PersistStatus::BackendError:
        return QObject::tr("backend error");
    }
    return QObject::tr("unknown");
}

static QString
persistIssueLine(const QString &label,
                 const SecretStore::PersistResult &persistResult) {
    QString line =
        QString("%1: %2").arg(label, persistStatusText(persistResult.status));
    if (!persistResult.detail.isEmpty())
        line += QString(" (%1)").arg(persistResult.detail);
    return line;
}

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

static QString idSecretKey(const QString &siteId, const QString &item) {
    return QString("site-id:%1:%2").arg(siteId, item);
}

static QString legacyNameSecretKey(const QString &siteName,
                                   const QString &item) {
    return QString("site:%1:%2").arg(siteName, item);
}

static QString siteSecretKey(const SiteEntry &entry, const QString &item) {
    if (!entry.siteId.isEmpty())
        return idSecretKey(entry.siteId, item);
    return legacyNameSecretKey(entry.name, item);
}

static void removeLegacyNameSecrets(SecretStore &store,
                                    const QString &siteName) {
    if (siteName.isEmpty())
        return;
    store.removeSecret(
        legacyNameSecretKey(siteName, QStringLiteral("password")));
    store.removeSecret(
        legacyNameSecretKey(siteName, QStringLiteral("keypass")));
    store.removeSecret(
        legacyNameSecretKey(siteName, QStringLiteral("proxypass")));
}

SiteManagerDialog::SiteManagerDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Site Manager"));
    resize(720, 480); // compact default; view will elide/scroll as needed
    auto *mainLayout = new QVBoxLayout(this);
    table_ = new QTableWidget(this);
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels(
        {tr("Name"), tr("Protocol"), tr("Host"), tr("User")});
    table_->verticalHeader()->setVisible(false);
    // Column sizing: stretch to fill and adapt on resize
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
    btEdit_ = dialogButtons->addButton(tr("Edit"), QDialogButtonBox::ActionRole);
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
    // Double-click: primary action (connect) for faster workflow.
    connect(table_, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *clickedItem) {
                if (!clickedItem)
                    return;
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
    if (loaded.needsSave)
        saveSites();
}

void SiteManagerDialog::saveSites() {
    // Keep Site Manager save semantics (no forced sync).
    SavedSitesPersistence::saveSites(sites_, false);
}

void SiteManagerDialog::refresh() {
    // Avoid reordering while populating
    const bool wasSorting = table_->isSortingEnabled();
    if (wasSorting)
        table_->setSortingEnabled(false);
    table_->setRowCount(sites_.size());
    for (int siteIndex = 0; siteIndex < sites_.size(); ++siteIndex) {
        const QString fullName = sites_[siteIndex].name;
        // Keep full text in the item; let view elide visually
        auto *itName = new QTableWidgetItem(fullName);
        itName->setToolTip(fullName);
        itName->setData(Qt::UserRole + 1, fullName);
        const QString fullHost =
            QString::fromStdString(sites_[siteIndex].opt.host);
        auto *itHost = new QTableWidgetItem(fullHost);
        itHost->setToolTip(fullHost);
        itHost->setData(Qt::UserRole + 1, fullHost);
        const QString fullProtocol = QString::fromLatin1(
            openscp::protocolDisplayName(sites_[siteIndex].opt.protocol));
        auto *itProtocol = new QTableWidgetItem(fullProtocol);
        itProtocol->setToolTip(fullProtocol);
        itProtocol->setData(Qt::UserRole + 1, fullProtocol);
        const QString fullUser =
            QString::fromStdString(sites_[siteIndex].opt.username);
        auto *itUser = new QTableWidgetItem(fullUser);
        itUser->setToolTip(fullUser);
        itUser->setData(Qt::UserRole + 1, fullUser);
        // Store original index so selection works even when the view is sorted
        itName->setData(Qt::UserRole, siteIndex);
        itProtocol->setData(Qt::UserRole, siteIndex);
        itHost->setData(Qt::UserRole, siteIndex);
        itUser->setData(Qt::UserRole, siteIndex);
        table_->setItem(siteIndex, 0, itName);
        table_->setItem(siteIndex, 1, itProtocol);
        table_->setItem(siteIndex, 2, itHost);
        table_->setItem(siteIndex, 3, itUser);
    }
    if (wasSorting)
        table_->setSortingEnabled(true);
    updateButtons();
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
        name = normalizedSiteName(
            QString("%1@%2").arg(QString::fromStdString(sessionOptions.username),
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
    sites_.push_back(newEntry);
    saveSites();
    refresh();
    // Save secrets
    SecretStore store;
    QStringList persistIssues;
    if (sessionOptions.password) {
        auto persistResult = store.setSecret(
            siteSecretKey(newEntry, QStringLiteral("password")),
            QString::fromStdString(*sessionOptions.password));
        if (!persistResult.isStored())
            persistIssues << persistIssueLine(tr("Password"), persistResult);
    }
    if (sessionOptions.private_key_passphrase) {
        auto persistResult = store.setSecret(
            siteSecretKey(newEntry, QStringLiteral("keypass")),
            QString::fromStdString(*sessionOptions.private_key_passphrase));
        if (!persistResult.isStored())
            persistIssues
                << persistIssueLine(tr("Key passphrase"), persistResult);
    }
    if (sessionOptions.proxy_type != openscp::ProxyType::None &&
        sessionOptions.proxy_password) {
        auto persistResult = store.setSecret(
            siteSecretKey(newEntry, QStringLiteral("proxypass")),
            QString::fromStdString(*sessionOptions.proxy_password));
        if (!persistResult.isStored())
            persistIssues
                << persistIssueLine(tr("Proxy password"), persistResult);
    } else {
        store.removeSecret(siteSecretKey(newEntry, QStringLiteral("proxypass")));
    }
    showPersistIssues(this, persistIssues);
}

void SiteManagerDialog::onEdit() {
    auto selectionModel = table_->selectionModel();
    if (!selectionModel || !selectionModel->hasSelection())
        return;
    int viewRow = selectionModel->selectedRows().first().row();
    int modelIndex = table_->item(viewRow, 0)
                         ? table_->item(viewRow, 0)->data(Qt::UserRole).toInt()
                         : viewRow;
    if (modelIndex < 0 || modelIndex >= sites_.size())
        return;
    SiteEntry editedEntry = sites_[modelIndex];
    ConnectionDialog dlg(this);
    dlg.setWindowTitle(tr("Edit site"));
    dlg.setSiteNameVisible(true);
    dlg.setSiteName(editedEntry.name);
    // Preload site options and stored secrets
    {
        SecretStore store;
        openscp::SessionOptions sessionOptions = editedEntry.opt;
        if (auto storedPassword = store.getSecret(
                siteSecretKey(editedEntry, QStringLiteral("password")))) {
            sessionOptions.password = storedPassword->toStdString();
        } else if (auto legacyPassword = store.getSecret(legacyNameSecretKey(
                       editedEntry.name, QStringLiteral("password")))) {
            sessionOptions.password = legacyPassword->toStdString();
        }
        if (auto storedKeyPassphrase = store.getSecret(
                siteSecretKey(editedEntry, QStringLiteral("keypass")))) {
            sessionOptions.private_key_passphrase =
                storedKeyPassphrase->toStdString();
        } else if (auto legacyKeyPassphrase =
                       store.getSecret(legacyNameSecretKey(
                           editedEntry.name, QStringLiteral("keypass")))) {
            sessionOptions.private_key_passphrase =
                legacyKeyPassphrase->toStdString();
        }
        if (sessionOptions.proxy_type != openscp::ProxyType::None) {
            if (auto storedProxyPassword = store.getSecret(
                    siteSecretKey(editedEntry, QStringLiteral("proxypass")))) {
                sessionOptions.proxy_password =
                    storedProxyPassword->toStdString();
            } else if (auto legacyProxyPassword =
                           store.getSecret(legacyNameSecretKey(
                               editedEntry.name, QStringLiteral("proxypass")))) {
                sessionOptions.proxy_password =
                    legacyProxyPassword->toStdString();
            }
        }
        dlg.setOptions(sessionOptions);
    }
    if (dlg.exec() != QDialog::Accepted)
        return;
    editedEntry.opt = dlg.options();
    QString name = normalizedSiteName(dlg.siteName());
    if (name.isEmpty()) {
        showMissingNameIssue(this);
        return;
    }
    if (hasDuplicateSiteName(sites_, name, modelIndex)) {
        showDuplicateNameIssue(this, name);
        return;
    }
    const QString oldName = sites_[modelIndex].name;
    editedEntry.name = name;
    sites_[modelIndex] = editedEntry;
    saveSites();
    refresh();
    // Reselect and focus the edited site even if sorting changed the row
    for (int rowIndex = 0; rowIndex < table_->rowCount(); ++rowIndex) {
        if (auto *nameItem = table_->item(rowIndex, 0)) {
            if (nameItem->data(Qt::UserRole).toInt() == modelIndex) {
                table_->setCurrentCell(rowIndex, 0);
                table_->selectRow(rowIndex);
                table_->scrollToItem(nameItem,
                                     QAbstractItemView::PositionAtCenter);
                table_->setFocus(Qt::OtherFocusReason);
                break;
            }
        }
    }
    // Update secrets
    SecretStore store;
    QStringList persistIssues;
    if (editedEntry.opt.password) {
        auto persistResult = store.setSecret(
            siteSecretKey(editedEntry, QStringLiteral("password")),
            QString::fromStdString(*editedEntry.opt.password));
        if (!persistResult.isStored())
            persistIssues << persistIssueLine(tr("Password"), persistResult);
    }
    if (editedEntry.opt.private_key_passphrase) {
        auto persistResult = store.setSecret(
            siteSecretKey(editedEntry, QStringLiteral("keypass")),
            QString::fromStdString(*editedEntry.opt.private_key_passphrase));
        if (!persistResult.isStored())
            persistIssues
                << persistIssueLine(tr("Key passphrase"), persistResult);
    }
    if (editedEntry.opt.proxy_type != openscp::ProxyType::None &&
        editedEntry.opt.proxy_password) {
        auto persistResult = store.setSecret(
            siteSecretKey(editedEntry, QStringLiteral("proxypass")),
            QString::fromStdString(*editedEntry.opt.proxy_password));
        if (!persistResult.isStored())
            persistIssues
                << persistIssueLine(tr("Proxy password"), persistResult);
    } else {
        store.removeSecret(siteSecretKey(editedEntry, QStringLiteral("proxypass")));
    }
    if (!oldName.isEmpty() && oldName != name) {
        removeLegacyNameSecrets(store, oldName);
    }
    showPersistIssues(this, persistIssues);
}

void SiteManagerDialog::onRemove() {
    auto selectionModel = table_->selectionModel();
    if (!selectionModel || !selectionModel->hasSelection())
        return;
    int viewRow = selectionModel->selectedRows().first().row();
    int modelIndex = table_->item(viewRow, 0)
                         ? table_->item(viewRow, 0)->data(Qt::UserRole).toInt()
                         : viewRow;
    if (modelIndex < 0 || modelIndex >= sites_.size())
        return;
    // Capture fields before removing for optional cleanup
    const SiteEntry removed = sites_[modelIndex];
    const QString name = removed.name;
    const QString removedHost = QString::fromStdString(removed.opt.host);
    const std::uint16_t removedPort = removed.opt.port;
    const QString removedKh =
        removed.opt.known_hosts_path
            ? QString::fromStdString(*removed.opt.known_hosts_path)
            : QString();
    sites_.remove(modelIndex);
    saveSites();
    // Optionally delete stored credentials and known_hosts entry for this site
    QSettings settings("OpenSCP", "OpenSCP");
    const bool deleteSecrets =
        settings.value("Sites/deleteSecretsOnRemove", false).toBool();
    if (deleteSecrets) {
        SecretStore store;
        store.removeSecret(siteSecretKey(removed, QStringLiteral("password")));
        store.removeSecret(siteSecretKey(removed, QStringLiteral("keypass")));
        store.removeSecret(siteSecretKey(removed, QStringLiteral("proxypass")));
        removeLegacyNameSecrets(store, name);
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

void SiteManagerDialog::onConnect() { accept(); }

bool SiteManagerDialog::selectedOptions(openscp::SessionOptions &out) const {
    auto selectionModel = table_->selectionModel();
    if (!selectionModel || !selectionModel->hasSelection())
        return false;
    int viewRow = selectionModel->selectedRows().first().row();
    int modelIndex = table_->item(viewRow, 0)
                         ? table_->item(viewRow, 0)->data(Qt::UserRole).toInt()
                         : viewRow;
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
    // Fill secrets at connection time
    SecretStore store;
    const SiteEntry &selected = sites_[modelIndex];
    const QString name = selected.name;
    const bool hasStableId = !selected.siteId.isEmpty();
    bool haveSecret = false;
    QStringList persistIssues;
    bool migratedNamePw = false;
    bool migratedNameKp = false;
    bool migratedNamePp = false;
    if (auto storedPassword = store.getSecret(
            siteSecretKey(selected, QStringLiteral("password")))) {
        out.password = storedPassword->toStdString();
        haveSecret = true;
    } else if (auto legacyPassword = store.getSecret(
                   legacyNameSecretKey(name, QStringLiteral("password")))) {
        out.password = legacyPassword->toStdString();
        haveSecret = true;
        auto persistResult = store.setSecret(
            siteSecretKey(selected, QStringLiteral("password")),
            *legacyPassword);
        if (persistResult.isStored())
            migratedNamePw = true;
        else
            persistIssues
                << persistIssueLine(QObject::tr("Password"), persistResult);
    }
    if (auto storedKeyPassphrase = store.getSecret(
            siteSecretKey(selected, QStringLiteral("keypass")))) {
        out.private_key_passphrase = storedKeyPassphrase->toStdString();
        haveSecret = true;
    } else if (auto legacyKeyPassphrase = store.getSecret(
                   legacyNameSecretKey(name, QStringLiteral("keypass")))) {
        out.private_key_passphrase = legacyKeyPassphrase->toStdString();
        haveSecret = true;
        auto persistResult = store.setSecret(
            siteSecretKey(selected, QStringLiteral("keypass")),
            *legacyKeyPassphrase);
        if (persistResult.isStored())
            migratedNameKp = true;
        else
            persistIssues << persistIssueLine(QObject::tr("Key passphrase"),
                                              persistResult);
    }
    if (out.proxy_type != openscp::ProxyType::None) {
        if (auto storedProxyPassword = store.getSecret(
                siteSecretKey(selected, QStringLiteral("proxypass")))) {
            out.proxy_password = storedProxyPassword->toStdString();
            haveSecret = true;
        } else if (auto legacyProxyPassword = store.getSecret(legacyNameSecretKey(
                       name, QStringLiteral("proxypass")))) {
            out.proxy_password = legacyProxyPassword->toStdString();
            haveSecret = true;
            auto persistResult = store.setSecret(
                siteSecretKey(selected, QStringLiteral("proxypass")),
                *legacyProxyPassword);
            if (persistResult.isStored())
                migratedNamePp = true;
            else
                persistIssues << persistIssueLine(QObject::tr("Proxy password"),
                                                  persistResult);
        }
    } else {
        out.proxy_password.reset();
    }
    if (hasStableId && migratedNamePw)
        store.removeSecret(
            legacyNameSecretKey(name, QStringLiteral("password")));
    if (hasStableId && migratedNameKp)
        store.removeSecret(
            legacyNameSecretKey(name, QStringLiteral("keypass")));
    if (hasStableId && migratedNamePp)
        store.removeSecret(
            legacyNameSecretKey(name, QStringLiteral("proxypass")));
    if (!haveSecret) {
        // Compatibility: migrate old values from QSettings if present
        QSettings settings("OpenSCP", "OpenSCP");
        int siteCount = settings.beginReadArray("sites");
        bool migratedPw = false, migratedKp = false, migratedPp = false;
        if (modelIndex >= 0 && modelIndex < siteCount) {
            settings.setArrayIndex(modelIndex);
            const QString legacyPassword = settings.value("password").toString();
            const QString legacyKeyPassphrase =
                settings.value("keyPass").toString();
            const QString legacyProxyPassword =
                settings.value("proxyPass").toString();
            if (!legacyPassword.isEmpty()) {
                out.password = legacyPassword.toStdString();
                auto persistResult =
                    store.setSecret(siteSecretKey(selected, QStringLiteral("password")),
                                    legacyPassword);
                if (persistResult.isStored())
                    migratedPw = true;
                else
                    persistIssues
                        << persistIssueLine(QObject::tr("Password"), persistResult);
            }
            if (!legacyKeyPassphrase.isEmpty()) {
                out.private_key_passphrase = legacyKeyPassphrase.toStdString();
                auto persistResult = store.setSecret(
                    siteSecretKey(selected, QStringLiteral("keypass")),
                    legacyKeyPassphrase);
                if (persistResult.isStored())
                    migratedKp = true;
                else
                    persistIssues
                        << persistIssueLine(QObject::tr("Key passphrase"),
                                            persistResult);
            }
            if (out.proxy_type != openscp::ProxyType::None &&
                !legacyProxyPassword.isEmpty()) {
                out.proxy_password = legacyProxyPassword.toStdString();
                auto persistResult = store.setSecret(
                    siteSecretKey(selected, QStringLiteral("proxypass")),
                    legacyProxyPassword);
                if (persistResult.isStored())
                    migratedPp = true;
                else
                    persistIssues
                        << persistIssueLine(QObject::tr("Proxy password"),
                                            persistResult);
            } else if (out.proxy_type == openscp::ProxyType::None &&
                       !legacyProxyPassword.isEmpty()) {
                migratedPp = true;
            }
        }
        settings.endArray();
        // After migrating, remove legacy keys from QSettings to avoid storing
        // secrets in plaintext
        if ((migratedPw || migratedKp || migratedPp) && modelIndex >= 0) {
            settings.beginWriteArray("sites");
            settings.setArrayIndex(modelIndex);
            if (migratedPw)
                settings.remove("password");
            if (migratedKp)
                settings.remove("keyPass");
            if (migratedPp)
                settings.remove("proxyPass");
            settings.endArray();
            settings.sync();
        }
    }
    if (!persistIssues.isEmpty()) {
        showPersistIssues(const_cast<SiteManagerDialog *>(this), persistIssues);
    }
    return true;
}

void SiteManagerDialog::updateButtons() {
    bool hasSelection = table_ && table_->selectionModel() &&
                        table_->selectionModel()->hasSelection();
    if (btEdit_)
        btEdit_->setEnabled(hasSelection);
    if (btDel_)
        btDel_->setEnabled(hasSelection);
    if (btConn_)
        btConn_->setEnabled(hasSelection);
}
