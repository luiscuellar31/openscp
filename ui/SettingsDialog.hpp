// Settings dialog: language, hidden files, click mode, and startup behavior.
#pragma once
#include <QDialog>
#include <QVector>

class QComboBox;
class QPushButton;
class QCheckBox;
class QResizeEvent;
class QFontMetrics;

class SettingsDialog : public QDialog {
    Q_OBJECT
  public:
    explicit SettingsDialog(QWidget *parent = nullptr);

  protected:
    void resizeEvent(QResizeEvent *event) override;

  private slots:
    void onApply(); // save without closing; disable until further changes
    void updateApplyFromControls(); // enable Apply if any option differs from
                                    // persisted

  private:
    static QString wrapTextToWidth(const QString &text, const QFontMetrics &fm,
                                   int maxWidth);
    void refreshWrappedCheckTexts();
    void trackWrappedCheck(QCheckBox *cb);

    QComboBox *langCombo_ = nullptr;        // es/en
    QCheckBox *showHidden_ = nullptr;       // show hidden files
    QComboBox *clickMode_ = nullptr;        // single click vs double click
    QComboBox *openBehaviorMode_ = nullptr; // ask/reveal/open downloaded files
    QCheckBox *showQueueOnEnqueue_ =
        nullptr; // auto-open transfer queue after enqueue
    class QLineEdit *defaultDownloadDirEdit_ =
        nullptr; // default local download directory
    class QPushButton *defaultDownloadBrowseBtn_ = nullptr;
    class QPushButton *resetMainLayoutBtn_ = nullptr;
    QComboBox *defaultProtocol_ = nullptr; // default protocol in connect/site dialogs
    QComboBox *scpModeDefault_ = nullptr; // default SCP transfer mode
    QCheckBox *showConnOnStart_ = nullptr; // open Site Manager at startup
    QCheckBox *showConnOnDisconnect_ =
        nullptr; // open Site Manager on disconnect
    QCheckBox *deleteSecretsOnRemove_ =
        nullptr; // when deleting a site, also delete its stored credentials
                 // (off by default)
#if defined(Q_OS_MAC) || defined(Q_OS_MACOS) || defined(__APPLE__)
    QCheckBox *macKeychainRestrictive_ =
        nullptr; // macOS: stricter Keychain accessibility (this device only)
#endif
    QCheckBox *knownHostsHashed_ =
        nullptr; // save hostnames hashed in known_hosts (recommended)
    QCheckBox *fpHex_ =
        nullptr; // show fingerprints in HEX colon format (visual only)
    QCheckBox *terminalForceInteractiveLogin_ =
        nullptr; // force password/kbd-interactive login in "Open in terminal"
    QCheckBox *terminalEnableSftpCliFallback_ =
        nullptr; // allow SSH terminal command to fallback to SFTP CLI
    QComboBox *defaultKnownHostsPolicy_ =
        nullptr; // default policy for new connections/sites
    QComboBox *defaultIntegrityPolicy_ =
        nullptr; // default integrity policy for new connections/sites
    class QSpinBox *noHostVerifyTtlMinSpin_ =
        nullptr; // temporary "no host verification" ttl in minutes
    QCheckBox *insecureFallback_ =
        nullptr; // allow insecure secret fallback (not recommended)
    class QSpinBox *maxConcurrentSpin_ = nullptr; // transfer worker concurrency
    class QSpinBox *globalSpeedDefaultSpin_ =
        nullptr; // default global speed limit KB/s (0 = unlimited)
    QComboBox *queueAutoClearModeDefault_ =
        nullptr; // default auto-clear mode for the transfer queue
    class QSpinBox *queueAutoClearMinutesDefaultSpin_ =
        nullptr; // default auto-clear delay in minutes
    class QLineEdit *stagingRootEdit_ = nullptr; // staging folder path
    class QPushButton *stagingBrowseBtn_ = nullptr;
    QCheckBox *autoCleanStaging_ =
        nullptr; // Auto-clean staging after successful drag-out
    class QSpinBox *stagingRetentionDaysSpin_ =
        nullptr; // startup cleanup retention for old staging batches
    class QSpinBox *stagingPrepTimeoutMsSpin_ =
        nullptr; // timeout before "wait/cancel" prompt
    class QSpinBox *stagingConfirmItemsSpin_ =
        nullptr; // threshold for "large batch" confirmation
    class QSpinBox *stagingConfirmMiBSpin_ =
        nullptr; // threshold in MiB for "large batch" confirmation
    class QSpinBox *maxDepthSpin_ = nullptr; // Advanced/maxFolderDepth
    class QSpinBox *sessionHealthIntervalSecSpin_ =
        nullptr; // remote session health probe interval
    class QSpinBox *remoteWriteabilityTtlMsSpin_ =
        nullptr; // cache ttl for remote writeability checks
    QPushButton *applyBtn_ =
        nullptr; // Apply button (enabled only when modified)
    QPushButton *closeBtn_ = nullptr; // Close button (never primary/default)
    QVector<QCheckBox *> wrappedChecks_; // checkboxes with auto text wrapping
};
