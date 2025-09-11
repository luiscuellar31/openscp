// Settings dialog: language, hidden files, click mode, and startup behavior.
#pragma once
#include <QDialog>

class QComboBox;
class QPushButton;
class QCheckBox;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

private slots:
    void onApply(); // save without closing; disable until further changes
    void updateApplyFromControls(); // enable Apply if any option differs from persisted

private:
    QComboBox* langCombo_  = nullptr;   // es/en
    QCheckBox* showHidden_ = nullptr;   // show hidden files
    QComboBox* clickMode_  = nullptr;   // single click vs double click
    QCheckBox* showConnOnStart_ = nullptr; // show site manager at startup and when closing last session
    QCheckBox* openInFolder_ = nullptr; // open downloaded files by revealing in folder instead of opening directly
    QCheckBox* deleteSecretsOnRemove_ = nullptr; // when deleting a site, also delete its stored credentials (off by default)
#if defined(Q_OS_MAC) || defined(Q_OS_MACOS) || defined(__APPLE__)
    QCheckBox* macKeychainRestrictive_ = nullptr; // macOS: stricter Keychain accessibility (this device only)
#endif
    QCheckBox* knownHostsHashed_ = nullptr; // save hostnames hashed in known_hosts (recommended)
    QCheckBox* fpHex_ = nullptr; // show fingerprints in HEX colon format (visual only)
    QCheckBox* insecureFallback_ = nullptr; // allow insecure secret fallback (not recommended)
    class QLineEdit* stagingRootEdit_ = nullptr; // staging folder path
    class QPushButton* stagingBrowseBtn_ = nullptr;
    QCheckBox* autoCleanStaging_ = nullptr; // Auto-clean staging after successful drag-out
    class QSpinBox* maxDepthSpin_ = nullptr; // Advanced/maxFolderDepth
    QPushButton* applyBtn_ = nullptr;   // Apply button (enabled only when modified)
    QPushButton* closeBtn_ = nullptr;   // Close button (never primary/default)
};
