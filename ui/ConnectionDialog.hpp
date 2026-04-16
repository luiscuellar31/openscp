// Dialog to capture remote connection options (protocol/host/auth/security).
#pragma once
#include "openscp/SftpTypes.hpp"
#include <QDialog>
#include <QSize>
#include <QString>

class QLineEdit;
class QSpinBox;
class QComboBox;
class QToolButton;
class QCheckBox;
class QFormLayout;

class ConnectionDialog : public QDialog {
    Q_OBJECT
    public:
    explicit ConnectionDialog(QWidget *parent = nullptr);
    openscp::SessionOptions options() const;
    void setOptions(const openscp::SessionOptions &opt);
    void setSiteNameVisible(bool visible);
    void setSiteName(const QString &name);
    QString siteName() const;
    void setQuickConnectSaveOptionsVisible(bool visible);
    bool saveSiteRequested() const;
    bool saveCredentialsRequested() const;

    private:
    void updateProtocolUi(openscp::Protocol protocol);

    bool quickConnectSaveOptionsVisible_ = false;
    QFormLayout *formLayout_ = nullptr;
    QComboBox *protocol_ = nullptr;
    QComboBox *scpMode_ = nullptr;
    QLineEdit *siteName_ = nullptr;
    QWidget *siteNameLabel_ = nullptr;
    QCheckBox *saveSite_ = nullptr;
    QCheckBox *saveCredentials_ = nullptr;
    QLineEdit *host_ = nullptr;
    QSpinBox *port_ = nullptr;
    QLineEdit *user_ = nullptr;
    QLineEdit *pass_ = nullptr;
    QLineEdit *keyPath_ = nullptr; // path to ~/.ssh/id_ed25519 or similar
    QLineEdit *keyPass_ = nullptr; // key passphrase (if any)
    QWidget *keyPathRow_ = nullptr;
    QWidget *keyPassRow_ = nullptr;

    // known_hosts
    QLineEdit *khPath_ = nullptr;
    QToolButton *khBrowse_ = nullptr;
    QComboBox *khPolicy_ = nullptr;
    QComboBox *integrityPolicy_ = nullptr;
    QWidget *khPathRow_ = nullptr;
    QCheckBox *ftpsVerifyPeer_ = nullptr;
    QLineEdit *ftpsCaPath_ = nullptr;
    QToolButton *ftpsCaBrowse_ = nullptr;
    QWidget *ftpsCaPathRow_ = nullptr;

    // proxy
    QComboBox *proxyType_ = nullptr;
    QLineEdit *proxyHost_ = nullptr;
    QSpinBox *proxyPort_ = nullptr;
    QLineEdit *proxyUser_ = nullptr;
    QLineEdit *proxyPass_ = nullptr;
    QWidget *proxyHostPortRow_ = nullptr;
    QWidget *proxyPassRow_ = nullptr;
    QCheckBox *jumpEnabled_ = nullptr;
    QLineEdit *jumpHost_ = nullptr;
    QSpinBox *jumpPort_ = nullptr;
    QLineEdit *jumpUser_ = nullptr;
    QLineEdit *jumpKeyPath_ = nullptr;
    QToolButton *jumpKeyBrowse_ = nullptr;
    QWidget *jumpHostPortRow_ = nullptr;
    QWidget *jumpKeyPathRow_ = nullptr;

    // Keep the compact dialog size when proxy rows are hidden (Direct mode).
    QSize directModeSize_;
    bool hasDirectModeSize_ = false;
    bool proxyRowsVisible_ = false;
    openscp::ProxyType lastProxyType_ = openscp::ProxyType::None;
    bool jumpRowsVisible_ = false;
};
