// Builds the connection form and exposes getters/setters for SessionOptions.
#include "ConnectionDialog.hpp"
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QFileDialog>
#include <QComboBox>
#include <QPushButton>
#include <QToolButton>
#include <QWidget>
#include <QDir>
#include <QTimer>

ConnectionDialog::ConnectionDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Conectar (SFTP)"));
    auto* lay = new QFormLayout(this);
    lay->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    siteName_ = new QLineEdit(this);
    host_ = new QLineEdit(this);
    port_ = new QSpinBox(this);
    user_ = new QLineEdit(this);
    pass_ = new QLineEdit(this);

    // Private key fields (optional)
    keyPath_ = new QLineEdit(this);
    keyPass_ = new QLineEdit(this);

    // Safer defaults: no implicit host/user values to avoid accidental connections.
    siteName_->setPlaceholderText(tr("Mi servidor"));
    host_->setPlaceholderText(tr("sftp.ejemplo.com"));
    port_->setRange(1, 65535);
    port_->setValue(22);
    port_->setFixedWidth(110);
    port_->setToolTip(tr("Puerto SSH/SFTP"));
    user_->setPlaceholderText(tr("usuario"));
    pass_->setPlaceholderText(tr("opcional"));
    keyPath_->setPlaceholderText(tr("~/.ssh/id_ed25519"));
    keyPass_->setPlaceholderText(tr("opcional"));

    // Make text inputs a bit wider by default for better readability.
    const int kInputMinWidth = 360;
    siteName_->setMinimumWidth(kInputMinWidth);
    host_->setMinimumWidth(kInputMinWidth);
    user_->setMinimumWidth(kInputMinWidth);
    pass_->setMinimumWidth(kInputMinWidth);
    keyPath_->setMinimumWidth(kInputMinWidth);
    keyPass_->setMinimumWidth(kInputMinWidth);

    pass_->setEchoMode(QLineEdit::Password);
    keyPass_->setEchoMode(QLineEdit::Password);

    auto* passToggle = new QToolButton(this);
    passToggle->setText(tr("Mostrar"));
    passToggle->setCheckable(true);
    connect(passToggle, &QToolButton::toggled, this, [this, passToggle](bool checked) {
        pass_->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
        passToggle->setText(checked ? tr("Ocultar") : tr("Mostrar"));
    });

    auto* keyPassToggle = new QToolButton(this);
    keyPassToggle->setText(tr("Mostrar"));
    keyPassToggle->setCheckable(true);
    connect(keyPassToggle, &QToolButton::toggled, this, [this, keyPassToggle](bool checked) {
        keyPass_->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
        keyPassToggle->setText(checked ? tr("Ocultar") : tr("Mostrar"));
    });

    auto* passRow = new QWidget(this);
    auto* passRowLayout = new QHBoxLayout(passRow);
    passRowLayout->setContentsMargins(0, 0, 0, 0);
    passRowLayout->setSpacing(6);
    passRowLayout->addWidget(pass_);
    passRowLayout->addWidget(passToggle);

    auto* keyPassRow = new QWidget(this);
    auto* keyPassRowLayout = new QHBoxLayout(keyPassRow);
    keyPassRowLayout->setContentsMargins(0, 0, 0, 0);
    keyPassRowLayout->setSpacing(6);
    keyPassRowLayout->addWidget(keyPass_);
    keyPassRowLayout->addWidget(keyPassToggle);

    auto* keyBrowseBtn = new QToolButton(this);
    keyBrowseBtn->setText(tr("Elegir…"));

    auto* hostPortRow = new QWidget(this);
    auto* hostPortRowLayout = new QHBoxLayout(hostPortRow);
    hostPortRowLayout->setContentsMargins(0, 0, 0, 0);
    hostPortRowLayout->setSpacing(6);
    hostPortRowLayout->addWidget(host_, 1);
    hostPortRowLayout->addWidget(port_);

    auto* keyPathRow = new QWidget(this);
    auto* keyPathRowLayout = new QHBoxLayout(keyPathRow);
    keyPathRowLayout->setContentsMargins(0, 0, 0, 0);
    keyPathRowLayout->setSpacing(6);
    keyPathRowLayout->addWidget(keyPath_);
    keyPathRowLayout->addWidget(keyBrowseBtn);

    // Layout
    lay->addRow(tr("Nombre del sitio:"), siteName_);
    siteNameLabel_ = lay->labelForField(siteName_);
    setSiteNameVisible(false);
    saveSite_ = new QCheckBox(tr("Guardar en sitios guardados"), this);
    saveSite_->setChecked(true);
    saveCredentials_ = new QCheckBox(tr("Guardar contraseña/passphrase"), this);
    saveCredentials_->setChecked(false);
    lay->addRow(QString(), saveSite_);
    lay->addRow(QString(), saveCredentials_);
    setQuickConnectSaveOptionsVisible(false);
    connect(saveSite_, &QCheckBox::toggled, this, [this](bool checked) {
        if (saveCredentials_) {
            saveCredentials_->setEnabled(checked);
            if (!checked) saveCredentials_->setChecked(false);
        }
        if (quickConnectSaveOptionsVisible_) {
            setSiteNameVisible(checked);
        }
    });
    lay->addRow(tr("Host / Puerto:"), hostPortRow);
    lay->addRow(tr("Usuario:"), user_);
    lay->addRow(tr("Contraseña:"), passRow);
    lay->addRow(tr("Ruta clave privada:"), keyPathRow);
    lay->addRow(tr("Passphrase clave:"), keyPassRow);

    // known_hosts
    khPath_ = new QLineEdit(this);
    khPath_->setPlaceholderText(tr("~/.ssh/known_hosts"));
    khPath_->setMinimumWidth(kInputMinWidth);
    khPolicy_ = new QComboBox(this);
    khPolicy_->addItem(tr("Estricto"), static_cast<int>(openscp::KnownHostsPolicy::Strict));
    khPolicy_->addItem(tr("Aceptar nuevo (TOFU)"), static_cast<int>(openscp::KnownHostsPolicy::AcceptNew));
    khPolicy_->addItem(tr("Sin verificación (doble confirmación, expira en 15 min)"),
                       static_cast<int>(openscp::KnownHostsPolicy::Off));
    auto* khPathRow = new QWidget(this);
    auto* khPathRowLayout = new QHBoxLayout(khPathRow);
    khPathRowLayout->setContentsMargins(0, 0, 0, 0);
    khPathRowLayout->setSpacing(6);
    khPathRowLayout->addWidget(khPath_);

    // Button to choose known_hosts
    khBrowse_ = new QToolButton(this);
    khBrowse_->setText(tr("Elegir…"));
    khPathRowLayout->addWidget(khBrowse_);

    lay->addRow(tr("known_hosts:"), khPathRow);
    lay->addRow(tr("Política:"), khPolicy_);

    connect(khBrowse_, &QToolButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Selecciona known_hosts"), QDir::homePath() + "/.ssh");
        if (!f.isEmpty()) khPath_->setText(f);
    });

    connect(keyBrowseBtn, &QToolButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Selecciona clave privada"), QDir::homePath() + "/.ssh");
        if (!f.isEmpty()) keyPath_->setText(f);
    });

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Initial focus to guide users to provide an explicit target host.
    QTimer::singleShot(0, host_, [this] { if (host_) host_->setFocus(Qt::OtherFocusReason); });
}

void ConnectionDialog::setSiteNameVisible(bool visible) {
    if (siteName_) siteName_->setVisible(visible);
    if (siteNameLabel_) siteNameLabel_->setVisible(visible);
}

void ConnectionDialog::setSiteName(const QString& name) {
    if (siteName_) siteName_->setText(name);
}

QString ConnectionDialog::siteName() const {
    return siteName_ ? siteName_->text() : QString();
}

void ConnectionDialog::setQuickConnectSaveOptionsVisible(bool visible) {
    quickConnectSaveOptionsVisible_ = visible;
    if (saveSite_) saveSite_->setVisible(visible);
    if (saveCredentials_) {
        saveCredentials_->setVisible(visible);
        saveCredentials_->setEnabled(saveSite_ && saveSite_->isChecked());
    }
    if (visible) {
        setSiteNameVisible(saveSite_ && saveSite_->isChecked());
    }
}

bool ConnectionDialog::saveSiteRequested() const {
    return quickConnectSaveOptionsVisible_ && saveSite_ && saveSite_->isChecked();
}

bool ConnectionDialog::saveCredentialsRequested() const {
    return quickConnectSaveOptionsVisible_ && saveCredentials_ && saveCredentials_->isChecked();
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
