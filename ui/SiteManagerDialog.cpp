// Administra sitios guardados con QSettings y SecretStore para credenciales.
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
#include "SecretStore.hpp"

SiteManagerDialog::SiteManagerDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Gestor de sitios");
    auto* lay = new QVBoxLayout(this);
    table_ = new QTableWidget(this);
    table_->setColumnCount(3);
    table_->setHorizontalHeaderLabels({"Nombre", "Host", "Usuario"});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    lay->addWidget(table_);

    auto* bb = new QDialogButtonBox(this);
    btAdd_  = bb->addButton("Añadir",   QDialogButtonBox::ActionRole);
    btEdit_ = bb->addButton("Editar",   QDialogButtonBox::ActionRole);
    btDel_  = bb->addButton("Eliminar", QDialogButtonBox::ActionRole);
    btConn_ = bb->addButton("Conectar", QDialogButtonBox::AcceptRole);
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

    // Estado inicial: deshabilitar Editar/Eliminar/Conectar si no hay selección
    updateButtons();
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &SiteManagerDialog::updateButtons);
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
        // Password y passphrase ya no se leen de QSettings; se obtendrán al conectar desde SecretStore
        const QString kp = s.value("keyPath").toString();
        if (!kp.isEmpty()) e.opt.private_key_path = kp.toStdString();
        // keyPass se recuperará dinámicamente
        const QString kh = s.value("knownHosts").toString();
        if (!kh.isEmpty()) e.opt.known_hosts_path = kh.toStdString();
        e.opt.known_hosts_policy = (openscp::KnownHostsPolicy)s.value("khPolicy", (int)openscp::KnownHostsPolicy::Strict).toInt();
        sites_.push_back(e);
    }
    s.endArray();
}

void SiteManagerDialog::saveSites() {
    QSettings s("OpenSCP", "OpenSCP");
    s.beginWriteArray("sites");
    for (int i = 0; i < sites_.size(); ++i) {
        s.setArrayIndex(i);
        const auto& e = sites_[i];
        s.setValue("name", e.name);
        s.setValue("host", QString::fromStdString(e.opt.host));
        s.setValue("port", (int)e.opt.port);
        s.setValue("user", QString::fromStdString(e.opt.username));
        // Password y passphrase se almacenan en SecretStore bajo claves derivadas del nombre del sitio
        s.setValue("keyPath", e.opt.private_key_path ? QString::fromStdString(*e.opt.private_key_path) : QString());
        s.setValue("knownHosts", e.opt.known_hosts_path ? QString::fromStdString(*e.opt.known_hosts_path) : QString());
        s.setValue("khPolicy", (int)e.opt.known_hosts_policy);
    }
    s.endArray();
}

void SiteManagerDialog::refresh() {
    table_->setRowCount(sites_.size());
    for (int i = 0; i < sites_.size(); ++i) {
        table_->setItem(i, 0, new QTableWidgetItem(sites_[i].name));
        table_->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(sites_[i].opt.host)));
        table_->setItem(i, 2, new QTableWidgetItem(QString::fromStdString(sites_[i].opt.username)));
    }
    updateButtons();
}

void SiteManagerDialog::onAdd() {
    ConnectionDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    auto opt = dlg.options();
    bool ok = false;
    QString name = QInputDialog::getText(
        this, "Nombre del sitio", "Nombre:",
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
    // Guardar secretos
    SecretStore store;
    if (opt.password) store.setSecret(QString("site:%1:password").arg(name), QString::fromStdString(*opt.password));
    if (opt.private_key_passphrase) store.setSecret(QString("site:%1:keypass").arg(name), QString::fromStdString(*opt.private_key_passphrase));
}

void SiteManagerDialog::onEdit() {
    auto sel = table_->selectionModel();
    if (!sel || !sel->hasSelection()) return;
    int row = sel->selectedRows().first().row();
    SiteEntry e = sites_[row];
    ConnectionDialog dlg(this);
    // Precargar opciones del sitio y secretos guardados
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
    QString name = QInputDialog::getText(this, "Nombre del sitio", "Nombre:", QLineEdit::Normal, e.name, &ok);
    if (!ok || name.isEmpty()) return;
    e.name = name;
    sites_[row] = e;
    saveSites();
    refresh();
    // Actualizar secretos
    SecretStore store;
    if (e.opt.password) store.setSecret(QString("site:%1:password").arg(name), QString::fromStdString(*e.opt.password));
    if (e.opt.private_key_passphrase) store.setSecret(QString("site:%1:keypass").arg(name), QString::fromStdString(*e.opt.private_key_passphrase));
}

void SiteManagerDialog::onRemove() {
    auto sel = table_->selectionModel();
    if (!sel || !sel->hasSelection()) return;
    int row = sel->selectedRows().first().row();
    sites_.remove(row);
    saveSites();
    refresh();
}

void SiteManagerDialog::onConnect() {
    accept();
}

bool SiteManagerDialog::selectedOptions(openscp::SessionOptions& out) const {
    auto sel = table_->selectionModel();
    if (!sel || !sel->hasSelection()) return false;
    int row = sel->selectedRows().first().row();
    if (row < 0 || row >= sites_.size()) return false;
    out = sites_[row].opt;
    // Rellenar secretos en tiempo de conexión
    SecretStore store;
    const QString name = sites_[row].name;
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
        // Compat: migrar antiguos valores desde QSettings si existen
        QSettings s("OpenSCP", "OpenSCP");
        int n = s.beginReadArray("sites");
        if (row >= 0 && row < n) {
            s.setArrayIndex(row);
            const QString oldPw = s.value("password").toString();
            const QString oldKp = s.value("keyPass").toString();
            if (!oldPw.isEmpty()) {
                out.password = oldPw.toStdString();
                store.setSecret(QString("site:%1:password").arg(name), oldPw);
            }
            if (!oldKp.isEmpty()) {
                out.private_key_passphrase = oldKp.toStdString();
                store.setSecret(QString("site:%1:keypass").arg(name), oldKp);
            }
        }
        s.endArray();
    }
    return true;
}

void SiteManagerDialog::updateButtons() {
    bool hasSel = table_ && table_->selectionModel() && table_->selectionModel()->hasSelection();
    if (btEdit_) btEdit_->setEnabled(hasSel);
    if (btDel_)  btDel_->setEnabled(hasSel);
    if (btConn_) btConn_->setEnabled(hasSel);
}
