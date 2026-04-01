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
#include <QSettings>
#include <QSignalBlocker>
#include <QtGlobal>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QWidget>

static void setFormRowVisible(QFormLayout *layout, QWidget *field,
                              bool visible) {
    if (!layout || !field)
        return;
    if (QWidget *label = layout->labelForField(field))
        label->setVisible(visible);
    field->setVisible(visible);
}

ConnectionDialog::ConnectionDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Connect"));
    auto *lay = new QFormLayout(this);
    formLayout_ = lay;
    lay->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    protocol_ = new QComboBox(this);
    protocol_->addItem(tr("SFTP"),
                       static_cast<int>(openscp::Protocol::Sftp));
    protocol_->addItem(tr("SCP"), static_cast<int>(openscp::Protocol::Scp));
    protocol_->addItem(tr("FTP"), static_cast<int>(openscp::Protocol::Ftp));
    protocol_->addItem(tr("FTPS"),
                       static_cast<int>(openscp::Protocol::Ftps));
    scpMode_ = new QComboBox(this);
    scpMode_->addItem(
        tr("Automatic (SCP with SFTP fallback)"),
        static_cast<int>(openscp::ScpTransferMode::Auto));
    scpMode_->addItem(
        tr("SCP only (disable SFTP fallback)"),
        static_cast<int>(openscp::ScpTransferMode::ScpOnly));
    {
        QSettings s("OpenSCP", "OpenSCP");
        const auto defaultProtocol = openscp::protocolFromStorageName(
            s.value("Protocol/defaultProtocol",
                    QString::fromLatin1(openscp::protocolStorageName(
                        openscp::Protocol::Sftp)))
                .toString()
                .trimmed()
                .toLower()
                .toStdString());
        const int pidx =
            protocol_->findData(static_cast<int>(defaultProtocol));
        if (pidx >= 0)
            protocol_->setCurrentIndex(pidx);

        const auto defaultMode = openscp::scpTransferModeFromStorageName(
            s.value("Protocol/scpTransferModeDefault",
                    QString::fromLatin1(openscp::scpTransferModeStorageName(
                        openscp::ScpTransferMode::Auto)))
                .toString()
                .trimmed()
                .toLower()
                .toStdString());
        const int i = scpMode_->findData(static_cast<int>(defaultMode));
        if (i >= 0)
            scpMode_->setCurrentIndex(i);
    }

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
    jumpEnabled_ = new QCheckBox(tr("Use SSH jump host (bastion)"), this);
    jumpHost_ = new QLineEdit(this);
    jumpPort_ = new QSpinBox(this);
    jumpUser_ = new QLineEdit(this);
    jumpKeyPath_ = new QLineEdit(this);
    jumpKeyBrowse_ = new QToolButton(this);

    // Safer defaults: no implicit host/user values to avoid accidental
    // connections.
    siteName_->setPlaceholderText(tr("My server"));
    port_->setRange(1, 65535);
    port_->setFixedWidth(110);
    port_->setToolTip(tr("Server port for the selected protocol"));
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
    jumpHost_->setPlaceholderText(tr("bastion.example.com"));
    jumpPort_->setRange(1, 65535);
    jumpPort_->setValue(22);
    jumpPort_->setFixedWidth(110);
    jumpUser_->setPlaceholderText(tr("optional"));
    jumpKeyPath_->setPlaceholderText(tr("optional"));
    jumpKeyBrowse_->setText(tr("Choose…"));
#ifdef Q_OS_WIN
    jumpEnabled_->setChecked(false);
    jumpEnabled_->setEnabled(false);
    jumpEnabled_->setToolTip(
        tr("SSH jump host is currently unavailable on Windows."));
#endif

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
    jumpHost_->setMinimumWidth(kInputMinWidth);
    jumpUser_->setMinimumWidth(kInputMinWidth);
    jumpKeyPath_->setMinimumWidth(kInputMinWidth);

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

    auto *jumpHostPortRow = new QWidget(this);
    auto *jumpHostPortLayout = new QHBoxLayout(jumpHostPortRow);
    jumpHostPortLayout->setContentsMargins(0, 0, 0, 0);
    jumpHostPortLayout->setSpacing(6);
    jumpHostPortLayout->addWidget(jumpHost_, 1);
    jumpHostPortLayout->addWidget(jumpPort_);

    auto *jumpKeyPathRow = new QWidget(this);
    auto *jumpKeyPathLayout = new QHBoxLayout(jumpKeyPathRow);
    jumpKeyPathLayout->setContentsMargins(0, 0, 0, 0);
    jumpKeyPathLayout->setSpacing(6);
    jumpKeyPathLayout->addWidget(jumpKeyPath_);
    jumpKeyPathLayout->addWidget(jumpKeyBrowse_);

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
    lay->addRow(tr("Protocol:"), protocol_);
    lay->addRow(tr("SCP mode:"), scpMode_);
    lay->addRow(tr("Host / Port:"), hostPortRow);
    lay->addRow(tr("User:"), user_);
    lay->addRow(tr("Password:"), passRow);
    lay->addRow(tr("Private key path:"), keyPathRow);
    lay->addRow(tr("Key passphrase:"), keyPassRow);
    lay->addRow(tr("Proxy:"), proxyType_);
    lay->addRow(tr("Proxy host / port:"), proxyHostPortRow);
    lay->addRow(tr("Proxy user:"), proxyUser_);
    lay->addRow(tr("Proxy password:"), proxyPassRow);
    lay->addRow(QString(), jumpEnabled_);
    lay->addRow(tr("Jump host / port:"), jumpHostPortRow);
    lay->addRow(tr("Jump user:"), jumpUser_);
    lay->addRow(tr("Jump private key:"), jumpKeyPathRow);

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
    ftpsVerifyPeer_ =
        new QCheckBox(tr("Verify FTPS server certificate (recommended)"), this);
    ftpsCaPath_ = new QLineEdit(this);
    ftpsCaPath_->setPlaceholderText(tr("System CA bundle"));
    ftpsCaPath_->setMinimumWidth(kInputMinWidth);
    ftpsCaBrowse_ = new QToolButton(this);
    ftpsCaBrowse_->setText(tr("Choose…"));
    ftpsCaPathRow_ = new QWidget(this);
    auto *ftpsCaRowLayout = new QHBoxLayout(ftpsCaPathRow_);
    ftpsCaRowLayout->setContentsMargins(0, 0, 0, 0);
    ftpsCaRowLayout->setSpacing(6);
    ftpsCaRowLayout->addWidget(ftpsCaPath_);
    ftpsCaRowLayout->addWidget(ftpsCaBrowse_);
    {
        QSettings s("OpenSCP", "OpenSCP");
        int khPolicyIdx = khPolicy_->findData(
            s.value("Security/defaultKnownHostsPolicy",
                    static_cast<int>(openscp::KnownHostsPolicy::Strict))
                .toInt());
        if (khPolicyIdx < 0) {
            khPolicyIdx = khPolicy_->findData(
                static_cast<int>(openscp::KnownHostsPolicy::Strict));
        }
        if (khPolicyIdx >= 0)
            khPolicy_->setCurrentIndex(khPolicyIdx);

        int integrityIdx = integrityPolicy_->findData(
            s.value("Security/defaultTransferIntegrityPolicy",
                    static_cast<int>(openscp::TransferIntegrityPolicy::Optional))
                .toInt());
        if (integrityIdx < 0) {
            integrityIdx = integrityPolicy_->findData(
                static_cast<int>(
                    openscp::TransferIntegrityPolicy::Optional));
        }
        if (integrityIdx >= 0)
            integrityPolicy_->setCurrentIndex(integrityIdx);

        if (ftpsVerifyPeer_) {
            ftpsVerifyPeer_->setChecked(
                s.value("Security/ftpsVerifyPeerDefault", true).toBool());
        }
        if (ftpsCaPath_) {
            ftpsCaPath_->setText(
                s.value("Security/ftpsCaCertPathDefault", QString())
                    .toString()
                    .trimmed());
        }
    }

    lay->addRow(tr("known_hosts:"), khPathRow);
    lay->addRow(tr("Policy:"), khPolicy_);
    lay->addRow(tr("Integrity:"), integrityPolicy_);
    lay->addRow(QString(), ftpsVerifyPeer_);
    lay->addRow(tr("FTPS CA bundle:"), ftpsCaPathRow_);

    auto updateProxyFields = [this, lay, proxyHostPortRow, proxyPassRow]() {
        const auto type = static_cast<openscp::ProxyType>(
            proxyType_->currentData().toInt());
        const bool showProxyRows = (type != openscp::ProxyType::None);
        if (showProxyRows && !proxyRowsVisible_) {
            directModeSize_ = size();
            hasDirectModeSize_ = true;
        }
        setFormRowVisible(lay, proxyHostPortRow, showProxyRows);
        setFormRowVisible(lay, proxyUser_, showProxyRows);
        setFormRowVisible(lay, proxyPassRow, showProxyRows);
        proxyHost_->setEnabled(showProxyRows);
        proxyPort_->setEnabled(showProxyRows);
        proxyUser_->setEnabled(showProxyRows);
        proxyPass_->setEnabled(showProxyRows);
        if (!showProxyRows)
            proxyPort_->setValue(1080);
        const bool visibilityChanged = (showProxyRows != proxyRowsVisible_);
        proxyRowsVisible_ = showProxyRows;
        if (visibilityChanged) {
            if (layout())
                layout()->activate();
            if (showProxyRows) {
                adjustSize();
            } else if (hasDirectModeSize_) {
                resize(directModeSize_);
            } else {
                adjustSize();
            }
        }
    };
    connect(proxyType_, &QComboBox::currentIndexChanged, this,
            [this, updateProxyFields](int) {
                const auto type = static_cast<openscp::ProxyType>(
                    proxyType_->currentData().toInt());
                if (type != openscp::ProxyType::None && jumpEnabled_ &&
                    jumpEnabled_->isChecked()) {
                    jumpEnabled_->setChecked(false);
                }
                updateProxyFields();
            });
    updateProxyFields();

    connect(protocol_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!protocol_)
            return;
        const auto protocol = static_cast<openscp::Protocol>(
            protocol_->currentData().toInt());
        updateProtocolUi(protocol);
    });
    const auto initialProtocol =
        protocol_ ? static_cast<openscp::Protocol>(protocol_->currentData().toInt())
                  : openscp::Protocol::Sftp;
    updateProtocolUi(initialProtocol);

    auto updateJumpFields = [this, lay, jumpHostPortRow, jumpKeyPathRow]() {
        const bool showJumpRows =
            jumpEnabled_ && jumpEnabled_->isEnabled() &&
            jumpEnabled_->isChecked();
        setFormRowVisible(lay, jumpHostPortRow, showJumpRows);
        setFormRowVisible(lay, jumpUser_, showJumpRows);
        setFormRowVisible(lay, jumpKeyPathRow, showJumpRows);
        jumpHost_->setEnabled(showJumpRows);
        jumpPort_->setEnabled(showJumpRows);
        jumpUser_->setEnabled(showJumpRows);
        jumpKeyPath_->setEnabled(showJumpRows);
        jumpKeyBrowse_->setEnabled(showJumpRows);
        const bool visibilityChanged = (showJumpRows != jumpRowsVisible_);
        jumpRowsVisible_ = showJumpRows;
        if (visibilityChanged) {
            if (layout())
                layout()->activate();
            adjustSize();
        }
    };
    connect(jumpEnabled_, &QCheckBox::toggled, this,
            [this, updateJumpFields](bool checked) {
                if (checked && proxyType_) {
                    const int directIdx = proxyType_->findData(
                        static_cast<int>(openscp::ProxyType::None));
                    if (directIdx >= 0 && proxyType_->currentIndex() != directIdx)
                        proxyType_->setCurrentIndex(directIdx);
                }
                updateJumpFields();
            });
    updateJumpFields();

    connect(khBrowse_, &QToolButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, tr("Select known_hosts"), QDir::homePath() + "/.ssh");
        if (!f.isEmpty())
            khPath_->setText(f);
    });

    connect(ftpsCaBrowse_, &QToolButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, tr("Select FTPS CA bundle"), QDir::homePath());
        if (!f.isEmpty())
            ftpsCaPath_->setText(f);
    });

    connect(keyBrowseBtn, &QToolButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, tr("Select private key"), QDir::homePath() + "/.ssh");
        if (!f.isEmpty())
            keyPath_->setText(f);
    });

    connect(jumpKeyBrowse_, &QToolButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, tr("Select jump private key"), QDir::homePath() + "/.ssh");
        if (!f.isEmpty())
            jumpKeyPath_->setText(f);
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
    if (protocol_) {
        o.protocol =
            static_cast<openscp::Protocol>(protocol_->currentData().toInt());
    }
    if (scpMode_) {
        o.scp_transfer_mode = static_cast<openscp::ScpTransferMode>(
            scpMode_->currentData().toInt());
    }
    if (o.protocol != openscp::Protocol::Scp)
        o.scp_transfer_mode = openscp::ScpTransferMode::Auto;
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
    if (ftpsVerifyPeer_) {
        o.ftps_verify_peer = ftpsVerifyPeer_->isChecked();
    }
    if (ftpsCaPath_ && !ftpsCaPath_->text().trimmed().isEmpty()) {
        o.ftps_ca_cert_path = ftpsCaPath_->text().trimmed().toStdString();
    }
    if (proxyType_) {
        o.proxy_type =
            static_cast<openscp::ProxyType>(proxyType_->currentData().toInt());
    }
    const bool useJump = jumpEnabled_ && jumpEnabled_->isEnabled() &&
                         jumpEnabled_->isChecked() &&
                         !jumpHost_->text().trimmed().isEmpty();
    const bool useProxy =
        (o.proxy_type != openscp::ProxyType::None) && !useJump;
    if (useProxy) {
        o.proxy_host = proxyHost_->text().trimmed().toStdString();
        o.proxy_port = static_cast<std::uint16_t>(proxyPort_->value());
        if (!proxyUser_->text().isEmpty())
            o.proxy_username = proxyUser_->text().toStdString();
        if (!proxyPass_->text().isEmpty())
            o.proxy_password = proxyPass_->text().toUtf8().toStdString();
    }
    if (useJump) {
        o.proxy_type = openscp::ProxyType::None;
        o.jump_host = jumpHost_->text().trimmed().toStdString();
        o.jump_port = static_cast<std::uint16_t>(jumpPort_->value());
        if (!jumpUser_->text().isEmpty())
            o.jump_username = jumpUser_->text().toStdString();
        if (!jumpKeyPath_->text().isEmpty())
            o.jump_private_key_path = jumpKeyPath_->text().toStdString();
    }
    if (o.protocol != openscp::Protocol::Ftps) {
        o.ftps_verify_peer = true;
        o.ftps_ca_cert_path.reset();
    }

    return o;
}

void ConnectionDialog::setOptions(const openscp::SessionOptions &o) {
    openscp::Protocol effectiveProtocol = o.protocol;
    if (protocol_) {
        int pidx = protocol_->findData(static_cast<int>(o.protocol));
        if (pidx >= 0) {
            QSignalBlocker block(protocol_);
            protocol_->setCurrentIndex(pidx);
        } else {
            effectiveProtocol = openscp::Protocol::Sftp;
        }
        updateProtocolUi(effectiveProtocol);
    }
    if (scpMode_) {
        const int modeIdx =
            scpMode_->findData(static_cast<int>(o.scp_transfer_mode));
        if (modeIdx >= 0)
            scpMode_->setCurrentIndex(modeIdx);
    }
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
    if (ftpsVerifyPeer_)
        ftpsVerifyPeer_->setChecked(o.ftps_verify_peer);
    if (ftpsCaPath_) {
        if (o.ftps_ca_cert_path && !o.ftps_ca_cert_path->empty()) {
            ftpsCaPath_->setText(QString::fromStdString(*o.ftps_ca_cert_path));
        } else {
            ftpsCaPath_->clear();
        }
    }

    bool jumpSupportedInUi = true;
#ifdef Q_OS_WIN
    jumpSupportedInUi = false;
#endif
    const bool hasJump =
        jumpSupportedInUi && o.jump_host.has_value() && !o.jump_host->empty();
    const openscp::ProxyType effectiveProxyType =
        hasJump ? openscp::ProxyType::None : o.proxy_type;
    if (proxyType_) {
        int i = proxyType_->findData(static_cast<int>(effectiveProxyType));
        if (i >= 0)
            proxyType_->setCurrentIndex(i);
    }
    if (effectiveProxyType != openscp::ProxyType::None && !o.proxy_host.empty())
        proxyHost_->setText(QString::fromStdString(o.proxy_host));
    if (effectiveProxyType != openscp::ProxyType::None && o.proxy_port != 0)
        proxyPort_->setValue(static_cast<int>(o.proxy_port));
    if (effectiveProxyType != openscp::ProxyType::None && o.proxy_username &&
        !o.proxy_username->empty())
        proxyUser_->setText(QString::fromStdString(*o.proxy_username));
    if (effectiveProxyType != openscp::ProxyType::None && o.proxy_password &&
        !o.proxy_password->empty())
        proxyPass_->setText(QString::fromStdString(*o.proxy_password));
    if (jumpEnabled_)
        jumpEnabled_->setChecked(hasJump);
    if (hasJump)
        jumpHost_->setText(QString::fromStdString(*o.jump_host));
    if (o.jump_port != 0)
        jumpPort_->setValue(static_cast<int>(o.jump_port));
    if (o.jump_username && !o.jump_username->empty())
        jumpUser_->setText(QString::fromStdString(*o.jump_username));
    if (o.jump_private_key_path && !o.jump_private_key_path->empty())
        jumpKeyPath_->setText(QString::fromStdString(*o.jump_private_key_path));
}

void ConnectionDialog::updateProtocolUi(openscp::Protocol protocol) {
    if (!host_ || !port_)
        return;
    if (formLayout_ && scpMode_) {
        const bool isScpProtocol = (protocol == openscp::Protocol::Scp);
        setFormRowVisible(formLayout_, scpMode_, isScpProtocol);
        scpMode_->setEnabled(isScpProtocol);
    }
    if (formLayout_) {
        const bool isFtpsProtocol = (protocol == openscp::Protocol::Ftps);
        if (ftpsVerifyPeer_) {
            setFormRowVisible(formLayout_, ftpsVerifyPeer_, isFtpsProtocol);
            ftpsVerifyPeer_->setEnabled(isFtpsProtocol);
        }
        if (ftpsCaPathRow_) {
            setFormRowVisible(formLayout_, ftpsCaPathRow_, isFtpsProtocol);
            if (ftpsCaPath_)
                ftpsCaPath_->setEnabled(isFtpsProtocol);
            if (ftpsCaBrowse_)
                ftpsCaBrowse_->setEnabled(isFtpsProtocol);
        }
    }
    const openscp::ProtocolCapabilities caps =
        openscp::capabilitiesForProtocol(protocol);
    switch (protocol) {
    case openscp::Protocol::Sftp:
        host_->setPlaceholderText(tr("sftp.example.com"));
        break;
    case openscp::Protocol::Scp:
        host_->setPlaceholderText(tr("scp.example.com"));
        break;
    case openscp::Protocol::Ftp:
        host_->setPlaceholderText(tr("ftp.example.com"));
        break;
    case openscp::Protocol::Ftps:
        host_->setPlaceholderText(tr("ftps.example.com"));
        break;
    case openscp::Protocol::WebDav:
        host_->setPlaceholderText(tr("webdav.example.com"));
        break;
    }
    port_->setValue(static_cast<int>(openscp::defaultPortForProtocol(protocol)));

    const bool sshAuthSupported =
        (protocol == openscp::Protocol::Sftp ||
         protocol == openscp::Protocol::Scp);
    if (keyPath_)
        keyPath_->setEnabled(sshAuthSupported);
    if (keyPass_)
        keyPass_->setEnabled(sshAuthSupported);

    if (khPath_)
        khPath_->setEnabled(caps.supports_known_hosts);
    if (khBrowse_)
        khBrowse_->setEnabled(caps.supports_known_hosts);
    if (khPolicy_)
        khPolicy_->setEnabled(caps.supports_known_hosts);
    if (integrityPolicy_)
        integrityPolicy_->setEnabled(caps.supports_transfer_integrity);

    if (proxyType_) {
        if (!caps.supports_proxy) {
            const int directIdx =
                proxyType_->findData(static_cast<int>(openscp::ProxyType::None));
            if (directIdx >= 0)
                proxyType_->setCurrentIndex(directIdx);
            proxyType_->setEnabled(false);
        } else {
            proxyType_->setEnabled(true);
        }
    }

    bool jumpSupported = caps.supports_jump_host;
#ifdef Q_OS_WIN
    jumpSupported = false;
#endif
    if (jumpEnabled_) {
        if (!jumpSupported && jumpEnabled_->isChecked())
            jumpEnabled_->setChecked(false);
        jumpEnabled_->setEnabled(jumpSupported);
        if (!jumpSupported) {
            jumpEnabled_->setToolTip(
                tr("Not available for the selected protocol."));
        } else {
            jumpEnabled_->setToolTip(QString());
        }
    }
}
