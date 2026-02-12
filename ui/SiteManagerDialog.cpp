// Manages saved sites with QSettings and SecretStore for credentials.
#include "SiteManagerDialog.hpp"
#include "ConnectionDialog.hpp"
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QSettings>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include "SecretStore.hpp"
#include "openscp/Libssh2SftpClient.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QToolTip>
#include <QCursor>

static QString persistStatusText(SecretStore::PersistStatus st) {
    switch (st) {
        case SecretStore::PersistStatus::Stored: return QObject::tr("guardado");
        case SecretStore::PersistStatus::Unavailable: return QObject::tr("no disponible");
        case SecretStore::PersistStatus::PermissionDenied: return QObject::tr("permiso denegado");
        case SecretStore::PersistStatus::BackendError: return QObject::tr("error del backend");
    }
    return QObject::tr("desconocido");
}

static QString persistIssueLine(const QString& label, const SecretStore::PersistResult& r) {
    QString line = QString("%1: %2").arg(label, persistStatusText(r.status));
    if (!r.detail.isEmpty()) line += QString(" (%1)").arg(r.detail);
    return line;
}

static void showPersistIssues(QWidget* parent, const QStringList& issues) {
    if (issues.isEmpty()) return;
    QMessageBox::warning(
        parent,
        QObject::tr("Credenciales no guardadas"),
        QObject::tr("No se pudieron guardar una o más credenciales en el backend seguro:\n%1")
            .arg(issues.join("\n"))
    );
}

SiteManagerDialog::SiteManagerDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Gestor de sitios"));
    resize(720, 480); // compact default; view will elide/scroll as needed
    auto* lay = new QVBoxLayout(this);
    table_ = new QTableWidget(this);
    table_->setColumnCount(3);
    table_->setHorizontalHeaderLabels({ tr("Nombre"), tr("Host"), tr("Usuario") });
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

    auto* bb = new QDialogButtonBox(this);
    btAdd_  = bb->addButton(tr("Añadir"),   QDialogButtonBox::ActionRole);
    btEdit_ = bb->addButton(tr("Editar"),   QDialogButtonBox::ActionRole);
    btDel_  = bb->addButton(tr("Eliminar"), QDialogButtonBox::ActionRole);
    btConn_ = bb->addButton(tr("Conectar"), QDialogButtonBox::AcceptRole);
    btClose_= bb->addButton(QDialogButtonBox::Close);
    if (btClose_) btClose_->setText(tr("Cerrar"));
    lay->addWidget(bb);
    connect(btAdd_,  &QPushButton::clicked, this, &SiteManagerDialog::onAdd);
    connect(btEdit_, &QPushButton::clicked, this, &SiteManagerDialog::onEdit);
    connect(btDel_,  &QPushButton::clicked, this, &SiteManagerDialog::onRemove);
    connect(btConn_, &QPushButton::clicked, this, &SiteManagerDialog::onConnect);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    loadSites();
    refresh();

    // Initial state: disable Edit/Delete/Connect if there is no selection
    updateButtons();
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &SiteManagerDialog::updateButtons);
    // Double-click: show full cell content as a tooltip at cursor position
    connect(table_, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem* it){
        if (!it) return;
        // Prefer full text stored in UserRole+1, then tooltip, then cell text
        QVariant fullVar = it->data(Qt::UserRole + 1);
        const QString full = fullVar.isValid() ? fullVar.toString() : (it->toolTip().isEmpty() ? it->text() : it->toolTip());
        if (!full.isEmpty()) QToolTip::showText(QCursor::pos(), full, table_);
    });
}

void SiteManagerDialog::loadSites() {
    sites_.clear();
    QSettings s("OpenSCP", "OpenSCP");
    int n = s.beginReadArray("sites");
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        SiteEntry e;
        e.name = s.value("name").toString();
        e.opt.host = s.value("host").toString().toStdString();
        e.opt.port = (std::uint16_t)s.value("port", 22).toUInt();
        e.opt.username = s.value("user").toString().toStdString();
        // Password and passphrase are no longer read from QSettings; they will be fetched from SecretStore when connecting
        const QString kp = s.value("keyPath").toString();
        if (!kp.isEmpty()) e.opt.private_key_path = kp.toStdString();
        // keyPass will be retrieved dynamically
        const QString kh = s.value("knownHosts").toString();
        if (!kh.isEmpty()) e.opt.known_hosts_path = kh.toStdString();
        e.opt.known_hosts_policy = (openscp::KnownHostsPolicy)s.value("khPolicy", (int)openscp::KnownHostsPolicy::Strict).toInt();
        sites_.push_back(e);
    }
    s.endArray();
    s.sync();
}

void SiteManagerDialog::saveSites() {
    QSettings s("OpenSCP", "OpenSCP");
    // Clear previous array to avoid stale entries after deletions
    s.remove("sites");
    s.beginWriteArray("sites");
    for (int i = 0; i < sites_.size(); ++i) {
        s.setArrayIndex(i);
        const auto& e = sites_[i];
        s.setValue("name", e.name);
        s.setValue("host", QString::fromStdString(e.opt.host));
        s.setValue("port", (int)e.opt.port);
        s.setValue("user", QString::fromStdString(e.opt.username));
        // Password and passphrase are stored in SecretStore under keys derived from the site name
        s.setValue("keyPath", e.opt.private_key_path ? QString::fromStdString(*e.opt.private_key_path) : QString());
        s.setValue("knownHosts", e.opt.known_hosts_path ? QString::fromStdString(*e.opt.known_hosts_path) : QString());
        s.setValue("khPolicy", (int)e.opt.known_hosts_policy);
    }
    s.endArray();
}

void SiteManagerDialog::refresh() {
    // Avoid reordering while populating
    const bool wasSorting = table_->isSortingEnabled();
    if (wasSorting) table_->setSortingEnabled(false);
    table_->setRowCount(sites_.size());
    for (int i = 0; i < sites_.size(); ++i) {
        const QString fullName = sites_[i].name;
        // Keep full text in the item; let view elide visually
        auto* itName = new QTableWidgetItem(fullName);
        itName->setToolTip(fullName);
        itName->setData(Qt::UserRole + 1, fullName);
        const QString fullHost = QString::fromStdString(sites_[i].opt.host);
        auto* itHost = new QTableWidgetItem(fullHost);
        itHost->setToolTip(fullHost);
        itHost->setData(Qt::UserRole + 1, fullHost);
        const QString fullUser = QString::fromStdString(sites_[i].opt.username);
        auto* itUser = new QTableWidgetItem(fullUser);
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
    if (wasSorting) table_->setSortingEnabled(true);
    updateButtons();
}

void SiteManagerDialog::onAdd() {
    ConnectionDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    auto opt = dlg.options();
    bool ok = false;
    QString name = QInputDialog::getText(
        this, tr("Nombre del sitio"), tr("Nombre:"),
        QLineEdit::Normal,
        QString("%1@%2").arg(QString::fromStdString(opt.username), QString::fromStdString(opt.host)),
        &ok
    );
    if (!ok || name.isEmpty()) return;
    SiteEntry e;
    e.name = name;
    e.opt = opt;
    sites_.push_back(e);
    saveSites();
    refresh();
    // Save secrets
    SecretStore store;
    QStringList persistIssues;
    if (opt.password) {
        auto r = store.setSecret(QString("site:%1:password").arg(name), QString::fromStdString(*opt.password));
        if (!r.ok()) persistIssues << persistIssueLine(tr("Contraseña"), r);
    }
    if (opt.private_key_passphrase) {
        auto r = store.setSecret(QString("site:%1:keypass").arg(name), QString::fromStdString(*opt.private_key_passphrase));
        if (!r.ok()) persistIssues << persistIssueLine(tr("Passphrase de clave"), r);
    }
    showPersistIssues(this, persistIssues);
}

void SiteManagerDialog::onEdit() {
    auto sel = table_->selectionModel();
    if (!sel || !sel->hasSelection()) return;
    int viewRow = sel->selectedRows().first().row();
    int modelIndex = table_->item(viewRow, 0) ? table_->item(viewRow, 0)->data(Qt::UserRole).toInt() : viewRow;
    if (modelIndex < 0 || modelIndex >= sites_.size()) return;
    SiteEntry e = sites_[modelIndex];
    ConnectionDialog dlg(this);
    // Preload site options and stored secrets
    {
        SecretStore store;
        openscp::SessionOptions opt = e.opt;
        if (auto pw = store.getSecret(QString("site:%1:password").arg(e.name))) opt.password = pw->toStdString();
        if (auto kp = store.getSecret(QString("site:%1:keypass").arg(e.name))) opt.private_key_passphrase = kp->toStdString();
        dlg.setOptions(opt);
    }
    if (dlg.exec() != QDialog::Accepted) return;
    e.opt = dlg.options();
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Nombre del sitio"), tr("Nombre:"), QLineEdit::Normal, e.name, &ok);
    if (!ok || name.isEmpty()) return;
    e.name = name;
    sites_[modelIndex] = e;
    saveSites();
    refresh();
    // Reselect and focus the edited site even if sorting changed the row
    for (int r = 0; r < table_->rowCount(); ++r) {
        if (auto* it = table_->item(r, 0)) {
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
        auto r = store.setSecret(QString("site:%1:password").arg(name), QString::fromStdString(*e.opt.password));
        if (!r.ok()) persistIssues << persistIssueLine(tr("Contraseña"), r);
    }
    if (e.opt.private_key_passphrase) {
        auto r = store.setSecret(QString("site:%1:keypass").arg(name), QString::fromStdString(*e.opt.private_key_passphrase));
        if (!r.ok()) persistIssues << persistIssueLine(tr("Passphrase de clave"), r);
    }
    showPersistIssues(this, persistIssues);
}

void SiteManagerDialog::onRemove() {
    auto sel = table_->selectionModel();
    if (!sel || !sel->hasSelection()) return;
    int viewRow = sel->selectedRows().first().row();
    int modelIndex = table_->item(viewRow, 0) ? table_->item(viewRow, 0)->data(Qt::UserRole).toInt() : viewRow;
    if (modelIndex < 0 || modelIndex >= sites_.size()) return;
    // Capture fields before removing for optional cleanup
    const SiteEntry removed = sites_[modelIndex];
    const QString name = removed.name;
    const QString removedHost = QString::fromStdString(removed.opt.host);
    const std::uint16_t removedPort = removed.opt.port;
    const QString removedKh = removed.opt.known_hosts_path ? QString::fromStdString(*removed.opt.known_hosts_path) : QString();
    sites_.remove(modelIndex);
    saveSites();
    // Optionally delete stored credentials and known_hosts entry for this site
    QSettings s("OpenSCP", "OpenSCP");
    const bool deleteSecrets = s.value("Sites/deleteSecretsOnRemove", false).toBool();
    if (deleteSecrets) {
        SecretStore store;
        store.removeSecret(QString("site:%1:password").arg(name));
        store.removeSecret(QString("site:%1:keypass").arg(name));
        // Also remove known_hosts entry if we know the file and host
        // Derive effective known_hosts path from the entry we just removed (if available),
        // falling back to ~/.ssh/known_hosts.
        QString khPath = removedKh;
        if (khPath.isEmpty()) {
            khPath = QDir::homePath() + "/.ssh/known_hosts";
        }
        QFileInfo khInfo(khPath);
        if (khInfo.exists() && khInfo.isFile()) {
            std::string rmerr;
            (void)openscp::RemoveKnownHostEntry(khPath.toStdString(), removedHost.toStdString(), removedPort, rmerr);
        }
    }
    refresh();
}

void SiteManagerDialog::onConnect() {
    accept();
}

bool SiteManagerDialog::selectedOptions(openscp::SessionOptions& out) const {
    auto sel = table_->selectionModel();
    if (!sel || !sel->hasSelection()) return false;
    int viewRow = sel->selectedRows().first().row();
    int modelIndex = table_->item(viewRow, 0) ? table_->item(viewRow, 0)->data(Qt::UserRole).toInt() : viewRow;
    if (modelIndex < 0 || modelIndex >= sites_.size()) return false;
    out = sites_[modelIndex].opt;
    // Apply global security preferences
    {
        QSettings s("OpenSCP", "OpenSCP");
        out.known_hosts_hash_names = s.value("Security/knownHostsHashed", true).toBool();
        out.show_fp_hex = s.value("Security/fpHex", false).toBool();
    }
    // Fill secrets at connection time
    SecretStore store;
    const QString name = sites_[modelIndex].name;
    bool haveSecret = false;
    if (auto pw = store.getSecret(QString("site:%1:password").arg(name))) {
        out.password = pw->toStdString();
        haveSecret = true;
    }
    if (auto kp = store.getSecret(QString("site:%1:keypass").arg(name))) {
        out.private_key_passphrase = kp->toStdString();
        haveSecret = true;
    }
    if (!haveSecret) {
        // Compatibility: migrate old values from QSettings if present
        QSettings s("OpenSCP", "OpenSCP");
        int n = s.beginReadArray("sites");
        bool migratedPw = false, migratedKp = false;
        QStringList persistIssues;
        if (modelIndex >= 0 && modelIndex < n) {
            s.setArrayIndex(modelIndex);
            const QString oldPw = s.value("password").toString();
            const QString oldKp = s.value("keyPass").toString();
            if (!oldPw.isEmpty()) {
                out.password = oldPw.toStdString();
                auto r = store.setSecret(QString("site:%1:password").arg(name), oldPw);
                if (r.ok()) migratedPw = true;
                else persistIssues << persistIssueLine(QObject::tr("Contraseña"), r);
            }
            if (!oldKp.isEmpty()) {
                out.private_key_passphrase = oldKp.toStdString();
                auto r = store.setSecret(QString("site:%1:keypass").arg(name), oldKp);
                if (r.ok()) migratedKp = true;
                else persistIssues << persistIssueLine(QObject::tr("Passphrase de clave"), r);
            }
        }
        s.endArray();
        if (!persistIssues.isEmpty()) {
            showPersistIssues(const_cast<SiteManagerDialog*>(this), persistIssues);
        }
        // After migrating, remove legacy keys from QSettings to avoid storing secrets in plaintext
        if ((migratedPw || migratedKp) && modelIndex >= 0) {
            s.beginWriteArray("sites");
            s.setArrayIndex(modelIndex);
            if (migratedPw) s.remove("password");
            if (migratedKp) s.remove("keyPass");
            s.endArray();
            s.sync();
        }
    }
    return true;
}

void SiteManagerDialog::updateButtons() {
    bool hasSel = table_ && table_->selectionModel() && table_->selectionModel()->hasSelection();
    if (btEdit_) btEdit_->setEnabled(hasSel);
    if (btDel_)  btDel_->setEnabled(hasSel);
    if (btConn_) btConn_->setEnabled(hasSel);
}
