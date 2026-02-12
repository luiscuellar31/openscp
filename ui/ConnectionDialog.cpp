// Builds the connection form and exposes getters/setters for SessionOptions.
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
    setWindowTitle(tr("Conectar (SFTP)"));
    auto* lay = new QFormLayout(this);

    host_ = new QLineEdit(this);
    port_ = new QSpinBox(this);
    user_ = new QLineEdit(this);
    pass_ = new QLineEdit(this);

    // Private key fields (optional)
    keyPath_ = new QLineEdit(this);
    keyPass_ = new QLineEdit(this);

    // Useful defaults
    host_->setText("localhost");
    port_->setRange(1, 65535);
    port_->setValue(22);
    user_->setText(QString::fromLocal8Bit(qgetenv("USER")));

    pass_->setEchoMode(QLineEdit::Password);
    keyPass_->setEchoMode(QLineEdit::Password);

    // Layout
    lay->addRow(tr("Host:"), host_);
    lay->addRow(tr("Puerto:"), port_);
    lay->addRow(tr("Usuario:"), user_);
    lay->addRow(tr("Contraseña:"), pass_);
    lay->addRow(tr("Ruta clave privada:"), keyPath_);
    lay->addRow(tr("Passphrase clave:"), keyPass_);

    // known_hosts
    khPath_ = new QLineEdit(this);
    khPolicy_ = new QComboBox(this);
    khPolicy_->addItem(tr("Estricto"), static_cast<int>(openscp::KnownHostsPolicy::Strict));
    khPolicy_->addItem(tr("Aceptar nuevo (TOFU)"), static_cast<int>(openscp::KnownHostsPolicy::AcceptNew));
    khPolicy_->addItem(tr("Sin verificación (doble confirmación, expira en 15 min)"),
                       static_cast<int>(openscp::KnownHostsPolicy::Off));
    lay->addRow(tr("known_hosts:"), khPath_);
    lay->addRow(tr("Política:"), khPolicy_);

    // Button to choose known_hosts
    khBrowse_ = new QPushButton(tr("Elegir known_hosts…"), this);
    lay->addRow("", khBrowse_);
    connect(khBrowse_, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Selecciona known_hosts"), QDir::homePath() + "/.ssh");
        if (!f.isEmpty()) khPath_->setText(f);
    });

    // Optional: button to choose private key file
    
    auto* browseBtn = new QPushButton(tr("Elegir clave…"), this);
    lay->addRow("", browseBtn);
    connect(browseBtn, &QPushButton::clicked, this, [this]{
        const QString f = QFileDialog::getOpenFileName(this, tr("Selecciona clave privada"), QDir::homePath() + "/.ssh");
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

    // Password (if provided)
    if (!pass_->text().isEmpty())
        o.password = pass_->text().toUtf8().toStdString();

    // Private key path (if provided)
    if (!keyPath_->text().isEmpty())
        o.private_key_path = keyPath_->text().toStdString();

    // Key passphrase (if provided)
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
    // Policy
    int idx = khPolicy_->findData(static_cast<int>(o.known_hosts_policy));
    if (idx >= 0) khPolicy_->setCurrentIndex(idx);
}
