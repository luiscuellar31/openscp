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
    setWindowTitle(tr("Gestor de sitios"));
    resize(820, 520); // un poco más grande por defecto
    auto* lay = new QVBoxLayout(this);
    table_ = new QTableWidget(this);
    table_->setColumnCount(3);
    table_->setHorizontalHeaderLabels({ tr("Nombre"), tr("Host"), tr("Usuario") });
    table_->verticalHeader()->setVisible(false);
    // Priorizar mostrar el nombre: estirar la primera columna; host/usuario al tamaño de su contenido
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
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
    // Evitar reordenamiento durante el llenado
    const bool wasSorting = table_->isSortingEnabled();
    if (wasSorting) table_->setSortingEnabled(false);
    table_->setRowCount(sites_.size());
    for (int i = 0; i < sites_.size(); ++i) {
        auto* itName = new QTableWidgetItem(sites_[i].name);
        auto* itHost = new QTableWidgetItem(QString::fromStdString(sites_[i].opt.host));
        auto* itUser = new QTableWidgetItem(QString::fromStdString(sites_[i].opt.username));
        // Guardar índice original para que selección funcione aunque la vista esté ordenada
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
    // Guardar secretos
    SecretStore store;
    if (opt.password) store.setSecret(QString("site:%1:password").arg(name), QString::fromStdString(*opt.password));
    if (opt.private_key_passphrase) store.setSecret(QString("site:%1:keypass").arg(name), QString::fromStdString(*opt.private_key_passphrase));
}

void SiteManagerDialog::onEdit() {
    auto sel = table_->selectionModel();
    if (!sel || !sel->hasSelection()) return;
    int viewRow = sel->selectedRows().first().row();
    int modelIndex = table_->item(viewRow, 0) ? table_->item(viewRow, 0)->data(Qt::UserRole).toInt() : viewRow;
    if (modelIndex < 0 || modelIndex >= sites_.size()) return;
    SiteEntry e = sites_[modelIndex];
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
    QString name = QInputDialog::getText(this, tr("Nombre del sitio"), tr("Nombre:"), QLineEdit::Normal, e.name, &ok);
    if (!ok || name.isEmpty()) return;
    e.name = name;
    sites_[modelIndex] = e;
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
    int viewRow = sel->selectedRows().first().row();
    int modelIndex = table_->item(viewRow, 0) ? table_->item(viewRow, 0)->data(Qt::UserRole).toInt() : viewRow;
    if (modelIndex < 0 || modelIndex >= sites_.size()) return;
    sites_.remove(modelIndex);
    saveSites();
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
    // Rellenar secretos en tiempo de conexión
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
        // Compat: migrar antiguos valores desde QSettings si existen
        QSettings s("OpenSCP", "OpenSCP");
        int n = s.beginReadArray("sites");
        if (modelIndex >= 0 && modelIndex < n) {
            s.setArrayIndex(modelIndex);
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
