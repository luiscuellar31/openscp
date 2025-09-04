// Construye el formulario de conexión y expone getters/setters de SessionOptions.
#include "ConnectionDialog.hpp"
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QFileDialog>
#include <QComboBox>
#include <QPushButton>
#include <QDir>

ConnectionDialog::ConnectionDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Conectar (SFTP)");
    auto* lay = new QFormLayout(this);

    host_ = new QLineEdit(this);
    port_ = new QSpinBox(this);
    user_ = new QLineEdit(this);
    pass_ = new QLineEdit(this);

    // NUEVO: campos de clave privada (opcionales)
    keyPath_ = new QLineEdit(this);
    keyPass_ = new QLineEdit(this);

    // Defaults útiles
    host_->setText("localhost");
    port_->setRange(1, 65535);
    port_->setValue(22);
    user_->setText(QString::fromLocal8Bit(qgetenv("USER")));

    pass_->setEchoMode(QLineEdit::Password);
    keyPass_->setEchoMode(QLineEdit::Password);

    // Layout
    lay->addRow("Host:", host_);
    lay->addRow("Puerto:", port_);
    lay->addRow("Usuario:", user_);
    lay->addRow("Contraseña:", pass_);
    lay->addRow("Ruta clave privada:", keyPath_);
    lay->addRow("Passphrase clave:", keyPass_);

    // known_hosts
    khPath_ = new QLineEdit(this);
    khPolicy_ = new QComboBox(this);
    khPolicy_->addItem("Estricto", static_cast<int>(openscp::KnownHostsPolicy::Strict));
    khPolicy_->addItem("Aceptar nuevo (TOFU)", static_cast<int>(openscp::KnownHostsPolicy::AcceptNew));
    khPolicy_->addItem("Sin verificación (no recomendado)", static_cast<int>(openscp::KnownHostsPolicy::Off));
    lay->addRow("known_hosts:", khPath_);
    lay->addRow("Política:", khPolicy_);

    // Botón para elegir known_hosts
    khBrowse_ = new QPushButton("Elegir known_hosts…", this);
    lay->addRow("", khBrowse_);
    connect(khBrowse_, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, "Selecciona known_hosts", QDir::homePath() + "/.ssh");
        if (!f.isEmpty()) khPath_->setText(f);
    });

    // (Opcional) botón para elegir archivo de clave privada
    // Si lo quieres, descomenta estas 6 líneas:
    
    auto* browseBtn = new QPushButton("Elegir clave…", this);
    lay->addRow("", browseBtn);
    connect(browseBtn, &QPushButton::clicked, this, [this]{
        const QString f = QFileDialog::getOpenFileName(this, "Selecciona clave privada", QDir::homePath() + "/.ssh");
        if (!f.isEmpty()) keyPath_->setText(f);
    });
    

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

openscp::SessionOptions ConnectionDialog::options() const {
    openscp::SessionOptions o;
    o.host     = host_->text().toStdString();
    o.port     = static_cast<std::uint16_t>(port_->value());
    o.username = user_->text().toStdString();

    // Password (si se escribió)
    if (!pass_->text().isEmpty())
        o.password = pass_->text().toUtf8().toStdString();

    // Clave privada (si se escribió)
    if (!keyPath_->text().isEmpty())
        o.private_key_path = keyPath_->text().toStdString();

    // Passphrase de la clave (si se escribió)
    if (!keyPass_->text().isEmpty())
        o.private_key_passphrase = keyPass_->text().toUtf8().toStdString();

    // known_hosts
    if (!khPath_->text().isEmpty())
        o.known_hosts_path = khPath_->text().toStdString();
    o.known_hosts_policy = static_cast<openscp::KnownHostsPolicy>(khPolicy_->currentData().toInt());

    return o;
}

void ConnectionDialog::setOptions(const openscp::SessionOptions& o) {
    if (!o.host.empty()) host_->setText(QString::fromStdString(o.host));
    if (o.port) port_->setValue((int)o.port);
    if (!o.username.empty()) user_->setText(QString::fromStdString(o.username));
    if (o.password && !o.password->empty()) pass_->setText(QString::fromStdString(*o.password));
    if (o.private_key_path && !o.private_key_path->empty()) keyPath_->setText(QString::fromStdString(*o.private_key_path));
    if (o.private_key_passphrase && !o.private_key_passphrase->empty()) keyPass_->setText(QString::fromStdString(*o.private_key_passphrase));
    if (o.known_hosts_path && !o.known_hosts_path->empty()) khPath_->setText(QString::fromStdString(*o.known_hosts_path));
    // Política
    int idx = khPolicy_->findData(static_cast<int>(o.known_hosts_policy));
    if (idx >= 0) khPolicy_->setCurrentIndex(idx);
}
