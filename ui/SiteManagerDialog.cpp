// Manages saved sites with QSettings and SecretStore for credentials.
#include "SiteManagerDialog.hpp"
#include "ConnectionDialog.hpp"
#include "SecretStore.hpp"
#include "openscp/Libssh2SftpClient.hpp"
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>

static QString persistStatusText(SecretStore::PersistStatus st) {
    switch (st) {
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

static QString persistIssueLine(const QString &label,
                                const SecretStore::PersistResult &r) {
    QString line = QString("%1: %2").arg(label, persistStatusText(r.status));
    if (!r.detail.isEmpty())
        line += QString(" (%1)").arg(r.detail);
    return line;
}

static void showPersistIssues(QWidget *parent, const QStringList &issues) {
    if (issues.isEmpty())
        return;
    QMessageBox::warning(parent, QObject::tr("Credentials not saved"),
                         QObject::tr("Could not save one or more credentials "
                                     "in the secure backend:\\n%1")
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
    for (int i = 0; i < sites.size(); ++i) {
        if (i == ignoreIndex)
            continue;
        if (normalizedSiteName(sites[i].name)
                .compare(normalizedCandidate, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

static void showDuplicateNameIssue(QWidget *parent, const QString &name) {
    QMessageBox::warning(
        parent, QObject::tr("Duplicate name"),
        QObject::tr("A site named \"%1\" already exists. Use a different name.")
            .arg(name));
}

static void showMissingNameIssue(QWidget *parent) {
    QMessageBox::warning(
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

static QString siteSecretKey(const SiteEntry &e, const QString &item) {
    if (!e.id.isEmpty())
        return idSecretKey(e.id, item);
    return legacyNameSecretKey(e.name, item);
}

static void removeLegacyNameSecrets(SecretStore &store,
                                    const QString &siteName) {
    if (siteName.isEmpty())
        return;
    store.removeSecret(
        legacyNameSecretKey(siteName, QStringLiteral("password")));
    store.removeSecret(
        legacyNameSecretKey(siteName, QStringLiteral("keypass")));
}

SiteManagerDialog::SiteManagerDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Site Manager"));
    resize(720, 480); // compact default; view will elide/scroll as needed
    auto *lay = new QVBoxLayout(this);
    table_ = new QTableWidget(this);
    table_->setColumnCount(3);
    table_->setHorizontalHeaderLabels({tr("Name"), tr("Host"), tr("User")});
    table_->verticalHeader()->setVisible(false);
    // Column sizing: stretch to fill and adapt on resize
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table_->horizontalHeader()->setMinimumSectionSize(80);
    // Elide long text on the right to avoid oversized cells
    table_->setTextElideMode(Qt::ElideRight);
    table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    table_->setWordWrap(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setSortingEnabled(true);
    table_->sortByColumn(0, Qt::AscendingOrder);
    lay->addWidget(table_);

    auto *bb = new QDialogButtonBox(this);
    btAdd_ = bb->addButton(tr("Add"), QDialogButtonBox::ActionRole);
    btEdit_ = bb->addButton(tr("Edit"), QDialogButtonBox::ActionRole);
    btDel_ = bb->addButton(tr("Delete"), QDialogButtonBox::ActionRole);
    btConn_ = bb->addButton(tr("Connect"), QDialogButtonBox::AcceptRole);
    btClose_ = bb->addButton(QDialogButtonBox::Close);
    if (btClose_)
        btClose_->setText(tr("Close"));
    lay->addWidget(bb);
    connect(btAdd_, &QPushButton::clicked, this, &SiteManagerDialog::onAdd);
    connect(btEdit_, &QPushButton::clicked, this, &SiteManagerDialog::onEdit);
    connect(btDel_, &QPushButton::clicked, this, &SiteManagerDialog::onRemove);
    connect(btConn_, &QPushButton::clicked, this,
            &SiteManagerDialog::onConnect);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    loadSites();
    refresh();

    // Initial state: disable Edit/Delete/Connect if there is no selection
    updateButtons();
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &SiteManagerDialog::updateButtons);
    // Double-click: primary action (connect) for faster workflow.
    connect(table_, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *it) {
                if (!it)
                    return;
                onConnect();
            });
}

void SiteManagerDialog::reloadFromSettings() {
    loadSites();
    refresh();
}

void SiteManagerDialog::loadSites() {
    sites_.clear();
    QSettings s("OpenSCP", "OpenSCP");
    int n = s.beginReadArray("sites");
    bool needsSave = false;
    QSet<QString> usedIds;
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        SiteEntry e;
        e.id = s.value("id").toString().trimmed();
        if (e.id.isEmpty() || usedIds.contains(e.id)) {
            e.id = newSiteId();
            needsSave = true;
        }
        usedIds.insert(e.id);
        e.name = s.value("name").toString();
        e.opt.host = s.value("host").toString().toStdString();
        e.opt.port = (std::uint16_t)s.value("port", 22).toUInt();
        e.opt.username = s.value("user").toString().toStdString();
        // Password and passphrase are no longer read from QSettings; they will
        // be fetched from SecretStore when connecting
        const QString kp = s.value("keyPath").toString();
        if (!kp.isEmpty())
            e.opt.private_key_path = kp.toStdString();
        // keyPass will be retrieved dynamically
        const QString kh = s.value("knownHosts").toString();
        if (!kh.isEmpty())
            e.opt.known_hosts_path = kh.toStdString();
        e.opt.known_hosts_policy =
            (openscp::KnownHostsPolicy)s
                .value("khPolicy", (int)openscp::KnownHostsPolicy::Strict)
                .toInt();
        sites_.push_back(e);
    }
    s.endArray();
    s.sync();
    if (needsSave)
        saveSites();
}

void SiteManagerDialog::saveSites() {
    QSettings s("OpenSCP", "OpenSCP");
    // Clear previous array to avoid stale entries after deletions
    s.remove("sites");
    s.beginWriteArray("sites");
    for (int i = 0; i < sites_.size(); ++i) {
        s.setArrayIndex(i);
        const auto &e = sites_[i];
        s.setValue("id", e.id);
        s.setValue("name", e.name);
        s.setValue("host", QString::fromStdString(e.opt.host));
        s.setValue("port", (int)e.opt.port);
        s.setValue("user", QString::fromStdString(e.opt.username));
        // Password and passphrase are stored in SecretStore under keys derived
        // from stable site UUID.
        s.setValue("keyPath",
                   e.opt.private_key_path
                       ? QString::fromStdString(*e.opt.private_key_path)
                       : QString());
        s.setValue("knownHosts",
                   e.opt.known_hosts_path
                       ? QString::fromStdString(*e.opt.known_hosts_path)
                       : QString());
        s.setValue("khPolicy", (int)e.opt.known_hosts_policy);
    }
    s.endArray();
}

void SiteManagerDialog::refresh() {
    // Avoid reordering while populating
    const bool wasSorting = table_->isSortingEnabled();
    if (wasSorting)
        table_->setSortingEnabled(false);
    table_->setRowCount(sites_.size());
    for (int i = 0; i < sites_.size(); ++i) {
        const QString fullName = sites_[i].name;
        // Keep full text in the item; let view elide visually
        auto *itName = new QTableWidgetItem(fullName);
        itName->setToolTip(fullName);
        itName->setData(Qt::UserRole + 1, fullName);
        const QString fullHost = QString::fromStdString(sites_[i].opt.host);
        auto *itHost = new QTableWidgetItem(fullHost);
        itHost->setToolTip(fullHost);
        itHost->setData(Qt::UserRole + 1, fullHost);
        const QString fullUser = QString::fromStdString(sites_[i].opt.username);
        auto *itUser = new QTableWidgetItem(fullUser);
        itUser->setToolTip(fullUser);
        itUser->setData(Qt::UserRole + 1, fullUser);
        // Store original index so selection works even when the view is sorted
        itName->setData(Qt::UserRole, i);
        itHost->setData(Qt::UserRole, i);
        itUser->setData(Qt::UserRole, i);
        table_->setItem(i, 0, itName);
        table_->setItem(i, 1, itHost);
        table_->setItem(i, 2, itUser);
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
    auto opt = dlg.options();
    QString name = normalizedSiteName(dlg.siteName());
    if (name.isEmpty()) {
        name = normalizedSiteName(
            QString("%1@%2").arg(QString::fromStdString(opt.username),
                                 QString::fromStdString(opt.host)));
    }
    if (name.isEmpty()) {
        showMissingNameIssue(this);
        return;
    }
    if (hasDuplicateSiteName(sites_, name)) {
        showDuplicateNameIssue(this, name);
        return;
    }
    SiteEntry e;
    e.id = newSiteId();
    e.name = name;
    e.opt = opt;
    sites_.push_back(e);
    saveSites();
    refresh();
    // Save secrets
    SecretStore store;
    QStringList persistIssues;
    if (opt.password) {
        auto r = store.setSecret(siteSecretKey(e, QStringLiteral("password")),
                                 QString::fromStdString(*opt.password));
        if (!r.ok())
            persistIssues << persistIssueLine(tr("Password"), r);
    }
    if (opt.private_key_passphrase) {
        auto r = store.setSecret(
            siteSecretKey(e, QStringLiteral("keypass")),
            QString::fromStdString(*opt.private_key_passphrase));
        if (!r.ok())
            persistIssues << persistIssueLine(tr("Key passphrase"), r);
    }
    showPersistIssues(this, persistIssues);
}

void SiteManagerDialog::onEdit() {
    auto sel = table_->selectionModel();
    if (!sel || !sel->hasSelection())
        return;
    int viewRow = sel->selectedRows().first().row();
    int modelIndex = table_->item(viewRow, 0)
                         ? table_->item(viewRow, 0)->data(Qt::UserRole).toInt()
                         : viewRow;
    if (modelIndex < 0 || modelIndex >= sites_.size())
        return;
    SiteEntry e = sites_[modelIndex];
    ConnectionDialog dlg(this);
    dlg.setWindowTitle(tr("Edit site"));
    dlg.setSiteNameVisible(true);
    dlg.setSiteName(e.name);
    // Preload site options and stored secrets
    {
        SecretStore store;
        openscp::SessionOptions opt = e.opt;
        if (auto pw =
                store.getSecret(siteSecretKey(e, QStringLiteral("password")))) {
            opt.password = pw->toStdString();
        } else if (auto legacyPw = store.getSecret(legacyNameSecretKey(
                       e.name, QStringLiteral("password")))) {
            opt.password = legacyPw->toStdString();
        }
        if (auto kp =
                store.getSecret(siteSecretKey(e, QStringLiteral("keypass")))) {
            opt.private_key_passphrase = kp->toStdString();
        } else if (auto legacyKp = store.getSecret(legacyNameSecretKey(
                       e.name, QStringLiteral("keypass")))) {
            opt.private_key_passphrase = legacyKp->toStdString();
        }
        dlg.setOptions(opt);
    }
    if (dlg.exec() != QDialog::Accepted)
        return;
    e.opt = dlg.options();
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
    e.name = name;
    sites_[modelIndex] = e;
    saveSites();
    refresh();
    // Reselect and focus the edited site even if sorting changed the row
    for (int r = 0; r < table_->rowCount(); ++r) {
        if (auto *it = table_->item(r, 0)) {
            if (it->data(Qt::UserRole).toInt() == modelIndex) {
                table_->setCurrentCell(r, 0);
                table_->selectRow(r);
                table_->scrollToItem(it, QAbstractItemView::PositionAtCenter);
                table_->setFocus(Qt::OtherFocusReason);
                break;
            }
        }
    }
    // Update secrets
    SecretStore store;
    QStringList persistIssues;
    if (e.opt.password) {
        auto r = store.setSecret(siteSecretKey(e, QStringLiteral("password")),
                                 QString::fromStdString(*e.opt.password));
        if (!r.ok())
            persistIssues << persistIssueLine(tr("Password"), r);
    }
    if (e.opt.private_key_passphrase) {
        auto r = store.setSecret(
            siteSecretKey(e, QStringLiteral("keypass")),
            QString::fromStdString(*e.opt.private_key_passphrase));
        if (!r.ok())
            persistIssues << persistIssueLine(tr("Key passphrase"), r);
    }
    if (!oldName.isEmpty() && oldName != name) {
        removeLegacyNameSecrets(store, oldName);
    }
    showPersistIssues(this, persistIssues);
}

void SiteManagerDialog::onRemove() {
    auto sel = table_->selectionModel();
    if (!sel || !sel->hasSelection())
        return;
    int viewRow = sel->selectedRows().first().row();
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
    QSettings s("OpenSCP", "OpenSCP");
    const bool deleteSecrets =
        s.value("Sites/deleteSecretsOnRemove", false).toBool();
    if (deleteSecrets) {
        SecretStore store;
        store.removeSecret(siteSecretKey(removed, QStringLiteral("password")));
        store.removeSecret(siteSecretKey(removed, QStringLiteral("keypass")));
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
    auto sel = table_->selectionModel();
    if (!sel || !sel->hasSelection())
        return false;
    int viewRow = sel->selectedRows().first().row();
    int modelIndex = table_->item(viewRow, 0)
                         ? table_->item(viewRow, 0)->data(Qt::UserRole).toInt()
                         : viewRow;
    if (modelIndex < 0 || modelIndex >= sites_.size())
        return false;
    out = sites_[modelIndex].opt;
    // Apply global security preferences
    {
        QSettings s("OpenSCP", "OpenSCP");
        out.known_hosts_hash_names =
            s.value("Security/knownHostsHashed", true).toBool();
        out.show_fp_hex = s.value("Security/fpHex", false).toBool();
    }
    // Fill secrets at connection time
    SecretStore store;
    const SiteEntry &selected = sites_[modelIndex];
    const QString name = selected.name;
    const bool hasStableId = !selected.id.isEmpty();
    bool haveSecret = false;
    QStringList persistIssues;
    bool migratedNamePw = false;
    bool migratedNameKp = false;
    if (auto pw = store.getSecret(
            siteSecretKey(selected, QStringLiteral("password")))) {
        out.password = pw->toStdString();
        haveSecret = true;
    } else if (auto legacyPw = store.getSecret(
                   legacyNameSecretKey(name, QStringLiteral("password")))) {
        out.password = legacyPw->toStdString();
        haveSecret = true;
        auto r = store.setSecret(
            siteSecretKey(selected, QStringLiteral("password")), *legacyPw);
        if (r.ok())
            migratedNamePw = true;
        else
            persistIssues << persistIssueLine(QObject::tr("Password"), r);
    }
    if (auto kp = store.getSecret(
            siteSecretKey(selected, QStringLiteral("keypass")))) {
        out.private_key_passphrase = kp->toStdString();
        haveSecret = true;
    } else if (auto legacyKp = store.getSecret(
                   legacyNameSecretKey(name, QStringLiteral("keypass")))) {
        out.private_key_passphrase = legacyKp->toStdString();
        haveSecret = true;
        auto r = store.setSecret(
            siteSecretKey(selected, QStringLiteral("keypass")), *legacyKp);
        if (r.ok())
            migratedNameKp = true;
        else
            persistIssues << persistIssueLine(QObject::tr("Key passphrase"), r);
    }
    if (hasStableId && migratedNamePw)
        store.removeSecret(
            legacyNameSecretKey(name, QStringLiteral("password")));
    if (hasStableId && migratedNameKp)
        store.removeSecret(
            legacyNameSecretKey(name, QStringLiteral("keypass")));
    if (!haveSecret) {
        // Compatibility: migrate old values from QSettings if present
        QSettings s("OpenSCP", "OpenSCP");
        int n = s.beginReadArray("sites");
        bool migratedPw = false, migratedKp = false;
        if (modelIndex >= 0 && modelIndex < n) {
            s.setArrayIndex(modelIndex);
            const QString oldPw = s.value("password").toString();
            const QString oldKp = s.value("keyPass").toString();
            if (!oldPw.isEmpty()) {
                out.password = oldPw.toStdString();
                auto r = store.setSecret(
                    siteSecretKey(selected, QStringLiteral("password")), oldPw);
                if (r.ok())
                    migratedPw = true;
                else
                    persistIssues
                        << persistIssueLine(QObject::tr("Password"), r);
            }
            if (!oldKp.isEmpty()) {
                out.private_key_passphrase = oldKp.toStdString();
                auto r = store.setSecret(
                    siteSecretKey(selected, QStringLiteral("keypass")), oldKp);
                if (r.ok())
                    migratedKp = true;
                else
                    persistIssues
                        << persistIssueLine(QObject::tr("Key passphrase"), r);
            }
        }
        s.endArray();
        // After migrating, remove legacy keys from QSettings to avoid storing
        // secrets in plaintext
        if ((migratedPw || migratedKp) && modelIndex >= 0) {
            s.beginWriteArray("sites");
            s.setArrayIndex(modelIndex);
            if (migratedPw)
                s.remove("password");
            if (migratedKp)
                s.remove("keyPass");
            s.endArray();
            s.sync();
        }
    }
    if (!persistIssues.isEmpty()) {
        showPersistIssues(const_cast<SiteManagerDialog *>(this), persistIssues);
    }
    return true;
}

void SiteManagerDialog::updateButtons() {
    bool hasSel = table_ && table_->selectionModel() &&
                  table_->selectionModel()->hasSelection();
    if (btEdit_)
        btEdit_->setEnabled(hasSel);
    if (btDel_)
        btDel_->setEnabled(hasSel);
    if (btConn_)
        btConn_->setEnabled(hasSel);
}
