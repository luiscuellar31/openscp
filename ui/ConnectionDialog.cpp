// Builds the connection form and exposes getters/setters for SessionOptions.
#include "ConnectionDialog.hpp"

#include "AppSettings.hpp"
#include "KeyboardFocusIndicator.hpp"

#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStringList>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QtGlobal>

#include <algorithm>
#include <initializer_list>
#include <string_view>

class ConnectionDisclosureHeader final : public QWidget {
    public:
    ConnectionDisclosureHeader(const QString &title, const QString &objectName,
                               QWidget *parent)
        : QWidget(parent) {
        setObjectName(objectName);
        setBackgroundRole(QPalette::AlternateBase);
        setAutoFillBackground(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        auto *headerLayout = new QHBoxLayout(this);
        headerLayout->setContentsMargins(6, 2, 8, 2);
        headerLayout->setSpacing(8);

        toggle_ = new QToolButton(this);
        toggle_->setObjectName(objectName + QStringLiteral("Toggle"));
        toggle_->setText(title);
        toggle_->setAccessibleName(title);
        toggle_->setCheckable(true);
        toggle_->setFocusPolicy(Qt::TabFocus);
        toggle_->setAutoRaise(true);
        toggle_->setArrowType(Qt::RightArrow);
        toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        toggle_->setStyleSheet(QStringLiteral(
            "QToolButton { border: none; background: transparent; padding: "
            "2px; }"));
        new openscpui::KeyboardFocusIndicator(toggle_);
        headerLayout->addWidget(toggle_);
        headerLayout->addStretch(1);

        summary_ = new QLabel(this);
        summary_->setObjectName(objectName + QStringLiteral("Summary"));
        summary_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        summary_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        headerLayout->addWidget(summary_);

        connect(toggle_, &QToolButton::toggled, this, [this](bool expanded) {
            toggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
            summary_->setVisible(!expanded && !summary_->toolTip().isEmpty());
        });
    }

    [[nodiscard]] bool isExpanded() const { return toggle_->isChecked(); }

    void setExpanded(bool expanded) { toggle_->setChecked(expanded); }

    void setSummary(const QString &summary) {
        summary_->setText(summary);
        summary_->setToolTip(summary);
        toggle_->setAccessibleDescription(summary);
        summary_->setVisible(!isExpanded() && !summary.isEmpty());
    }

    QToolButton *toggleButton() const { return toggle_; }

    private:
    QToolButton *toggle_ = nullptr;
    QLabel *summary_ = nullptr;
};

namespace {

openscp::SecureString secureUtf8(const QString &value) {
    QByteArray bytes = value.toUtf8();
    openscp::SecureString secure(std::string_view(
        bytes.constData(), static_cast<std::size_t>(bytes.size())));
    bytes.fill('\0');
    return secure;
}

void setFormRowVisible(QFormLayout *layout, QWidget *field, bool visible) {
    if (!layout || !field)
        return;
    if (QWidget *label = layout->labelForField(field))
        label->setVisible(visible);
    field->setVisible(visible);
}

void setFormRowsVisible(QFormLayout *layout, bool visible,
                        std::initializer_list<QWidget *> fields) {
    for (QWidget *field : fields)
        setFormRowVisible(layout, field, visible);
}

void stabilizeFormLabelColumn(QFormLayout *layout) {
    if (!layout)
        return;

    QList<QWidget *> labels;
    int widestLabel = 0;
    for (int row = 0; row < layout->rowCount(); ++row) {
        QLayoutItem *item = layout->itemAt(row, QFormLayout::LabelRole);
        QWidget *label = item ? item->widget() : nullptr;
        if (!label)
            continue;
        labels.push_back(label);
        widestLabel = std::max(widestLabel, label->sizeHint().width());
    }
    for (QWidget *label : labels)
        label->setMinimumWidth(widestLabel);
}

} // namespace

ConnectionDialog::ConnectionDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Connect"));
    auto *outerLayout = new QVBoxLayout(this);
    formContainer_ = new QWidget();
    auto *lay = new QFormLayout(formContainer_);
    formLayout_ = lay;
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setVerticalSpacing(8);
    lay->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    scrollArea_ = new QScrollArea(this);
    scrollArea_->setObjectName(QStringLiteral("connectionOptionsScrollArea"));
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setFrameShape(QFrame::NoFrame);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea_->setWidget(formContainer_);
    outerLayout->addWidget(scrollArea_, 1);

    protocol_ = new QComboBox(this);
    protocol_->addItem(tr("SFTP"), static_cast<int>(openscp::Protocol::Sftp));
    protocol_->addItem(tr("SCP"), static_cast<int>(openscp::Protocol::Scp));
    protocol_->addItem(tr("FTP"), static_cast<int>(openscp::Protocol::Ftp));
    protocol_->addItem(tr("FTPS"), static_cast<int>(openscp::Protocol::Ftps));
    protocol_->addItem(tr("WebDAV"),
                       static_cast<int>(openscp::Protocol::WebDav));
    scpMode_ = new QComboBox(this);
    scpMode_->addItem(tr("Automatic (safe SFTP uploads)"),
                      static_cast<int>(openscp::ScpTransferMode::Auto));
    scpMode_->addItem(tr("SCP only (uploads are non-atomic)"),
                      static_cast<int>(openscp::ScpTransferMode::ScpOnly));
    scpMode_->setItemData(
        0,
        tr("Uploads use a temporary remote file and atomic rename through "
           "SFTP."),
        Qt::ToolTipRole);
    scpMode_->setItemData(
        1,
        tr("Classic SCP writes directly to the final remote path. A canceled "
           "or failed upload may leave a partial destination."),
        Qt::ToolTipRole);
    {
        openscpui::AppSettings settings;
        const auto defaultProtocol = openscp::protocolFromStorageName(
            settings
                .value(openscpui::settingskeys::kDefaultProtocol,
                       QString::fromLatin1(openscp::protocolStorageName(
                           openscp::Protocol::Sftp)))
                .toString()
                .trimmed()
                .toLower()
                .toStdString());
        const int pidx = protocol_->findData(static_cast<int>(defaultProtocol));
        if (pidx >= 0)
            protocol_->setCurrentIndex(pidx);

        const auto defaultMode = openscp::scpTransferModeFromStorageName(
            settings
                .value(openscpui::settingskeys::kDefaultScpTransferMode,
                       QString::fromLatin1(openscp::scpTransferModeStorageName(
                           openscp::ScpTransferMode::Auto)))
                .toString()
                .trimmed()
                .toLower()
                .toStdString());
        const int modeIndex = scpMode_->findData(static_cast<int>(defaultMode));
        if (modeIndex >= 0)
            scpMode_->setCurrentIndex(modeIndex);
    }

    siteName_ = new QLineEdit(this);
    initialLocalPath_ = new QLineEdit(this);
    initialLocalPathBrowse_ = new QToolButton(this);
    initialRemotePath_ = new QLineEdit(this);
    rememberLastPaths_ =
        new QCheckBox(tr("Remember the last local and remote paths"), this);
    initialRemotePath_->setObjectName(
        QStringLiteral("connectionInitialRemotePath"));
    rememberLastPaths_->setObjectName(
        QStringLiteral("connectionRememberLastPaths"));
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
    initialLocalPath_->setPlaceholderText(
        tr("Home/current directory when empty"));
    initialLocalPathBrowse_->setText(tr("Choose…"));
    initialRemotePath_->setPlaceholderText(QStringLiteral("/"));
    initialRemotePath_->setText(QStringLiteral("/"));
    port_->setRange(1, 65535);
    port_->setFixedWidth(110);
    port_->setToolTip(tr("Server port for the selected protocol"));
    user_->setPlaceholderText(tr("user"));
    pass_->setPlaceholderText(tr("optional"));
    keyPath_->setPlaceholderText(tr("~/.ssh/id_ed25519"));
    keyPass_->setPlaceholderText(tr("optional"));
    proxyHost_->setPlaceholderText(tr("proxy.example.com"));
    proxyPort_->setRange(1, 65535);
    proxyPort_->setValue(static_cast<int>(
        openscp::defaultPortForProxyType(openscp::ProxyType::Socks5)));
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

    // Reserve enough room for useful values without letting a disclosed row
    // dictate a different dialog width.
    const int kInputMinWidth = 320;
    siteName_->setMinimumWidth(kInputMinWidth);
    initialLocalPath_->setMinimumWidth(kInputMinWidth);
    initialRemotePath_->setMinimumWidth(kInputMinWidth);
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

    keyPassRow_ = new QWidget(this);
    auto *keyPassRowLayout = new QHBoxLayout(keyPassRow_);
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

    keyPathRow_ = new QWidget(this);
    auto *keyPathRowLayout = new QHBoxLayout(keyPathRow_);
    keyPathRowLayout->setContentsMargins(0, 0, 0, 0);
    keyPathRowLayout->setSpacing(6);
    keyPathRowLayout->addWidget(keyPath_);
    keyPathRowLayout->addWidget(keyBrowseBtn);

    proxyType_ = new QComboBox(this);
    proxyType_->setObjectName(QStringLiteral("connectionProxyType"));
    proxyType_->addItem(tr("Direct (no proxy)"),
                        static_cast<int>(openscp::ProxyType::None));
    proxyType_->addItem(tr("SOCKS5"),
                        static_cast<int>(openscp::ProxyType::Socks5));
    proxyType_->addItem(tr("HTTP CONNECT"),
                        static_cast<int>(openscp::ProxyType::HttpConnect));

    proxyHostPortRow_ = new QWidget(this);
    auto *proxyHostPortLayout = new QHBoxLayout(proxyHostPortRow_);
    proxyHostPortLayout->setContentsMargins(0, 0, 0, 0);
    proxyHostPortLayout->setSpacing(6);
    proxyHostPortLayout->addWidget(proxyHost_, 1);
    proxyHostPortLayout->addWidget(proxyPort_);

    proxyPassRow_ = new QWidget(this);
    auto *proxyPassRowLayout = new QHBoxLayout(proxyPassRow_);
    proxyPassRowLayout->setContentsMargins(0, 0, 0, 0);
    proxyPassRowLayout->setSpacing(6);
    proxyPassRowLayout->addWidget(proxyPass_);
    proxyPassRowLayout->addWidget(proxyPassToggle);

    jumpHostPortRow_ = new QWidget(this);
    auto *jumpHostPortLayout = new QHBoxLayout(jumpHostPortRow_);
    jumpHostPortLayout->setContentsMargins(0, 0, 0, 0);
    jumpHostPortLayout->setSpacing(6);
    jumpHostPortLayout->addWidget(jumpHost_, 1);
    jumpHostPortLayout->addWidget(jumpPort_);

    jumpKeyPathRow_ = new QWidget(this);
    auto *jumpKeyPathLayout = new QHBoxLayout(jumpKeyPathRow_);
    jumpKeyPathLayout->setContentsMargins(0, 0, 0, 0);
    jumpKeyPathLayout->setSpacing(6);
    jumpKeyPathLayout->addWidget(jumpKeyPath_);
    jumpKeyPathLayout->addWidget(jumpKeyBrowse_);

    initialLocalPathRow_ = new QWidget(this);
    initialLocalPathRow_->setObjectName(
        QStringLiteral("connectionInitialLocalPathRow"));
    auto *initialLocalPathLayout = new QHBoxLayout(initialLocalPathRow_);
    initialLocalPathLayout->setContentsMargins(0, 0, 0, 0);
    initialLocalPathLayout->setSpacing(6);
    initialLocalPathLayout->addWidget(initialLocalPath_);
    initialLocalPathLayout->addWidget(initialLocalPathBrowse_);

    saveSite_ = new QCheckBox(tr("Save to saved sites"), this);
    saveSite_->setChecked(true);
    saveCredentials_ = new QCheckBox(tr("Save passwords/passphrases"), this);
    saveCredentials_->setChecked(false);
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
    khPathRow_ = new QWidget(this);
    auto *khPathRowLayout = new QHBoxLayout(khPathRow_);
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
    integrityPolicy_->setToolTip(
        tr("Checksum verification for resume and final transfer validation."));
    ftpsMode_ = new QComboBox(this);
    ftpsMode_->addItem(tr("Automatic (based on port)"),
                       static_cast<int>(openscp::FtpsMode::Auto));
    ftpsMode_->addItem(tr("Explicit TLS (AUTH TLS)"),
                       static_cast<int>(openscp::FtpsMode::ExplicitTls));
    ftpsMode_->addItem(tr("Implicit TLS"),
                       static_cast<int>(openscp::FtpsMode::ImplicitTls));
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

    webDavScheme_ = new QComboBox(this);
    webDavScheme_->addItem(tr("HTTPS (recommended)"),
                           static_cast<int>(openscp::WebDavScheme::Https));
    webDavScheme_->addItem(tr("HTTP (insecure)"),
                           static_cast<int>(openscp::WebDavScheme::Http));
    webDavBasePath_ = new QLineEdit(this);
    webDavBasePath_->setPlaceholderText(QStringLiteral("/"));
    webDavBasePath_->setText(QStringLiteral("/"));
    webDavBasePath_->setMinimumWidth(kInputMinWidth);
    webDavBasePath_->setToolTip(
        tr("Remote paths are confined below this WebDAV collection."));
    webDavVerifyPeer_ = new QCheckBox(
        tr("Verify WebDAV server certificate (recommended)"), this);
    webDavCaPath_ = new QLineEdit(this);
    webDavCaPath_->setPlaceholderText(tr("System CA bundle"));
    webDavCaPath_->setMinimumWidth(kInputMinWidth);
    webDavCaBrowse_ = new QToolButton(this);
    webDavCaBrowse_->setText(tr("Choose…"));
    webDavCaPathRow_ = new QWidget(this);
    auto *webDavCaRowLayout = new QHBoxLayout(webDavCaPathRow_);
    webDavCaRowLayout->setContentsMargins(0, 0, 0, 0);
    webDavCaRowLayout->setSpacing(6);
    webDavCaRowLayout->addWidget(webDavCaPath_);
    webDavCaRowLayout->addWidget(webDavCaBrowse_);
    {
        openscpui::AppSettings settings;
        int khPolicyIdx = khPolicy_->findData(
            settings
                .value(openscpui::settingskeys::kDefaultKnownHostsPolicy,
                       static_cast<int>(openscp::KnownHostsPolicy::Strict))
                .toInt());
        if (khPolicyIdx < 0) {
            khPolicyIdx = khPolicy_->findData(
                static_cast<int>(openscp::KnownHostsPolicy::Strict));
        }
        if (khPolicyIdx >= 0)
            khPolicy_->setCurrentIndex(khPolicyIdx);

        int integrityIdx = integrityPolicy_->findData(
            settings
                .value(openscpui::settingskeys::kDefaultTransferIntegrityPolicy,
                       static_cast<int>(
                           openscp::TransferIntegrityPolicy::Optional))
                .toInt());
        if (integrityIdx < 0) {
            integrityIdx = integrityPolicy_->findData(
                static_cast<int>(openscp::TransferIntegrityPolicy::Optional));
        }
        if (integrityIdx >= 0)
            integrityPolicy_->setCurrentIndex(integrityIdx);

        if (ftpsVerifyPeer_) {
            ftpsVerifyPeer_->setChecked(
                settings
                    .value(openscpui::settingskeys::kFtpsVerifyPeerDefault,
                           true)
                    .toBool());
        }
        if (ftpsCaPath_) {
            ftpsCaPath_->setText(
                settings
                    .value(openscpui::settingskeys::kFtpsCaCertPathDefault,
                           QString())
                    .toString()
                    .trimmed());
        }
        if (webDavVerifyPeer_) {
            webDavVerifyPeer_->setChecked(
                settings
                    .value(openscpui::settingskeys::kWebDavVerifyPeerDefault,
                           true)
                    .toBool());
        }
        if (webDavCaPath_) {
            webDavCaPath_->setText(
                settings
                    .value(openscpui::settingskeys::kWebDavCaCertPathDefault,
                           QString())
                    .toString()
                    .trimmed());
        }
    }

    pathsSection_ = new ConnectionDisclosureHeader(
        tr("Startup folders"), QStringLiteral("connectionPathsSection"), this);
    sshKeySection_ = new ConnectionDisclosureHeader(
        tr("SSH key"), QStringLiteral("connectionSshKeySection"), this);
    networkSection_ = new ConnectionDisclosureHeader(
        tr("Proxy and jump host"), QStringLiteral("connectionNetworkSection"),
        this);
    securitySection_ = new ConnectionDisclosureHeader(
        tr("Security"), QStringLiteral("connectionSecuritySection"), this);

    siteName_->setObjectName(QStringLiteral("connectionSiteName"));
    protocol_->setObjectName(QStringLiteral("connectionProtocol"));
    host_->setObjectName(QStringLiteral("connectionHost"));
    port_->setObjectName(QStringLiteral("connectionPort"));
    user_->setObjectName(QStringLiteral("connectionUser"));
    pass_->setObjectName(QStringLiteral("connectionPassword"));
    keyPathRow_->setObjectName(QStringLiteral("connectionSshKeyPathRow"));
    keyPassRow_->setObjectName(QStringLiteral("connectionSshKeyPassphraseRow"));
    proxyHostPortRow_->setObjectName(
        QStringLiteral("connectionProxyHostPortRow"));
    jumpEnabled_->setObjectName(QStringLiteral("connectionJumpEnabled"));
    khPathRow_->setObjectName(QStringLiteral("connectionKnownHostsPathRow"));

    lay->addRow(tr("Site name:"), siteName_);
    siteNameLabel_ = lay->labelForField(siteName_);
    lay->addRow(QString(), saveSite_);
    lay->addRow(QString(), saveCredentials_);
    lay->addRow(tr("Protocol:"), protocol_);
    lay->addRow(tr("SCP mode:"), scpMode_);
    lay->addRow(tr("FTPS mode:"), ftpsMode_);
    lay->addRow(tr("WebDAV scheme:"), webDavScheme_);
    lay->addRow(tr("WebDAV base path:"), webDavBasePath_);
    lay->addRow(tr("Host / Port:"), hostPortRow);
    lay->addRow(tr("User:"), user_);
    lay->addRow(tr("Password:"), passRow);

    lay->addRow(pathsSection_);
    lay->addRow(tr("Initial local path:"), initialLocalPathRow_);
    lay->addRow(tr("Initial remote path:"), initialRemotePath_);
    lay->addRow(QString(), rememberLastPaths_);

    lay->addRow(sshKeySection_);
    lay->addRow(tr("Private key path:"), keyPathRow_);
    lay->addRow(tr("Key passphrase:"), keyPassRow_);

    lay->addRow(networkSection_);
    lay->addRow(tr("Proxy:"), proxyType_);
    lay->addRow(tr("Proxy host / port:"), proxyHostPortRow_);
    lay->addRow(tr("Proxy user:"), proxyUser_);
    lay->addRow(tr("Proxy password:"), proxyPassRow_);
    lay->addRow(QString(), jumpEnabled_);
    lay->addRow(tr("Jump host / port:"), jumpHostPortRow_);
    lay->addRow(tr("Jump user:"), jumpUser_);
    lay->addRow(tr("Jump private key:"), jumpKeyPathRow_);

    lay->addRow(securitySection_);
    lay->addRow(tr("Known hosts file:"), khPathRow_);
    lay->addRow(tr("Policy:"), khPolicy_);
    lay->addRow(tr("Integrity:"), integrityPolicy_);
    lay->addRow(QString(), ftpsVerifyPeer_);
    lay->addRow(tr("FTPS CA bundle:"), ftpsCaPathRow_);
    lay->addRow(QString(), webDavVerifyPeer_);
    lay->addRow(tr("WebDAV CA bundle:"), webDavCaPathRow_);

    // Hidden rows otherwise change QFormLayout's shared label column when
    // opened. Reserving the widest translated label keeps every state aligned.
    stabilizeFormLabelColumn(lay);

    setSiteNameVisible(false);
    setQuickConnectSaveOptionsVisible(false);

    auto updateProxyFields = [this, lay]() {
        const auto type = openscp::normalizeProxyType(
            static_cast<openscp::ProxyType>(proxyType_->currentData().toInt()));
        const auto protocol = protocol_ ? static_cast<openscp::Protocol>(
                                              protocol_->currentData().toInt())
                                        : openscp::Protocol::Sftp;
        const auto caps = openscp::capabilitiesForProtocol(protocol);
        const bool proxyConfigured =
            caps.supports_proxy && (type != openscp::ProxyType::None);
        const bool showProxyRows =
            proxyConfigured && networkSection_ && networkSection_->isExpanded();
        const std::uint16_t defaultPortForType =
            openscp::defaultPortForProxyType(type);
        const std::uint16_t previousDefaultPort =
            openscp::defaultPortForProxyType(lastProxyType_);
        setFormRowsVisible(lay, showProxyRows,
                           {proxyHostPortRow_, proxyUser_, proxyPassRow_});
        if (!proxyConfigured) {
            proxyPort_->setValue(static_cast<int>(
                openscp::defaultPortForProxyType(openscp::ProxyType::Socks5)));
        } else {
            const int currentPort = proxyPort_->value();
            const bool isFirstProxySelection =
                (lastProxyType_ == openscp::ProxyType::None);
            const bool usesPreviousDefault =
                (previousDefaultPort != 0) &&
                (currentPort == static_cast<int>(previousDefaultPort));
            if (defaultPortForType != 0 &&
                (isFirstProxySelection || usesPreviousDefault) &&
                currentPort != static_cast<int>(defaultPortForType)) {
                proxyPort_->setValue(static_cast<int>(defaultPortForType));
            }
        }
        lastProxyType_ = type;
    };

    auto updateJumpFields = [this, lay]() {
        const auto protocol = protocol_ ? static_cast<openscp::Protocol>(
                                              protocol_->currentData().toInt())
                                        : openscp::Protocol::Sftp;
        bool jumpSupported =
            openscp::capabilitiesForProtocol(protocol).supports_jump_host;
#ifdef Q_OS_WIN
        jumpSupported = false;
#endif
        if (!jumpSupported && jumpEnabled_ && jumpEnabled_->isChecked()) {
            const QSignalBlocker blocker(jumpEnabled_);
            jumpEnabled_->setChecked(false);
        }

        const bool showJumpRows =
            jumpSupported && jumpEnabled_ && jumpEnabled_->isChecked() &&
            networkSection_ && networkSection_->isExpanded();
        setFormRowsVisible(lay, showJumpRows,
                           {jumpHostPortRow_, jumpUser_, jumpKeyPathRow_});
    };
    auto refreshNetworkFields = [this, updateProxyFields, updateJumpFields]() {
        updateProxyFields();
        updateJumpFields();
        updateSectionSummaries();
        adjustToContent();
    };
    connect(proxyType_, &QComboBox::currentIndexChanged, this,
            [this, refreshNetworkFields](int) {
                const auto type =
                    openscp::normalizeProxyType(static_cast<openscp::ProxyType>(
                        proxyType_->currentData().toInt()));
                if (type != openscp::ProxyType::None && jumpEnabled_ &&
                    jumpEnabled_->isChecked()) {
                    const QSignalBlocker blocker(jumpEnabled_);
                    jumpEnabled_->setChecked(false);
                }
                refreshNetworkFields();
            });
    connect(jumpEnabled_, &QCheckBox::toggled, this,
            [this, refreshNetworkFields](bool checked) {
                if (checked && proxyType_) {
                    const int directIdx = proxyType_->findData(
                        static_cast<int>(openscp::ProxyType::None));
                    if (directIdx >= 0 &&
                        proxyType_->currentIndex() != directIdx) {
                        const QSignalBlocker blocker(proxyType_);
                        proxyType_->setCurrentIndex(directIdx);
                    }
                }
                refreshNetworkFields();
            });
    connect(ftpsMode_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!protocol_ || !port_ || !ftpsMode_)
            return;
        const auto protocol =
            static_cast<openscp::Protocol>(protocol_->currentData().toInt());
        const auto selectedMode = openscp::normalizeFtpsMode(
            static_cast<openscp::FtpsMode>(ftpsMode_->currentData().toInt()));
        if (protocol == openscp::Protocol::Ftps) {
            if (selectedMode == openscp::FtpsMode::ExplicitTls &&
                (lastFtpsMode_ == openscp::FtpsMode::ImplicitTls ||
                 port_->value() == 990)) {
                port_->setValue(21);
            } else if (selectedMode == openscp::FtpsMode::ImplicitTls &&
                       (lastFtpsMode_ == openscp::FtpsMode::ExplicitTls ||
                        port_->value() == 21)) {
                port_->setValue(990);
            }
        }
        lastFtpsMode_ = selectedMode;
    });
    connect(webDavScheme_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!protocol_ || !port_ || !webDavScheme_)
            return;
        const auto protocol =
            static_cast<openscp::Protocol>(protocol_->currentData().toInt());
        const auto selectedScheme =
            openscp::normalizeWebDavScheme(static_cast<openscp::WebDavScheme>(
                webDavScheme_->currentData().toInt()));
        const int previousDefaultPort = static_cast<int>(
            openscp::defaultPortForWebDavScheme(lastWebDavScheme_));
        const int selectedDefaultPort = static_cast<int>(
            openscp::defaultPortForWebDavScheme(selectedScheme));
        if (protocol == openscp::Protocol::WebDav &&
            port_->value() == previousDefaultPort &&
            previousDefaultPort != selectedDefaultPort) {
            port_->setValue(selectedDefaultPort);
        }
        lastWebDavScheme_ = selectedScheme;
        updateProtocolUi(protocol, false);
        adjustToContent();
    });

    connect(pathsSection_->toggleButton(), &QToolButton::toggled, this,
            [this](bool) {
                updatePathSectionVisibility();
                adjustToContent();
            });
    auto refreshProtocolDetails = [this](bool) {
        if (protocol_) {
            updateProtocolUi(static_cast<openscp::Protocol>(
                                 protocol_->currentData().toInt()),
                             false);
        }
        adjustToContent();
    };
    for (ConnectionDisclosureHeader *section :
         {sshKeySection_, securitySection_}) {
        connect(section->toggleButton(), &QToolButton::toggled, this,
                refreshProtocolDetails);
    }
    connect(networkSection_->toggleButton(), &QToolButton::toggled, this,
            [this, refreshNetworkFields](bool) {
                if (protocol_) {
                    updateProtocolUi(static_cast<openscp::Protocol>(
                                         protocol_->currentData().toInt()),
                                     false);
                }
                refreshNetworkFields();
            });

    connect(initialLocalPath_, &QLineEdit::textChanged, this,
            &ConnectionDialog::updateSectionSummaries);
    connect(initialRemotePath_, &QLineEdit::textChanged, this,
            &ConnectionDialog::updateSectionSummaries);
    connect(rememberLastPaths_, &QCheckBox::toggled, this,
            &ConnectionDialog::updateSectionSummaries);
    connect(keyPath_, &QLineEdit::textChanged, this,
            &ConnectionDialog::updateSectionSummaries);
    connect(khPolicy_, &QComboBox::currentIndexChanged, this,
            &ConnectionDialog::updateSectionSummaries);
    connect(integrityPolicy_, &QComboBox::currentIndexChanged, this,
            &ConnectionDialog::updateSectionSummaries);
    connect(ftpsVerifyPeer_, &QCheckBox::toggled, this,
            &ConnectionDialog::updateSectionSummaries);
    connect(webDavVerifyPeer_, &QCheckBox::toggled, this,
            &ConnectionDialog::updateSectionSummaries);

    connect(protocol_, &QComboBox::currentIndexChanged, this,
            [this, updateProxyFields, updateJumpFields](int) {
                if (!protocol_)
                    return;
                QPointer<QWidget> previouslyFocused = focusWidget();
                const auto protocol = static_cast<openscp::Protocol>(
                    protocol_->currentData().toInt());
                updateProtocolUi(protocol);
                updateProxyFields();
                updateJumpFields();
                updateSectionSummaries();
                adjustToContent();
                if (host_ && previouslyFocused &&
                    previouslyFocused != protocol_ &&
                    previouslyFocused != host_ &&
                    !previouslyFocused->isVisibleTo(this)) {
                    host_->setFocus(Qt::OtherFocusReason);
                }
            });
    const auto initialProtocol =
        protocol_
            ? static_cast<openscp::Protocol>(protocol_->currentData().toInt())
            : openscp::Protocol::Sftp;
    updateProtocolUi(initialProtocol);
    updateProxyFields();
    updateJumpFields();
    updateSectionSummaries();

    connect(khBrowse_, &QToolButton::clicked, this, [this] {
        const QString selectedPath = QFileDialog::getOpenFileName(
            this, tr("Select known_hosts"), QDir::homePath() + "/.ssh");
        if (!selectedPath.isEmpty())
            khPath_->setText(selectedPath);
    });

    connect(ftpsCaBrowse_, &QToolButton::clicked, this, [this] {
        const QString selectedPath = QFileDialog::getOpenFileName(
            this, tr("Select FTPS CA bundle"), QDir::homePath());
        if (!selectedPath.isEmpty())
            ftpsCaPath_->setText(selectedPath);
    });
    connect(webDavCaBrowse_, &QToolButton::clicked, this, [this] {
        const QString selectedPath = QFileDialog::getOpenFileName(
            this, tr("Select WebDAV CA bundle"), QDir::homePath());
        if (!selectedPath.isEmpty())
            webDavCaPath_->setText(selectedPath);
    });

    connect(keyBrowseBtn, &QToolButton::clicked, this, [this] {
        const QString selectedPath = QFileDialog::getOpenFileName(
            this, tr("Select private key"), QDir::homePath() + "/.ssh");
        if (!selectedPath.isEmpty())
            keyPath_->setText(selectedPath);
    });

    connect(jumpKeyBrowse_, &QToolButton::clicked, this, [this] {
        const QString selectedPath = QFileDialog::getOpenFileName(
            this, tr("Select jump private key"), QDir::homePath() + "/.ssh");
        if (!selectedPath.isEmpty())
            jumpKeyPath_->setText(selectedPath);
    });
    connect(initialLocalPathBrowse_, &QToolButton::clicked, this, [this] {
        const QString start = initialLocalPath_->text().trimmed().isEmpty()
                                  ? QDir::homePath()
                                  : initialLocalPath_->text().trimmed();
        const QString selectedPath = QFileDialog::getExistingDirectory(
            this, tr("Select initial local path"), start);
        if (!selectedPath.isEmpty())
            initialLocalPath_->setText(QDir::cleanPath(selectedPath));
    });

    dialogButtons_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    dialogButtons_->setObjectName(QStringLiteral("connectionDialogButtons"));
    acceptButton_ = dialogButtons_->button(QDialogButtonBox::Ok);
    if (acceptButton_)
        acceptButton_->setObjectName(QStringLiteral("connectionAcceptButton"));
    outerLayout->addWidget(dialogButtons_);
    connect(dialogButtons_, &QDialogButtonBox::accepted, this,
            &QDialog::accept);
    connect(dialogButtons_, &QDialogButtonBox::rejected, this,
            &QDialog::reject);

    // Initial focus to guide users to provide an explicit target host.
    QTimer::singleShot(0, host_, [this] {
        if (host_)
            host_->setFocus(Qt::OtherFocusReason);
    });
    adjustToContent();
}

void ConnectionDialog::updateSectionSummaries() {
    if (pathsSection_) {
        const bool remembersFolders =
            rememberLastPaths_ && rememberLastPaths_->isChecked();
        const bool hasCustomFolders =
            (initialLocalPath_ &&
             !initialLocalPath_->text().trimmed().isEmpty()) ||
            (initialRemotePath_ &&
             !initialRemotePath_->text().trimmed().isEmpty() &&
             initialRemotePath_->text().trimmed() != QLatin1String("/"));
        pathsSection_->setSummary(remembersFolders ? tr("Remember last folders")
                                  : hasCustomFolders ? tr("Custom folders")
                                                     : tr("Default folders"));
    }

    if (sshKeySection_) {
        const QString keyPath =
            keyPath_ ? keyPath_->text().trimmed() : QString();
        sshKeySection_->setSummary(keyPath.isEmpty() ? tr("Not configured")
                                                     : tr("Configured"));
    }

    if (networkSection_) {
        QString networkSummary = tr("Direct connection");
        if (jumpEnabled_ && jumpEnabled_->isChecked())
            networkSummary = tr("SSH jump host");
        else if (proxyType_) {
            const auto proxyType =
                openscp::normalizeProxyType(static_cast<openscp::ProxyType>(
                    proxyType_->currentData().toInt()));
            if (proxyType != openscp::ProxyType::None)
                networkSummary = proxyType_->currentText();
        }
        networkSection_->setSummary(networkSummary);
    }

    if (securitySection_ && protocol_) {
        const auto protocol =
            static_cast<openscp::Protocol>(protocol_->currentData().toInt());
        const auto caps = openscp::capabilitiesForProtocol(protocol);
        QStringList securitySummary;
        if (caps.supports_known_hosts && khPolicy_) {
            const auto knownHostsPolicy = openscp::normalizeKnownHostsPolicy(
                static_cast<openscp::KnownHostsPolicy>(
                    khPolicy_->currentData().toInt()));
            if (knownHostsPolicy == openscp::KnownHostsPolicy::AcceptNew)
                securitySummary.push_back(tr("Accept new hosts"));
            else if (knownHostsPolicy == openscp::KnownHostsPolicy::Off)
                securitySummary.push_back(tr("No host verification"));
        }
        if (protocol == openscp::Protocol::Ftps && ftpsVerifyPeer_ &&
            !ftpsVerifyPeer_->isChecked())
            securitySummary.push_back(tr("Certificate verification off"));
        if (protocol == openscp::Protocol::WebDav && webDavVerifyPeer_ &&
            webDavScheme_) {
            const auto scheme = openscp::normalizeWebDavScheme(
                static_cast<openscp::WebDavScheme>(
                    webDavScheme_->currentData().toInt()));
            if (scheme == openscp::WebDavScheme::Https &&
                !webDavVerifyPeer_->isChecked())
                securitySummary.push_back(tr("Certificate verification off"));
        }
        if (caps.can_checksum && integrityPolicy_) {
            const auto integrityPolicy =
                openscp::normalizeTransferIntegrityPolicy(
                    static_cast<openscp::TransferIntegrityPolicy>(
                        integrityPolicy_->currentData().toInt()));
            if (integrityPolicy == openscp::TransferIntegrityPolicy::Required) {
                securitySummary.push_back(tr("Integrity required"));
            } else if (integrityPolicy ==
                       openscp::TransferIntegrityPolicy::Off) {
                securitySummary.push_back(tr("Integrity off"));
            }
        }
        if (securitySummary.isEmpty())
            securitySummary.push_back(tr("Default security"));
        securitySection_->setSummary(
            securitySummary.join(QStringLiteral(" · ")));
    }
}

void ConnectionDialog::updatePathSectionVisibility() {
    const bool showRows =
        siteOptionsVisible_ && pathsSection_ && pathsSection_->isExpanded();
    if (pathsSection_)
        pathsSection_->setVisible(siteOptionsVisible_);
    setFormRowsVisible(
        formLayout_, showRows,
        {initialLocalPathRow_, initialRemotePath_, rememberLastPaths_});
    updateSectionSummaries();
}

void ConnectionDialog::adjustToContent() {
    if (!formLayout_ || !formContainer_ || !scrollArea_ || !dialogButtons_ ||
        !layout()) {
        return;
    }

    const bool restoreUpdates = updatesEnabled();
    if (restoreUpdates)
        setUpdatesEnabled(false);

    const int preservedWidth = width();
    formLayout_->invalidate();
    formLayout_->activate();
    const QSize formMinimumSize = formLayout_->minimumSize();
    const int contentHeight = formLayout_->sizeHint().height();
    formContainer_->setMinimumSize(formMinimumSize.width(), contentHeight);

    const QMargins margins = layout()->contentsMargins();
    const int verticalScrollBarWidth =
        style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, scrollArea_);
    const int minimumDialogWidth =
        formMinimumSize.width() + margins.left() + margins.right() +
        (2 * scrollArea_->frameWidth()) + verticalScrollBarWidth;
    setMinimumWidth(std::max(minimumWidth(), minimumDialogWidth));

    QScreen *targetScreen = screen();
    if (!targetScreen)
        targetScreen = QGuiApplication::primaryScreen();
    const int maximumFrameHeight =
        targetScreen ? targetScreen->availableGeometry().height() * 4 / 5 : 720;
    const int frameDecorationHeight =
        std::max(0, frameGeometry().height() - height());
    const int maximumDialogHeight =
        std::max(320, maximumFrameHeight - frameDecorationHeight);
    const int fixedChromeHeight = margins.top() + margins.bottom() +
                                  std::max(0, layout()->spacing()) +
                                  dialogButtons_->sizeHint().height();
    scrollArea_->setFixedHeight(std::min(
        contentHeight, std::max(200, maximumDialogHeight - fixedChromeHeight)));

    layout()->invalidate();
    layout()->activate();
    resize(std::max(preservedWidth, minimumWidth()),
           std::min(maximumDialogHeight, sizeHint().height()));

    if (restoreUpdates) {
        setUpdatesEnabled(true);
        update();
    }
}

void ConnectionDialog::setSiteNameVisible(bool visible) {
    siteOptionsVisible_ = visible;
    if (siteName_)
        siteName_->setVisible(visible);
    if (siteNameLabel_)
        siteNameLabel_->setVisible(visible);
    updatePathSectionVisibility();
    adjustToContent();
}

void ConnectionDialog::setSiteName(const QString &name) {
    if (siteName_)
        siteName_->setText(name);
}

QString ConnectionDialog::siteName() const {
    return siteName_ ? siteName_->text() : QString();
}

void ConnectionDialog::setInitialLocalPath(const QString &path) {
    if (initialLocalPath_) {
        initialLocalPath_->setText(path);
        if (!path.trimmed().isEmpty() && pathsSection_)
            pathsSection_->setExpanded(true);
    }
}

QString ConnectionDialog::initialLocalPath() const {
    return initialLocalPath_ ? initialLocalPath_->text().trimmed() : QString();
}

void ConnectionDialog::setInitialRemotePath(const QString &path) {
    if (!initialRemotePath_)
        return;
    QString normalized = path.trimmed();
    if (normalized.isEmpty())
        normalized = QStringLiteral("/");
    if (!normalized.startsWith('/'))
        normalized.prepend('/');
    initialRemotePath_->setText(normalized);
    if (normalized != QLatin1String("/") && pathsSection_)
        pathsSection_->setExpanded(true);
}

QString ConnectionDialog::initialRemotePath() const {
    QString path =
        initialRemotePath_ ? initialRemotePath_->text().trimmed() : QString();
    if (path.isEmpty())
        path = QStringLiteral("/");
    if (!path.startsWith('/'))
        path.prepend('/');
    return path;
}

void ConnectionDialog::setRememberLastPaths(bool remember) {
    if (rememberLastPaths_) {
        rememberLastPaths_->setChecked(remember);
        if (remember && pathsSection_)
            pathsSection_->setExpanded(true);
    }
}

bool ConnectionDialog::rememberLastPaths() const {
    return rememberLastPaths_ && rememberLastPaths_->isChecked();
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

void ConnectionDialog::setAcceptButtonText(const QString &text) {
    if (acceptButton_ && !text.trimmed().isEmpty())
        acceptButton_->setText(text);
}

openscp::SessionOptions ConnectionDialog::options() const {
    openscp::SessionOptions sessionOptions;
    if (protocol_) {
        sessionOptions.protocol =
            static_cast<openscp::Protocol>(protocol_->currentData().toInt());
    }
    if (scpMode_) {
        sessionOptions.scp_transfer_mode =
            static_cast<openscp::ScpTransferMode>(
                scpMode_->currentData().toInt());
    }
    if (ftpsMode_) {
        sessionOptions.ftps_mode = openscp::normalizeFtpsMode(
            static_cast<openscp::FtpsMode>(ftpsMode_->currentData().toInt()));
    }
    if (sessionOptions.protocol != openscp::Protocol::Scp)
        sessionOptions.scp_transfer_mode = openscp::ScpTransferMode::Auto;
    if (webDavScheme_) {
        sessionOptions.webdav_scheme =
            openscp::normalizeWebDavScheme(static_cast<openscp::WebDavScheme>(
                webDavScheme_->currentData().toInt()));
    }
    if (webDavBasePath_) {
        sessionOptions.webdav_base_path = openscp::normalizeWebDavBasePath(
            webDavBasePath_->text().trimmed().toStdString());
    }
    sessionOptions.host = host_->text().toStdString();
    sessionOptions.port = static_cast<std::uint16_t>(port_->value());
    sessionOptions.username = user_->text().toStdString();

    // Password (if provided)
    if (!pass_->text().isEmpty())
        sessionOptions.password = secureUtf8(pass_->text());

    // Private key path (if provided)
    if (!keyPath_->text().isEmpty())
        sessionOptions.private_key_path = keyPath_->text().toStdString();

    // Key passphrase (if provided)
    if (!keyPass_->text().isEmpty())
        sessionOptions.private_key_passphrase = secureUtf8(keyPass_->text());

    // known_hosts
    if (!khPath_->text().isEmpty())
        sessionOptions.known_hosts_path = khPath_->text().toStdString();
    sessionOptions.known_hosts_policy =
        openscp::knownHostsPolicyFromStorageValue(
            khPolicy_->currentData().toInt());
    if (integrityPolicy_) {
        sessionOptions.transfer_integrity_policy =
            openscp::transferIntegrityPolicyFromStorageValue(
                integrityPolicy_->currentData().toInt());
    }
    if (ftpsVerifyPeer_) {
        sessionOptions.ftps_verify_peer = ftpsVerifyPeer_->isChecked();
    }
    if (ftpsCaPath_ && !ftpsCaPath_->text().trimmed().isEmpty()) {
        sessionOptions.ftps_ca_cert_path =
            ftpsCaPath_->text().trimmed().toStdString();
    }
    if (webDavVerifyPeer_) {
        sessionOptions.webdav_verify_peer = webDavVerifyPeer_->isChecked();
    }
    if (webDavCaPath_ && !webDavCaPath_->text().trimmed().isEmpty()) {
        sessionOptions.webdav_ca_cert_path =
            webDavCaPath_->text().trimmed().toStdString();
    }
    if (proxyType_) {
        sessionOptions.proxy_type = openscp::normalizeProxyType(
            static_cast<openscp::ProxyType>(proxyType_->currentData().toInt()));
    }
    const auto caps = openscp::capabilitiesForProtocol(sessionOptions.protocol);
    bool jumpSupported = caps.supports_jump_host;
#ifdef Q_OS_WIN
    jumpSupported = false;
#endif
    const bool useJump = jumpEnabled_ && jumpSupported &&
                         jumpEnabled_->isChecked() &&
                         !jumpHost_->text().trimmed().isEmpty();
    const bool useProxy =
        caps.supports_proxy &&
        (sessionOptions.proxy_type != openscp::ProxyType::None) && !useJump;
    if (useProxy) {
        sessionOptions.proxy_host = proxyHost_->text().trimmed().toStdString();
        sessionOptions.proxy_port =
            static_cast<std::uint16_t>(proxyPort_->value());
        if (!proxyUser_->text().isEmpty())
            sessionOptions.proxy_username = proxyUser_->text().toStdString();
        if (!proxyPass_->text().isEmpty())
            sessionOptions.proxy_password = secureUtf8(proxyPass_->text());
    }
    if (useJump) {
        sessionOptions.proxy_type = openscp::ProxyType::None;
        sessionOptions.jump_host = jumpHost_->text().trimmed().toStdString();
        sessionOptions.jump_port =
            static_cast<std::uint16_t>(jumpPort_->value());
        if (!jumpUser_->text().isEmpty())
            sessionOptions.jump_username = jumpUser_->text().toStdString();
        if (!jumpKeyPath_->text().isEmpty())
            sessionOptions.jump_private_key_path =
                jumpKeyPath_->text().toStdString();
    } else {
        sessionOptions.jump_host.reset();
        sessionOptions.jump_username.reset();
        sessionOptions.jump_private_key_path.reset();
    }
    if (!caps.supports_proxy) {
        sessionOptions.proxy_type = openscp::ProxyType::None;
        sessionOptions.proxy_host.clear();
        sessionOptions.proxy_port = 0;
        sessionOptions.proxy_username.reset();
        sessionOptions.proxy_password.reset();
    }
    if (sessionOptions.protocol != openscp::Protocol::Ftps) {
        sessionOptions.ftps_mode = openscp::FtpsMode::Auto;
        sessionOptions.ftps_verify_peer = true;
        sessionOptions.ftps_ca_cert_path.reset();
    }
    if (sessionOptions.protocol != openscp::Protocol::WebDav) {
        sessionOptions.webdav_scheme = openscp::WebDavScheme::Https;
        sessionOptions.webdav_base_path = "/";
        sessionOptions.webdav_verify_peer = true;
        sessionOptions.webdav_ca_cert_path.reset();
    } else if (sessionOptions.webdav_scheme == openscp::WebDavScheme::Http) {
        sessionOptions.webdav_verify_peer = false;
        sessionOptions.webdav_ca_cert_path.reset();
    }

    return sessionOptions;
}

void ConnectionDialog::setOptions(const openscp::SessionOptions &options) {
    openscp::Protocol effectiveProtocol = options.protocol;
    if (protocol_) {
        int pidx = protocol_->findData(static_cast<int>(options.protocol));
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
            scpMode_->findData(static_cast<int>(options.scp_transfer_mode));
        if (modeIdx >= 0)
            scpMode_->setCurrentIndex(modeIdx);
    }
    if (!options.host.empty())
        host_->setText(QString::fromStdString(options.host));
    if (options.port)
        port_->setValue(static_cast<int>(options.port));
    if (!options.username.empty())
        user_->setText(QString::fromStdString(options.username));
    if (options.password && !options.password->empty())
        pass_->setText(QString::fromUtf8(
            options.password->data(),
            static_cast<qsizetype>(options.password->size())));
    if (options.private_key_path && !options.private_key_path->empty())
        keyPath_->setText(QString::fromStdString(*options.private_key_path));
    if (options.private_key_passphrase &&
        !options.private_key_passphrase->empty())
        keyPass_->setText(QString::fromUtf8(
            options.private_key_passphrase->data(),
            static_cast<qsizetype>(options.private_key_passphrase->size())));
    if (options.known_hosts_path && !options.known_hosts_path->empty())
        khPath_->setText(QString::fromStdString(*options.known_hosts_path));
    // Policy
    int idx = khPolicy_->findData(static_cast<int>(options.known_hosts_policy));
    if (idx >= 0)
        khPolicy_->setCurrentIndex(idx);
    if (integrityPolicy_) {
        int integrityPolicyIndex = integrityPolicy_->findData(
            static_cast<int>(options.transfer_integrity_policy));
        if (integrityPolicyIndex >= 0)
            integrityPolicy_->setCurrentIndex(integrityPolicyIndex);
    }
    if (ftpsMode_) {
        const auto mode = openscp::normalizeFtpsMode(options.ftps_mode);
        const int ftpsModeIndex = ftpsMode_->findData(static_cast<int>(mode));
        if (ftpsModeIndex >= 0)
            ftpsMode_->setCurrentIndex(ftpsModeIndex);
        lastFtpsMode_ = mode;
    }
    if (ftpsVerifyPeer_)
        ftpsVerifyPeer_->setChecked(options.ftps_verify_peer);
    if (ftpsCaPath_) {
        if (options.ftps_ca_cert_path && !options.ftps_ca_cert_path->empty()) {
            ftpsCaPath_->setText(
                QString::fromStdString(*options.ftps_ca_cert_path));
        } else {
            ftpsCaPath_->clear();
        }
    }
    if (webDavScheme_) {
        const int webDavSchemeIndex = webDavScheme_->findData(static_cast<int>(
            openscp::normalizeWebDavScheme(options.webdav_scheme)));
        if (webDavSchemeIndex >= 0)
            webDavScheme_->setCurrentIndex(webDavSchemeIndex);
        lastWebDavScheme_ =
            openscp::normalizeWebDavScheme(options.webdav_scheme);
    }
    if (webDavBasePath_) {
        webDavBasePath_->setText(QString::fromStdString(
            openscp::normalizeWebDavBasePath(options.webdav_base_path)));
    }
    if (webDavVerifyPeer_)
        webDavVerifyPeer_->setChecked(options.webdav_verify_peer);
    if (webDavCaPath_) {
        if (options.webdav_ca_cert_path &&
            !options.webdav_ca_cert_path->empty()) {
            webDavCaPath_->setText(
                QString::fromStdString(*options.webdav_ca_cert_path));
        } else {
            webDavCaPath_->clear();
        }
    }

    const auto caps = openscp::capabilitiesForProtocol(effectiveProtocol);
    bool jumpSupportedInUi = caps.supports_jump_host;
#ifdef Q_OS_WIN
    jumpSupportedInUi = false;
#endif
    const bool hasJump = jumpSupportedInUi && options.jump_host.has_value() &&
                         !options.jump_host->empty();
    const bool proxySupportedInUi = caps.supports_proxy;
    const auto requestedProxyType =
        openscp::normalizeProxyType(options.proxy_type);
    const openscp::ProxyType effectiveProxyType =
        (!proxySupportedInUi || hasJump) ? openscp::ProxyType::None
                                         : requestedProxyType;
    if (proxyType_) {
        int proxyTypeIndex =
            proxyType_->findData(static_cast<int>(effectiveProxyType));
        if (proxyTypeIndex >= 0)
            proxyType_->setCurrentIndex(proxyTypeIndex);
    }
    if (effectiveProxyType != openscp::ProxyType::None &&
        !options.proxy_host.empty())
        proxyHost_->setText(QString::fromStdString(options.proxy_host));
    if (effectiveProxyType != openscp::ProxyType::None) {
        const std::uint16_t effectiveProxyPort =
            (options.proxy_port != 0)
                ? options.proxy_port
                : openscp::defaultPortForProxyType(effectiveProxyType);
        if (effectiveProxyPort != 0)
            proxyPort_->setValue(static_cast<int>(effectiveProxyPort));
    }
    if (effectiveProxyType != openscp::ProxyType::None &&
        options.proxy_username && !options.proxy_username->empty())
        proxyUser_->setText(QString::fromStdString(*options.proxy_username));
    if (effectiveProxyType != openscp::ProxyType::None &&
        options.proxy_password && !options.proxy_password->empty())
        proxyPass_->setText(QString::fromUtf8(
            options.proxy_password->data(),
            static_cast<qsizetype>(options.proxy_password->size())));
    if (jumpEnabled_)
        jumpEnabled_->setChecked(hasJump);
    if (hasJump)
        jumpHost_->setText(QString::fromStdString(*options.jump_host));
    if (options.jump_port != 0)
        jumpPort_->setValue(static_cast<int>(options.jump_port));
    if (options.jump_username && !options.jump_username->empty())
        jumpUser_->setText(QString::fromStdString(*options.jump_username));
    if (options.jump_private_key_path &&
        !options.jump_private_key_path->empty())
        jumpKeyPath_->setText(
            QString::fromStdString(*options.jump_private_key_path));

    const bool sshAuthSupported =
        effectiveProtocol == openscp::Protocol::Sftp ||
        effectiveProtocol == openscp::Protocol::Scp;
    const bool hasSshKey =
        (options.private_key_path && !options.private_key_path->empty()) ||
        (options.private_key_passphrase &&
         !options.private_key_passphrase->empty());
    if (sshKeySection_)
        sshKeySection_->setExpanded(sshAuthSupported && hasSshKey);

    if (networkSection_)
        networkSection_->setExpanded(hasJump || effectiveProxyType !=
                                                    openscp::ProxyType::None);

    const bool hasCustomSecurity =
        (options.known_hosts_path && !options.known_hosts_path->empty()) ||
        options.known_hosts_policy != openscp::KnownHostsPolicy::Strict ||
        options.transfer_integrity_policy !=
            openscp::TransferIntegrityPolicy::Optional ||
        (effectiveProtocol == openscp::Protocol::Ftps &&
         (!options.ftps_verify_peer ||
          (options.ftps_ca_cert_path &&
           !options.ftps_ca_cert_path->empty()))) ||
        (effectiveProtocol == openscp::Protocol::WebDav &&
         (!options.webdav_verify_peer ||
          (options.webdav_ca_cert_path &&
           !options.webdav_ca_cert_path->empty())));
    if (securitySection_)
        securitySection_->setExpanded(hasCustomSecurity);

    updateProtocolUi(effectiveProtocol, false);
    updateSectionSummaries();
    adjustToContent();
}

void ConnectionDialog::updateProtocolUi(openscp::Protocol protocol,
                                        bool resetPort) {
    if (!host_ || !port_)
        return;
    const bool isWebDavProtocol = (protocol == openscp::Protocol::WebDav);
    const bool isFtpsProtocol = (protocol == openscp::Protocol::Ftps);
    const bool securityExpanded =
        securitySection_ && securitySection_->isExpanded();
    const auto selectedWebDavScheme =
        webDavScheme_
            ? openscp::normalizeWebDavScheme(static_cast<openscp::WebDavScheme>(
                  webDavScheme_->currentData().toInt()))
            : openscp::WebDavScheme::Https;
    if (formLayout_ && scpMode_) {
        const bool isScpProtocol = (protocol == openscp::Protocol::Scp);
        setFormRowVisible(formLayout_, scpMode_, isScpProtocol);
    }
    if (formLayout_) {
        if (ftpsMode_) {
            setFormRowVisible(formLayout_, ftpsMode_, isFtpsProtocol);
        }
        if (ftpsVerifyPeer_) {
            setFormRowVisible(formLayout_, ftpsVerifyPeer_,
                              isFtpsProtocol && securityExpanded);
        }
        if (ftpsCaPathRow_) {
            setFormRowVisible(formLayout_, ftpsCaPathRow_,
                              isFtpsProtocol && securityExpanded);
        }
        if (webDavScheme_) {
            setFormRowVisible(formLayout_, webDavScheme_, isWebDavProtocol);
        }
        if (webDavBasePath_) {
            setFormRowVisible(formLayout_, webDavBasePath_, isWebDavProtocol);
        }
        const bool showWebDavTlsRows =
            isWebDavProtocol && securityExpanded &&
            (selectedWebDavScheme == openscp::WebDavScheme::Https);
        if (webDavVerifyPeer_) {
            setFormRowVisible(formLayout_, webDavVerifyPeer_,
                              showWebDavTlsRows);
        }
        if (webDavCaPathRow_) {
            setFormRowVisible(formLayout_, webDavCaPathRow_, showWebDavTlsRows);
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
    if (resetPort) {
        std::uint16_t protocolDefaultPort =
            openscp::defaultPortForProtocol(protocol);
        if (protocol == openscp::Protocol::Ftps && ftpsMode_) {
            const auto mode =
                openscp::normalizeFtpsMode(static_cast<openscp::FtpsMode>(
                    ftpsMode_->currentData().toInt()));
            if (mode == openscp::FtpsMode::ExplicitTls)
                protocolDefaultPort = 21;
            else if (mode == openscp::FtpsMode::ImplicitTls)
                protocolDefaultPort = 990;
        }
        if (isWebDavProtocol) {
            protocolDefaultPort =
                openscp::defaultPortForWebDavScheme(selectedWebDavScheme);
        }
        port_->setValue(static_cast<int>(protocolDefaultPort));
    }

    const bool sshAuthSupported = (protocol == openscp::Protocol::Sftp ||
                                   protocol == openscp::Protocol::Scp);
    const bool sshKeyExpanded = sshKeySection_ && sshKeySection_->isExpanded();
    if (sshKeySection_)
        sshKeySection_->setVisible(sshAuthSupported);
    setFormRowsVisible(formLayout_, sshAuthSupported && sshKeyExpanded,
                       {keyPathRow_, keyPassRow_});

    const bool securityAvailable = caps.supports_known_hosts ||
                                   caps.can_checksum || isFtpsProtocol ||
                                   isWebDavProtocol;
    if (securitySection_)
        securitySection_->setVisible(securityAvailable);
    setFormRowsVisible(formLayout_,
                       caps.supports_known_hosts && securityExpanded,
                       {khPathRow_, khPolicy_});
    setFormRowVisible(formLayout_, integrityPolicy_,
                      caps.can_checksum && securityExpanded);

    bool jumpSupported = caps.supports_jump_host;
#ifdef Q_OS_WIN
    jumpSupported = false;
#endif
    const bool networkExpanded =
        networkSection_ && networkSection_->isExpanded();
    if (networkSection_)
        networkSection_->setVisible(caps.supports_proxy || jumpSupported);

    if (proxyType_) {
        if (!caps.supports_proxy) {
            const int directIdx = proxyType_->findData(
                static_cast<int>(openscp::ProxyType::None));
            if (directIdx >= 0 && proxyType_->currentIndex() != directIdx) {
                const QSignalBlocker blocker(proxyType_);
                proxyType_->setCurrentIndex(directIdx);
            }
        }
        if (formLayout_)
            setFormRowVisible(formLayout_, proxyType_,
                              caps.supports_proxy && networkExpanded);
        const auto selectedProxyType = openscp::normalizeProxyType(
            static_cast<openscp::ProxyType>(proxyType_->currentData().toInt()));
        const bool showProxyRows =
            caps.supports_proxy && networkExpanded &&
            (selectedProxyType != openscp::ProxyType::None);
        setFormRowsVisible(formLayout_, showProxyRows,
                           {proxyHostPortRow_, proxyUser_, proxyPassRow_});
    }

    if (jumpEnabled_) {
        if (!jumpSupported && jumpEnabled_->isChecked()) {
            const QSignalBlocker blocker(jumpEnabled_);
            jumpEnabled_->setChecked(false);
        }
        if (formLayout_)
            setFormRowVisible(formLayout_, jumpEnabled_,
                              jumpSupported && networkExpanded);
        jumpEnabled_->setToolTip(
            jumpSupported ? QString()
                          : tr("Not available for the selected protocol."));
        const bool showJumpRows =
            jumpSupported && networkExpanded && jumpEnabled_->isChecked();
        setFormRowsVisible(formLayout_, showJumpRows,
                           {jumpHostPortRow_, jumpUser_, jumpKeyPathRow_});
    }
    updateSectionSummaries();
}
