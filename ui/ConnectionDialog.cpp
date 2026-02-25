// Builds the connection form and exposes getters/setters for SessionOptions.
#include "ConnectionDialog.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QWidget>

ConnectionDialog::ConnectionDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Connect (SFTP)"));
    auto *lay = new QFormLayout(this);
    lay->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    siteName_ = new QLineEdit(this);
    host_ = new QLineEdit(this);
    port_ = new QSpinBox(this);
    user_ = new QLineEdit(this);
    pass_ = new QLineEdit(this);

    // Private key fields (optional)
    keyPath_ = new QLineEdit(this);
    keyPass_ = new QLineEdit(this);
    proxyHost_ = new QLineEdit(this);
    proxyPort_ = new QSpinBox(this);
    proxyUser_ = new QLineEdit(this);
    proxyPass_ = new QLineEdit(this);

    // Safer defaults: no implicit host/user values to avoid accidental
    // connections.
    siteName_->setPlaceholderText(tr("My server"));
    host_->setPlaceholderText(tr("sftp.example.com"));
    port_->setRange(1, 65535);
    port_->setValue(22);
    port_->setFixedWidth(110);
    port_->setToolTip(tr("SSH/SFTP port"));
    user_->setPlaceholderText(tr("user"));
    pass_->setPlaceholderText(tr("optional"));
    keyPath_->setPlaceholderText(tr("~/.ssh/id_ed25519"));
    keyPass_->setPlaceholderText(tr("optional"));
    proxyHost_->setPlaceholderText(tr("proxy.example.com"));
    proxyPort_->setRange(1, 65535);
    proxyPort_->setValue(1080);
    proxyPort_->setFixedWidth(110);
    proxyUser_->setPlaceholderText(tr("optional"));
    proxyPass_->setPlaceholderText(tr("optional"));

    // Make text inputs a bit wider by default for better readability.
    const int kInputMinWidth = 360;
    siteName_->setMinimumWidth(kInputMinWidth);
    host_->setMinimumWidth(kInputMinWidth);
    user_->setMinimumWidth(kInputMinWidth);
    pass_->setMinimumWidth(kInputMinWidth);
    keyPath_->setMinimumWidth(kInputMinWidth);
    keyPass_->setMinimumWidth(kInputMinWidth);
    proxyHost_->setMinimumWidth(kInputMinWidth);
    proxyUser_->setMinimumWidth(kInputMinWidth);
    proxyPass_->setMinimumWidth(kInputMinWidth);

    pass_->setEchoMode(QLineEdit::Password);
    keyPass_->setEchoMode(QLineEdit::Password);
    proxyPass_->setEchoMode(QLineEdit::Password);

    auto *passToggle = new QToolButton(this);
    passToggle->setText(tr("Show"));
    passToggle->setCheckable(true);
    connect(passToggle, &QToolButton::toggled, this,
            [this, passToggle](bool checked) {
                pass_->setEchoMode(checked ? QLineEdit::Normal
                                           : QLineEdit::Password);
                passToggle->setText(checked ? tr("Hide") : tr("Show"));
            });

    auto *keyPassToggle = new QToolButton(this);
    keyPassToggle->setText(tr("Show"));
    keyPassToggle->setCheckable(true);
    connect(keyPassToggle, &QToolButton::toggled, this,
            [this, keyPassToggle](bool checked) {
                keyPass_->setEchoMode(checked ? QLineEdit::Normal
                                              : QLineEdit::Password);
                keyPassToggle->setText(checked ? tr("Hide") : tr("Show"));
            });

    auto *proxyPassToggle = new QToolButton(this);
    proxyPassToggle->setText(tr("Show"));
    proxyPassToggle->setCheckable(true);
    connect(proxyPassToggle, &QToolButton::toggled, this,
            [this, proxyPassToggle](bool checked) {
                proxyPass_->setEchoMode(checked ? QLineEdit::Normal
                                                : QLineEdit::Password);
                proxyPassToggle->setText(checked ? tr("Hide") : tr("Show"));
            });

    auto *passRow = new QWidget(this);
    auto *passRowLayout = new QHBoxLayout(passRow);
    passRowLayout->setContentsMargins(0, 0, 0, 0);
    passRowLayout->setSpacing(6);
    passRowLayout->addWidget(pass_);
    passRowLayout->addWidget(passToggle);

    auto *keyPassRow = new QWidget(this);
    auto *keyPassRowLayout = new QHBoxLayout(keyPassRow);
    keyPassRowLayout->setContentsMargins(0, 0, 0, 0);
    keyPassRowLayout->setSpacing(6);
    keyPassRowLayout->addWidget(keyPass_);
    keyPassRowLayout->addWidget(keyPassToggle);

    auto *keyBrowseBtn = new QToolButton(this);
    keyBrowseBtn->setText(tr("Choose…"));

    auto *hostPortRow = new QWidget(this);
    auto *hostPortRowLayout = new QHBoxLayout(hostPortRow);
    hostPortRowLayout->setContentsMargins(0, 0, 0, 0);
    hostPortRowLayout->setSpacing(6);
    hostPortRowLayout->addWidget(host_, 1);
    hostPortRowLayout->addWidget(port_);

    auto *keyPathRow = new QWidget(this);
    auto *keyPathRowLayout = new QHBoxLayout(keyPathRow);
    keyPathRowLayout->setContentsMargins(0, 0, 0, 0);
    keyPathRowLayout->setSpacing(6);
    keyPathRowLayout->addWidget(keyPath_);
    keyPathRowLayout->addWidget(keyBrowseBtn);

    proxyType_ = new QComboBox(this);
    proxyType_->addItem(tr("Direct (no proxy)"),
                        static_cast<int>(openscp::ProxyType::None));
    proxyType_->addItem(tr("SOCKS5"),
                        static_cast<int>(openscp::ProxyType::Socks5));
    proxyType_->addItem(tr("HTTP CONNECT"),
                        static_cast<int>(openscp::ProxyType::HttpConnect));

    auto *proxyHostPortRow = new QWidget(this);
    auto *proxyHostPortLayout = new QHBoxLayout(proxyHostPortRow);
    proxyHostPortLayout->setContentsMargins(0, 0, 0, 0);
    proxyHostPortLayout->setSpacing(6);
    proxyHostPortLayout->addWidget(proxyHost_, 1);
    proxyHostPortLayout->addWidget(proxyPort_);

    auto *proxyPassRow = new QWidget(this);
    auto *proxyPassRowLayout = new QHBoxLayout(proxyPassRow);
    proxyPassRowLayout->setContentsMargins(0, 0, 0, 0);
    proxyPassRowLayout->setSpacing(6);
    proxyPassRowLayout->addWidget(proxyPass_);
    proxyPassRowLayout->addWidget(proxyPassToggle);

    // Layout
    lay->addRow(tr("Site name:"), siteName_);
    siteNameLabel_ = lay->labelForField(siteName_);
    setSiteNameVisible(false);
    saveSite_ = new QCheckBox(tr("Save to saved sites"), this);
    saveSite_->setChecked(true);
    saveCredentials_ =
        new QCheckBox(tr("Save passwords/passphrases"), this);
    saveCredentials_->setChecked(false);
    lay->addRow(QString(), saveSite_);
    lay->addRow(QString(), saveCredentials_);
    setQuickConnectSaveOptionsVisible(false);
    connect(saveSite_, &QCheckBox::toggled, this, [this](bool checked) {
        if (saveCredentials_) {
            saveCredentials_->setEnabled(checked);
            if (!checked)
                saveCredentials_->setChecked(false);
        }
        if (quickConnectSaveOptionsVisible_) {
            setSiteNameVisible(checked);
        }
    });
    lay->addRow(tr("Host / Port:"), hostPortRow);
    lay->addRow(tr("User:"), user_);
    lay->addRow(tr("Password:"), passRow);
    lay->addRow(tr("Private key path:"), keyPathRow);
    lay->addRow(tr("Key passphrase:"), keyPassRow);
    lay->addRow(tr("Proxy:"), proxyType_);
    lay->addRow(tr("Proxy host / port:"), proxyHostPortRow);
    lay->addRow(tr("Proxy user:"), proxyUser_);
    lay->addRow(tr("Proxy password:"), proxyPassRow);

    // known_hosts
    khPath_ = new QLineEdit(this);
    khPath_->setPlaceholderText(tr("~/.ssh/known_hosts"));
    khPath_->setMinimumWidth(kInputMinWidth);
    khPolicy_ = new QComboBox(this);
    khPolicy_->addItem(tr("Strict"),
                       static_cast<int>(openscp::KnownHostsPolicy::Strict));
    khPolicy_->addItem(tr("Accept new (TOFU)"),
                       static_cast<int>(openscp::KnownHostsPolicy::AcceptNew));
    khPolicy_->addItem(
        tr("No verification (double confirmation, expires in 15 min)"),
        static_cast<int>(openscp::KnownHostsPolicy::Off));
    auto *khPathRow = new QWidget(this);
    auto *khPathRowLayout = new QHBoxLayout(khPathRow);
    khPathRowLayout->setContentsMargins(0, 0, 0, 0);
    khPathRowLayout->setSpacing(6);
    khPathRowLayout->addWidget(khPath_);

    // Button to choose known_hosts
    khBrowse_ = new QToolButton(this);
    khBrowse_->setText(tr("Choose…"));
    khPathRowLayout->addWidget(khBrowse_);

    integrityPolicy_ = new QComboBox(this);
    integrityPolicy_->addItem(
        tr("Optional (recommended)"),
        static_cast<int>(openscp::TransferIntegrityPolicy::Optional));
    integrityPolicy_->addItem(
        tr("Required (strict)"),
        static_cast<int>(openscp::TransferIntegrityPolicy::Required));
    integrityPolicy_->addItem(
        tr("Off (not recommended)"),
        static_cast<int>(openscp::TransferIntegrityPolicy::Off));
    integrityPolicy_->setToolTip(tr(
        "Checksum verification for resume and final transfer validation."));

    lay->addRow(tr("known_hosts:"), khPathRow);
    lay->addRow(tr("Policy:"), khPolicy_);
    lay->addRow(tr("Integrity:"), integrityPolicy_);

    auto updateProxyFields = [this]() {
        const auto type = static_cast<openscp::ProxyType>(
            proxyType_->currentData().toInt());
        const bool enabled = (type != openscp::ProxyType::None);
        proxyHost_->setEnabled(enabled);
        proxyPort_->setEnabled(enabled);
        proxyUser_->setEnabled(enabled);
        proxyPass_->setEnabled(enabled);
        if (!enabled)
            proxyPort_->setValue(1080);
    };
    connect(proxyType_, &QComboBox::currentIndexChanged, this,
            [updateProxyFields](int) { updateProxyFields(); });
    updateProxyFields();

    connect(khBrowse_, &QToolButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, tr("Select known_hosts"), QDir::homePath() + "/.ssh");
        if (!f.isEmpty())
            khPath_->setText(f);
    });

    connect(keyBrowseBtn, &QToolButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, tr("Select private key"), QDir::homePath() + "/.ssh");
        if (!f.isEmpty())
            keyPath_->setText(f);
    });

    auto *bb = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Initial focus to guide users to provide an explicit target host.
    QTimer::singleShot(0, host_, [this] {
        if (host_)
            host_->setFocus(Qt::OtherFocusReason);
    });
}

void ConnectionDialog::setSiteNameVisible(bool visible) {
    if (siteName_)
        siteName_->setVisible(visible);
    if (siteNameLabel_)
        siteNameLabel_->setVisible(visible);
}

void ConnectionDialog::setSiteName(const QString &name) {
    if (siteName_)
        siteName_->setText(name);
}

QString ConnectionDialog::siteName() const {
    return siteName_ ? siteName_->text() : QString();
}

void ConnectionDialog::setQuickConnectSaveOptionsVisible(bool visible) {
    quickConnectSaveOptionsVisible_ = visible;
    if (saveSite_)
        saveSite_->setVisible(visible);
    if (saveCredentials_) {
        saveCredentials_->setVisible(visible);
        saveCredentials_->setEnabled(saveSite_ && saveSite_->isChecked());
    }
    if (visible) {
        setSiteNameVisible(saveSite_ && saveSite_->isChecked());
    }
}

bool ConnectionDialog::saveSiteRequested() const {
    return quickConnectSaveOptionsVisible_ && saveSite_ &&
           saveSite_->isChecked();
}

bool ConnectionDialog::saveCredentialsRequested() const {
    return quickConnectSaveOptionsVisible_ && saveCredentials_ &&
           saveCredentials_->isChecked();
}

openscp::SessionOptions ConnectionDialog::options() const {
    openscp::SessionOptions o;
    o.host = host_->text().toStdString();
    o.port = static_cast<std::uint16_t>(port_->value());
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
    o.known_hosts_policy = static_cast<openscp::KnownHostsPolicy>(
        khPolicy_->currentData().toInt());
    if (integrityPolicy_) {
        o.transfer_integrity_policy =
            static_cast<openscp::TransferIntegrityPolicy>(
                integrityPolicy_->currentData().toInt());
    }
    if (proxyType_) {
        o.proxy_type = static_cast<openscp::ProxyType>(
            proxyType_->currentData().toInt());
    }
    if (o.proxy_type != openscp::ProxyType::None) {
        o.proxy_host = proxyHost_->text().trimmed().toStdString();
        o.proxy_port = static_cast<std::uint16_t>(proxyPort_->value());
        if (!proxyUser_->text().isEmpty())
            o.proxy_username = proxyUser_->text().toStdString();
        if (!proxyPass_->text().isEmpty())
            o.proxy_password = proxyPass_->text().toUtf8().toStdString();
    }

    return o;
}

void ConnectionDialog::setOptions(const openscp::SessionOptions &o) {
    if (!o.host.empty())
        host_->setText(QString::fromStdString(o.host));
    if (o.port)
        port_->setValue((int)o.port);
    if (!o.username.empty())
        user_->setText(QString::fromStdString(o.username));
    if (o.password && !o.password->empty())
        pass_->setText(QString::fromStdString(*o.password));
    if (o.private_key_path && !o.private_key_path->empty())
        keyPath_->setText(QString::fromStdString(*o.private_key_path));
    if (o.private_key_passphrase && !o.private_key_passphrase->empty())
        keyPass_->setText(QString::fromStdString(*o.private_key_passphrase));
    if (o.known_hosts_path && !o.known_hosts_path->empty())
        khPath_->setText(QString::fromStdString(*o.known_hosts_path));
    // Policy
    int idx = khPolicy_->findData(static_cast<int>(o.known_hosts_policy));
    if (idx >= 0)
        khPolicy_->setCurrentIndex(idx);
    if (integrityPolicy_) {
        int i = integrityPolicy_->findData(
            static_cast<int>(o.transfer_integrity_policy));
        if (i >= 0)
            integrityPolicy_->setCurrentIndex(i);
    }
    if (proxyType_) {
        int i = proxyType_->findData(static_cast<int>(o.proxy_type));
        if (i >= 0)
            proxyType_->setCurrentIndex(i);
    }
    if (!o.proxy_host.empty())
        proxyHost_->setText(QString::fromStdString(o.proxy_host));
    if (o.proxy_port != 0)
        proxyPort_->setValue(static_cast<int>(o.proxy_port));
    if (o.proxy_username && !o.proxy_username->empty())
        proxyUser_->setText(QString::fromStdString(*o.proxy_username));
    if (o.proxy_password && !o.proxy_password->empty())
        proxyPass_->setText(QString::fromStdString(*o.proxy_password));
}
