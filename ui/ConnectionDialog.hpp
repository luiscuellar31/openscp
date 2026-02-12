// Dialog to capture SFTP connection options (host/port/user/key/known_hosts).
#pragma once
#include <QDialog>
#include <QString>
#include "openscp/SftpTypes.hpp"

class QLineEdit;
class QSpinBox;
class QComboBox;
class QToolButton;

class ConnectionDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConnectionDialog(QWidget* parent = nullptr);
    openscp::SessionOptions options() const;
    void setOptions(const openscp::SessionOptions& opt);
    void setSiteNameVisible(bool visible);
    void setSiteName(const QString& name);
    QString siteName() const;

private:
    QLineEdit* siteName_ = nullptr;
    QWidget* siteNameLabel_ = nullptr;
    QLineEdit* host_ = nullptr;
    QSpinBox* port_ = nullptr;
    QLineEdit* user_ = nullptr;
    QLineEdit* pass_ = nullptr;
    QLineEdit* keyPath_ = nullptr;   // path to ~/.ssh/id_ed25519 or similar
    QLineEdit* keyPass_ = nullptr;   // key passphrase (if any)

    // known_hosts
    QLineEdit* khPath_ = nullptr;
    QToolButton* khBrowse_ = nullptr;
    QComboBox* khPolicy_ = nullptr;
};
