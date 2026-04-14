// Implementation of OpenSCP settings dialog.
#include "SettingsDialog.hpp"
#include "UiAlerts.hpp"
#include "openscp/SftpTypes.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

static QString defaultDownloadDirPath() {
    QString p =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (p.isEmpty())
        p = QDir::homePath() + "/Downloads";
    return p;
}

constexpr int kQueueAutoClearOff = 0;
constexpr int kQueueAutoClearCompleted = 1;
constexpr int kQueueAutoClearFailedCanceled = 2;
constexpr int kQueueAutoClearFinished = 3;

QString SettingsDialog::wrapTextToWidth(const QString &text,
                                        const QFontMetrics &fm,
                                        int maxWidth) {
    if (text.isEmpty() || maxWidth <= 0)
        return text;

    const QStringList paragraphs = text.split('\n', Qt::KeepEmptyParts);
    QStringList wrappedParagraphs;
    wrappedParagraphs.reserve(paragraphs.size());

    for (const QString &paragraph : paragraphs) {
        if (paragraph.trimmed().isEmpty()) {
            wrappedParagraphs << QString();
            continue;
        }
        const QStringList words = paragraph.split(' ', Qt::SkipEmptyParts);
        QStringList lines;
        QString current;
        for (const QString &word : words) {
            const QString candidate =
                current.isEmpty() ? word : (current + QStringLiteral(" ") + word);
            if (current.isEmpty() || fm.horizontalAdvance(candidate) <= maxWidth) {
                current = candidate;
                continue;
            }
            lines << current;
            current = word;
        }
        if (!current.isEmpty())
            lines << current;
        wrappedParagraphs << lines.join('\n');
    }
    return wrappedParagraphs.join('\n');
}

void SettingsDialog::trackWrappedCheck(QCheckBox *cb) {
    if (!cb)
        return;
    cb->setProperty("rawText", cb->text());
    cb->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    wrappedChecks_.push_back(cb);
}

void SettingsDialog::refreshWrappedCheckTexts() {
    for (QCheckBox *cb : wrappedChecks_) {
        if (!cb)
            continue;
        const QString raw = cb->property("rawText").toString();
        if (raw.isEmpty())
            continue;

        int widgetWidth = cb->width();
        if (widgetWidth <= 0)
            widgetWidth = cb->contentsRect().width();
        if (widgetWidth <= 0) {
            int viewportWidth = 0;
            for (QWidget *p = cb->parentWidget(); p; p = p->parentWidget()) {
                if (auto *scroll = qobject_cast<QScrollArea *>(p)) {
                    viewportWidth = scroll->viewport()
                                        ? scroll->viewport()->width()
                                        : scroll->width();
                    break;
                }
            }
            widgetWidth = viewportWidth > 0 ? viewportWidth : width();
        }

        const int indicatorW =
            cb->style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, cb);
        const int indicatorH =
            cb->style()->pixelMetric(QStyle::PM_IndicatorHeight, nullptr, cb);
        const int spacingW = cb->style()->pixelMetric(
            QStyle::PM_CheckBoxLabelSpacing, nullptr, cb);
        const int textWidth = qMax(100, widgetWidth - indicatorW - spacingW - 10);
        const QString wrapped = wrapTextToWidth(raw, QFontMetrics(cb->font()),
                                                textWidth);
        if (cb->text() != wrapped) {
            cb->setText(wrapped);
            cb->updateGeometry();
        }
        const int lineCount = wrapped.count('\n') + 1;
        const int textHeight = lineCount * QFontMetrics(cb->font()).lineSpacing();
        const int minHeight = qMax(indicatorH, textHeight) + 6;
        if (cb->minimumHeight() != minHeight)
            cb->setMinimumHeight(minHeight);
    }
}

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Settings"));
    resize(860, 620);
    setMinimumSize(760, 560);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto *contentRow = new QHBoxLayout();
    contentRow->setContentsMargins(0, 0, 0, 0);
    contentRow->setSpacing(10);
    root->addLayout(contentRow, 1);

    auto *sectionList = new QListWidget(this);
    sectionList->setSelectionMode(QAbstractItemView::SingleSelection);
    sectionList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sectionList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    sectionList->setFixedWidth(150);
    sectionList->setStyleSheet(
        "QListWidget { border: 1px solid palette(midlight); border-radius: "
        "8px; padding: 6px; }"
        "QListWidget::item { padding: 8px 10px; border-radius: 6px; margin: "
        "1px 0; }"
        "QListWidget::item:selected { background: palette(highlight); color: "
        "palette(highlighted-text); }");
    contentRow->addWidget(sectionList);

    auto *pages = new QStackedWidget(this);
    contentRow->addWidget(pages, 1);
    auto recalcSectionListWidth = [sectionList]() {
        const QFontMetrics fm(sectionList->font());
        int maxTextWidth = 0;
        for (int i = 0; i < sectionList->count(); ++i) {
            if (auto *item = sectionList->item(i)) {
                maxTextWidth = qMax(maxTextWidth, fm.horizontalAdvance(item->text()));
            }
        }
        // Padding/frame/margins for the list plus translated label width.
        const int width = qBound(150, maxTextWidth + 48, 260);
        sectionList->setFixedWidth(width);
    };

    constexpr int kFieldMinWidth = 320;
    constexpr int kFieldMaxWidth = 520;
    auto addLabeledRow = [](QFormLayout *target, QWidget *parent,
                            const QString &labelText, QWidget *field) {
        auto *label = new QLabel(labelText, parent);
        label->setWordWrap(true);
        label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        target->addRow(label, field);
    };
    auto createFormPage = [pages, sectionList, recalcSectionListWidth](
                              const QString &title,
                              QFormLayout *&outForm) -> QWidget * {
        auto *scroll = new QScrollArea(pages);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *page = new QWidget(scroll);
        auto *pageLay = new QVBoxLayout(page);
        pageLay->setContentsMargins(12, 12, 12, 12);
        pageLay->setSpacing(8);
        auto *form = new QFormLayout();
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setHorizontalSpacing(8);
        form->setVerticalSpacing(8);
        pageLay->addLayout(form);
        pageLay->addStretch(1);
        scroll->setWidget(page);
        pages->addWidget(scroll);
        sectionList->addItem(title);
        recalcSectionListWidth();
        outForm = form;
        return page;
    };
    auto addCheckRow = [this](QFormLayout *target, QWidget *parent,
                              const QString &text) {
        auto *cb = new QCheckBox(text, parent);
        trackWrappedCheck(cb);
        target->addRow(QString(), cb);
        return cb;
    };
    auto addComboRow = [kFieldMinWidth,
                        kFieldMaxWidth,
                        addLabeledRow](QFormLayout *target, QWidget *parent,
                                        const QString &labelText) {
        auto *combo = new QComboBox(parent);
        combo->setMinimumWidth(kFieldMinWidth);
        combo->setMaximumWidth(kFieldMaxWidth);
        addLabeledRow(target, parent, labelText, combo);
        return combo;
    };
    auto bindDirtyFlag = [this]<typename Sender, typename Signal>(
                             Sender *sender, Signal signal) {
        if (sender)
            connect(sender, signal, this,
                    &SettingsDialog::updateApplyFromControls);
    };

    QFormLayout *generalForm = nullptr;
    QWidget *generalPage = createFormPage(tr("General"), generalForm);

    // General settings
    langCombo_ = addComboRow(generalForm, generalPage, tr("Language:"));
    langCombo_->addItem(tr("Spanish"), "es");
    langCombo_->addItem(tr("English"), "en");
    langCombo_->addItem(tr("French"), "fr");
    langCombo_->addItem(tr("Portuguese"), "pt");
    clickMode_ = addComboRow(generalForm, generalPage, tr("Open with:"));
    clickMode_->addItem(tr("Double click"), 2);
    clickMode_->addItem(tr("Single click"), 1);
    openBehaviorMode_ =
        addComboRow(generalForm, generalPage, tr("On file open:"));
    openBehaviorMode_->addItem(tr("Always ask"), QStringLiteral("ask"));
    openBehaviorMode_->addItem(tr("Show folder"), QStringLiteral("reveal"));
    openBehaviorMode_->addItem(tr("Open file"), QStringLiteral("open"));

    showHidden_ =
        addCheckRow(generalForm, generalPage, tr("Show hidden files"));
    showConnOnStart_ = addCheckRow(generalForm, generalPage,
                                   tr("Open Site Manager on startup"));
    showConnOnDisconnect_ = addCheckRow(generalForm, generalPage,
                                        tr("Open Site Manager on disconnect"));
    showQueueOnEnqueue_ = addCheckRow(
        generalForm, generalPage, tr("Open queue when enqueuing transfers"));
    {
        auto *rowWidget = new QWidget(generalPage);
        auto *row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        defaultDownloadDirEdit_ = new QLineEdit(generalPage);
        defaultDownloadDirEdit_->setMinimumWidth(kFieldMinWidth);
        defaultDownloadDirEdit_->setMaximumWidth(kFieldMaxWidth);
        defaultDownloadBrowseBtn_ = new QPushButton(tr("Choose…"), generalPage);
        row->addWidget(defaultDownloadDirEdit_, 1);
        row->addWidget(defaultDownloadBrowseBtn_);
        addLabeledRow(generalForm, generalPage, tr("Download folder:"),
                      rowWidget);
        connect(defaultDownloadBrowseBtn_, &QPushButton::clicked, this, [this] {
            const QString cur = defaultDownloadDirEdit_
                                    ? defaultDownloadDirEdit_->text()
                                    : QString();
            const QString pick = QFileDialog::getExistingDirectory(
                this, tr("Select download folder"),
                cur.isEmpty() ? QDir::homePath() : cur);
            if (!pick.isEmpty() && defaultDownloadDirEdit_)
                defaultDownloadDirEdit_->setText(pick);
        });
    }
    {
        resetMainLayoutBtn_ =
            new QPushButton(tr("Restore default sizes"), generalPage);
        addLabeledRow(generalForm, generalPage, tr("Window layout:"),
                      resetMainLayoutBtn_);
        connect(resetMainLayoutBtn_, &QPushButton::clicked, this, [this] {
            const auto ret = UiAlerts::question(
                this, tr("Restore layout"),
                tr("Restore the main window layout and column sizes to "
                   "their defaults?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (ret != QMessageBox::Yes)
                return;

            QSettings s("OpenSCP", "OpenSCP");
            s.remove("UI/mainWindow/geometry");
            s.remove("UI/mainWindow/windowState");
            s.remove("UI/mainWindow/splitterState");
            s.remove("UI/mainWindow/leftHeaderState");
            s.remove("UI/mainWindow/rightHeaderLocal");
            s.remove("UI/mainWindow/rightHeaderRemote");
            s.sync();

            bool appliedNow = false;
            if (QWidget *w = parentWidget()) {
                appliedNow = QMetaObject::invokeMethod(
                    w, "resetMainWindowLayoutToDefaults",
                    Qt::DirectConnection);
            }

            UiAlerts::information(
                this, tr("Restore layout"),
                appliedNow
                    ? tr("Default layout restored.")
                    : tr("Default layout will be used the next time the app "
                         "starts."));
        });
    }

    QFormLayout *transfersForm = nullptr;
    QWidget *transfersPage = createFormPage(tr("Transfers"), transfersForm);
    transfersForm->setVerticalSpacing(10);
    maxConcurrentSpin_ = new QSpinBox(transfersPage);
    maxConcurrentSpin_->setRange(1, 8);
    maxConcurrentSpin_->setValue(2);
    maxConcurrentSpin_->setMinimumWidth(90);
    maxConcurrentSpin_->setToolTip(
        tr("Maximum number of concurrent transfers."));
    addLabeledRow(transfersForm, transfersPage, tr("Parallel tasks:"),
                  maxConcurrentSpin_);
    globalSpeedDefaultSpin_ = new QSpinBox(transfersPage);
    globalSpeedDefaultSpin_->setRange(0, 1'000'000);
    globalSpeedDefaultSpin_->setValue(0);
    globalSpeedDefaultSpin_->setSuffix(" KB/s");
    globalSpeedDefaultSpin_->setMinimumWidth(120);
    globalSpeedDefaultSpin_->setToolTip(tr("0 = no global speed limit."));
    addLabeledRow(transfersForm, transfersPage, tr("Default global limit:"),
                  globalSpeedDefaultSpin_);
    queueAutoClearModeDefault_ = new QComboBox(transfersPage);
    queueAutoClearModeDefault_->setMinimumWidth(kFieldMinWidth);
    queueAutoClearModeDefault_->setMaximumWidth(kFieldMaxWidth);
    queueAutoClearModeDefault_->addItem(tr("Off"), kQueueAutoClearOff);
    queueAutoClearModeDefault_->addItem(tr("Completed"),
                                        kQueueAutoClearCompleted);
    queueAutoClearModeDefault_->addItem(tr("Failed/Canceled"),
                                        kQueueAutoClearFailedCanceled);
    queueAutoClearModeDefault_->addItem(tr("All finished"),
                                        kQueueAutoClearFinished);
    addLabeledRow(transfersForm, transfersPage, tr("Queue auto-clear default:"),
                  queueAutoClearModeDefault_);
    queueAutoClearMinutesDefaultSpin_ = new QSpinBox(transfersPage);
    queueAutoClearMinutesDefaultSpin_->setRange(1, 1440);
    queueAutoClearMinutesDefaultSpin_->setValue(15);
    queueAutoClearMinutesDefaultSpin_->setSuffix(tr(" min"));
    queueAutoClearMinutesDefaultSpin_->setMinimumWidth(100);
    addLabeledRow(transfersForm, transfersPage, tr("Queue auto-clear after:"),
                  queueAutoClearMinutesDefaultSpin_);
    auto updateQueueAutoClearDefaultsUi = [this]() {
        if (!queueAutoClearModeDefault_ || !queueAutoClearMinutesDefaultSpin_)
            return;
        const bool enabled =
            queueAutoClearModeDefault_->currentData().toInt() !=
            kQueueAutoClearOff;
        queueAutoClearMinutesDefaultSpin_->setEnabled(enabled);
    };
    connect(queueAutoClearModeDefault_, &QComboBox::currentIndexChanged, this,
            [this, updateQueueAutoClearDefaultsUi](int) {
                updateQueueAutoClearDefaultsUi();
                updateApplyFromControls();
            });

    QFormLayout *sitesForm = nullptr;
    QWidget *sitesPage = createFormPage(tr("Sites"), sitesForm);
    sitesForm->setVerticalSpacing(8);
    defaultProtocol_ = new QComboBox(sitesPage);
    defaultProtocol_->setMinimumWidth(kFieldMinWidth);
    defaultProtocol_->setMaximumWidth(kFieldMaxWidth);
    defaultProtocol_->addItem(tr("SFTP"),
                              static_cast<int>(openscp::Protocol::Sftp));
    defaultProtocol_->addItem(tr("SCP"),
                              static_cast<int>(openscp::Protocol::Scp));
    defaultProtocol_->addItem(tr("FTP"),
                              static_cast<int>(openscp::Protocol::Ftp));
    defaultProtocol_->addItem(tr("FTPS"),
                              static_cast<int>(openscp::Protocol::Ftps));
    defaultProtocol_->addItem(tr("WebDAV"),
                              static_cast<int>(openscp::Protocol::WebDav));
    addLabeledRow(sitesForm, sitesPage, tr("Default protocol:"),
                  defaultProtocol_);
    scpModeDefault_ = new QComboBox(sitesPage);
    scpModeDefault_->setMinimumWidth(kFieldMinWidth);
    scpModeDefault_->setMaximumWidth(kFieldMaxWidth);
    scpModeDefault_->addItem(
        tr("Automatic (SCP with SFTP fallback)"),
        static_cast<int>(openscp::ScpTransferMode::Auto));
    scpModeDefault_->addItem(
        tr("SCP only (disable SFTP fallback)"),
        static_cast<int>(openscp::ScpTransferMode::ScpOnly));
    addLabeledRow(sitesForm, sitesPage, tr("Default SCP mode:"),
                  scpModeDefault_);
    deleteSecretsOnRemove_ = addCheckRow(
        sitesForm, sitesPage,
        tr("When deleting a site, also remove its stored credentials."));

    QFormLayout *securityForm = nullptr;
    QWidget *securityPage = createFormPage(tr("Security"), securityForm);
    securityForm->setVerticalSpacing(8);
    defaultKnownHostsPolicy_ = new QComboBox(securityPage);
    defaultKnownHostsPolicy_->setMinimumWidth(kFieldMinWidth);
    defaultKnownHostsPolicy_->setMaximumWidth(kFieldMaxWidth);
    defaultKnownHostsPolicy_->addItem(
        tr("Strict"), static_cast<int>(openscp::KnownHostsPolicy::Strict));
    defaultKnownHostsPolicy_->addItem(
        tr("Accept new (TOFU)"),
        static_cast<int>(openscp::KnownHostsPolicy::AcceptNew));
    defaultKnownHostsPolicy_->addItem(
        tr("No verification (double confirmation, expires in 15 min)"),
        static_cast<int>(openscp::KnownHostsPolicy::Off));
    addLabeledRow(securityForm, securityPage, tr("Default known_hosts policy:"),
                  defaultKnownHostsPolicy_);
    defaultIntegrityPolicy_ = new QComboBox(securityPage);
    defaultIntegrityPolicy_->setMinimumWidth(kFieldMinWidth);
    defaultIntegrityPolicy_->setMaximumWidth(kFieldMaxWidth);
    defaultIntegrityPolicy_->addItem(
        tr("Optional (recommended)"),
        static_cast<int>(openscp::TransferIntegrityPolicy::Optional));
    defaultIntegrityPolicy_->addItem(
        tr("Required (strict)"),
        static_cast<int>(openscp::TransferIntegrityPolicy::Required));
    defaultIntegrityPolicy_->addItem(
        tr("Off (not recommended)"),
        static_cast<int>(openscp::TransferIntegrityPolicy::Off));
    addLabeledRow(securityForm, securityPage, tr("Default integrity policy:"),
                  defaultIntegrityPolicy_);
    ftpsVerifyPeerDefault_ = new QCheckBox(
        tr("Verify FTPS server certificate by default (recommended)."),
        securityPage);
    trackWrappedCheck(ftpsVerifyPeerDefault_);
    securityForm->addRow(QString(), ftpsVerifyPeerDefault_);
    {
        auto *rowWidget = new QWidget(securityPage);
        auto *row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        ftpsCaCertPathDefaultEdit_ = new QLineEdit(securityPage);
        ftpsCaCertPathDefaultEdit_->setMinimumWidth(kFieldMinWidth);
        ftpsCaCertPathDefaultEdit_->setMaximumWidth(kFieldMaxWidth);
        ftpsCaCertPathDefaultEdit_->setPlaceholderText(tr("System CA bundle"));
        ftpsCaCertPathDefaultBrowseBtn_ =
            new QPushButton(tr("Choose…"), securityPage);
        row->addWidget(ftpsCaCertPathDefaultEdit_, 1);
        row->addWidget(ftpsCaCertPathDefaultBrowseBtn_);
        addLabeledRow(securityForm, securityPage, tr("Default FTPS CA bundle:"),
                      rowWidget);
        connect(ftpsCaCertPathDefaultBrowseBtn_, &QPushButton::clicked, this,
                [this] {
                    const QString cur = ftpsCaCertPathDefaultEdit_
                                            ? ftpsCaCertPathDefaultEdit_->text()
                                            : QString();
                    const QString pick = QFileDialog::getOpenFileName(
                        this, tr("Select FTPS CA bundle"),
                        cur.isEmpty() ? QDir::homePath() : cur);
                    if (!pick.isEmpty() && ftpsCaCertPathDefaultEdit_)
                        ftpsCaCertPathDefaultEdit_->setText(pick);
                });
    }
    knownHostsHashed_ = new QCheckBox(
        tr("Hash hostnames in known_hosts (recommended)."), securityPage);
    fpHex_ = new QCheckBox(
        tr("Show fingerprint in HEX (colon) format (visual only)."),
        securityPage);
    terminalForceInteractiveLogin_ = new QCheckBox(
        tr("Force interactive login when using Open in terminal "
           "(disable key/agent auth)."),
        securityPage);
    terminalEnableSftpCliFallback_ = new QCheckBox(
        tr("Enable automatic SFTP CLI fallback when using Open in terminal."),
        securityPage);
    trackWrappedCheck(knownHostsHashed_);
    trackWrappedCheck(fpHex_);
    trackWrappedCheck(terminalForceInteractiveLogin_);
    trackWrappedCheck(terminalEnableSftpCliFallback_);
    securityForm->addRow(QString(), knownHostsHashed_);
    securityForm->addRow(QString(), fpHex_);
    securityForm->addRow(QString(), terminalForceInteractiveLogin_);
    securityForm->addRow(QString(), terminalEnableSftpCliFallback_);
#if defined(Q_OS_MAC) || defined(Q_OS_MACOS) || defined(__APPLE__)
    macKeychainRestrictive_ = new QCheckBox(
        tr("Use stricter Keychain accessibility (this device only)."),
        securityPage);
    trackWrappedCheck(macKeychainRestrictive_);
    securityForm->addRow(QString(), macKeychainRestrictive_);
#endif
#if !defined(__APPLE__) && !defined(Q_OS_MAC) && !defined(Q_OS_MACOS) &&       \
    !defined(HAVE_LIBSECRET) && !defined(OPENSCP_BUILD_SECURE_ONLY)
    insecureFallback_ = new QCheckBox(
        tr("Allow insecure credentials fallback (not recommended)."),
        securityPage);
    trackWrappedCheck(insecureFallback_);
    securityForm->addRow(QString(), insecureFallback_);
#endif
    {
        auto *ttlRow = new QWidget(securityPage);
        auto *ttlLay = new QHBoxLayout(ttlRow);
        ttlLay->setContentsMargins(0, 0, 0, 0);
        ttlLay->setSpacing(6);
        noHostVerifyTtlMinSpin_ = new QSpinBox(securityPage);
        noHostVerifyTtlMinSpin_->setRange(1, 120);
        noHostVerifyTtlMinSpin_->setValue(15);
        noHostVerifyTtlMinSpin_->setSuffix(tr(" min"));
        noHostVerifyTtlMinSpin_->setMinimumWidth(100);
        noHostVerifyTtlMinSpin_->setToolTip(
            tr("Duration of the temporary exception for no host-key "
               "verification policy."));
        ttlLay->addWidget(noHostVerifyTtlMinSpin_);
        ttlLay->addStretch(1);
        addLabeledRow(securityForm, securityPage, tr("No-verification TTL:"),
                      ttlRow);
    }

    QFormLayout *networkForm = nullptr;
    QWidget *networkPage = createFormPage(tr("Network"), networkForm);
    networkForm->setVerticalSpacing(8);
    sessionHealthIntervalSecSpin_ = new QSpinBox(networkPage);
    sessionHealthIntervalSecSpin_->setRange(60, 86400);
    sessionHealthIntervalSecSpin_->setValue(600);
    sessionHealthIntervalSecSpin_->setSuffix(tr(" s"));
    sessionHealthIntervalSecSpin_->setMinimumWidth(100);
    addLabeledRow(networkForm, networkPage, tr("Session health check interval:"),
                  sessionHealthIntervalSecSpin_);
    remoteWriteabilityTtlMsSpin_ = new QSpinBox(networkPage);
    remoteWriteabilityTtlMsSpin_->setRange(1000, 120000);
    remoteWriteabilityTtlMsSpin_->setValue(15000);
    remoteWriteabilityTtlMsSpin_->setSuffix(tr(" ms"));
    remoteWriteabilityTtlMsSpin_->setSingleStep(500);
    remoteWriteabilityTtlMsSpin_->setMinimumWidth(110);
    addLabeledRow(networkForm, networkPage, tr("Remote writeability cache TTL:"),
                  remoteWriteabilityTtlMsSpin_);

    QFormLayout *stagingForm = nullptr;
    QWidget *stagingPage =
        createFormPage(tr("Staging and drag-out"), stagingForm);
    stagingForm->setVerticalSpacing(8);
    {
        auto *rowWidget = new QWidget(stagingPage);
        auto *row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        stagingRootEdit_ = new QLineEdit(stagingPage);
        stagingRootEdit_->setMinimumWidth(kFieldMinWidth);
        stagingRootEdit_->setMaximumWidth(kFieldMaxWidth);
        stagingBrowseBtn_ = new QPushButton(tr("Choose…"), stagingPage);
        row->addWidget(stagingRootEdit_, 1);
        row->addWidget(stagingBrowseBtn_);
        addLabeledRow(stagingForm, stagingPage, tr("Staging folder:"),
                      rowWidget);
        connect(stagingBrowseBtn_, &QPushButton::clicked, this, [this] {
            const QString cur =
                stagingRootEdit_ ? stagingRootEdit_->text() : QString();
            const QString pick = QFileDialog::getExistingDirectory(
                this, tr("Select staging folder"),
                cur.isEmpty() ? QDir::homePath() : cur);
            if (!pick.isEmpty() && stagingRootEdit_)
                stagingRootEdit_->setText(pick);
        });
    }
    autoCleanStaging_ = new QCheckBox(
        tr("Auto-clean staging after successful drag-out (recommended)."),
        stagingPage);
    trackWrappedCheck(autoCleanStaging_);
    stagingForm->addRow(QString(), autoCleanStaging_);
    stagingRetentionDaysSpin_ = new QSpinBox(stagingPage);
    stagingRetentionDaysSpin_->setRange(1, 365);
    stagingRetentionDaysSpin_->setValue(7);
    stagingRetentionDaysSpin_->setSuffix(tr(" days"));
    stagingRetentionDaysSpin_->setMinimumWidth(110);
    addLabeledRow(stagingForm, stagingPage, tr("Startup cleanup retention:"),
                  stagingRetentionDaysSpin_);
    stagingPrepTimeoutMsSpin_ = new QSpinBox(stagingPage);
    stagingPrepTimeoutMsSpin_->setRange(250, 60000);
    stagingPrepTimeoutMsSpin_->setSingleStep(250);
    stagingPrepTimeoutMsSpin_->setValue(2000);
    stagingPrepTimeoutMsSpin_->setSuffix(tr(" ms"));
    stagingPrepTimeoutMsSpin_->setMinimumWidth(110);
    stagingPrepTimeoutMsSpin_->setToolTip(
        tr("Time before showing the Wait/Cancel dialog."));
    addLabeledRow(stagingForm, stagingPage, tr("Preparation timeout:"),
                  stagingPrepTimeoutMsSpin_);
    stagingConfirmItemsSpin_ = new QSpinBox(stagingPage);
    stagingConfirmItemsSpin_->setRange(50, 100000);
    stagingConfirmItemsSpin_->setValue(500);
    stagingConfirmItemsSpin_->setMinimumWidth(110);
    stagingConfirmItemsSpin_->setToolTip(
        tr("Item count threshold to request confirmation for large batches."));
    addLabeledRow(stagingForm, stagingPage, tr("Confirm from items:"),
                  stagingConfirmItemsSpin_);
    stagingConfirmMiBSpin_ = new QSpinBox(stagingPage);
    stagingConfirmMiBSpin_->setRange(128, 65536);
    stagingConfirmMiBSpin_->setValue(1024);
    stagingConfirmMiBSpin_->setSuffix(tr(" MiB"));
    stagingConfirmMiBSpin_->setMinimumWidth(120);
    stagingConfirmMiBSpin_->setToolTip(tr(
        "Estimated size threshold to request confirmation for large batches."));
    addLabeledRow(stagingForm, stagingPage, tr("Confirm from size:"),
                  stagingConfirmMiBSpin_);

    // Maximum folder recursion depth
    {
        auto *rowWidget = new QWidget(stagingPage);
        auto *row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        maxDepthSpin_ = new QSpinBox(stagingPage);
        maxDepthSpin_->setRange(4, 256);
        maxDepthSpin_->setValue(32);
        maxDepthSpin_->setMinimumWidth(90);
        maxDepthSpin_->setToolTip(tr("Limit for recursive folder drag-out to "
                                     "avoid deep trees and loops."));
        auto *hint = new QLabel(tr("Recommended: 32"), stagingPage);
        hint->setStyleSheet("color: palette(window-text);");
        row->addWidget(maxDepthSpin_);
        row->addWidget(hint);
        row->addStretch(1);
        addLabeledRow(stagingForm, stagingPage, tr("Maximum depth:"),
                      rowWidget);
    }
    connect(sectionList, &QListWidget::currentRowChanged, pages,
            &QStackedWidget::setCurrentIndex);
    connect(sectionList, &QListWidget::currentRowChanged, this,
            [this](int) {
                refreshWrappedCheckTexts();
                QTimer::singleShot(0, this,
                                   [this] { refreshWrappedCheckTexts(); });
            });
    sectionList->setCurrentRow(0);

    // Buttons row: align to right, order: Close (left) then Apply (right)
    auto *btnRow = new QWidget(this);
    auto *hb = new QHBoxLayout(btnRow);
    hb->setContentsMargins(0, 0, 0, 0);
    hb->addStretch();
    closeBtn_ = new QPushButton(tr("Close"), btnRow);
    applyBtn_ = new QPushButton(tr("Apply"), btnRow);
    hb->addWidget(closeBtn_);
    hb->addWidget(applyBtn_);
    root->addWidget(btnRow);

    // Visual priority: Apply is the primary (default) only when enabled.
    applyBtn_->setEnabled(false);
    applyBtn_->setAutoDefault(true);
    applyBtn_->setDefault(false);
    closeBtn_->setAutoDefault(false);
    closeBtn_->setDefault(false);
    connect(applyBtn_, &QPushButton::clicked, this, &SettingsDialog::onApply);
    connect(closeBtn_, &QPushButton::clicked, this, &SettingsDialog::reject);

    // Load from QSettings
    QSettings s("OpenSCP", "OpenSCP");
    const QString lang = s.value("UI/language", "en").toString();
    const int li = langCombo_->findData(lang);
    if (li >= 0)
        langCombo_->setCurrentIndex(li);
    if (showHidden_)
        showHidden_->setChecked(s.value("UI/showHidden", false).toBool());
    if (showConnOnStart_)
        showConnOnStart_->setChecked(
            s.value("UI/showConnOnStart", true).toBool());
    if (showConnOnDisconnect_)
        showConnOnDisconnect_->setChecked(
            s.value("UI/openSiteManagerOnDisconnect", true).toBool());
    clickMode_->setCurrentIndex(s.value("UI/singleClick", false).toBool() ? 1
                                                                          : 0);
    QString openBehaviorMode =
        s.value("UI/openBehaviorMode").toString().trimmed().toLower();
    if (openBehaviorMode.isEmpty()) {
        const bool revealLegacy =
            s.value("UI/openRevealInFolder", false).toBool();
        openBehaviorMode =
            revealLegacy ? QStringLiteral("reveal") : QStringLiteral("ask");
    }
    if (openBehaviorMode_) {
        int modeIdx = openBehaviorMode_->findData(openBehaviorMode);
        if (modeIdx < 0)
            modeIdx = openBehaviorMode_->findData(QStringLiteral("ask"));
        if (modeIdx >= 0)
            openBehaviorMode_->setCurrentIndex(modeIdx);
    }
    if (showQueueOnEnqueue_)
        showQueueOnEnqueue_->setChecked(
            s.value("UI/showQueueOnEnqueue", true).toBool());
    if (defaultDownloadDirEdit_)
        defaultDownloadDirEdit_->setText(
            s.value("UI/defaultDownloadDir", defaultDownloadDirPath())
                .toString());
    if (deleteSecretsOnRemove_)
        deleteSecretsOnRemove_->setChecked(
            s.value("Sites/deleteSecretsOnRemove", false).toBool());
    if (defaultProtocol_) {
        const auto defaultProtocol = openscp::protocolFromStorageName(
            s.value("Protocol/defaultProtocol",
                    QString::fromLatin1(openscp::protocolStorageName(
                        openscp::Protocol::Sftp)))
                .toString()
                .trimmed()
                .toLower()
                .toStdString());
        const int protocolIdx =
            defaultProtocol_->findData(static_cast<int>(defaultProtocol));
        if (protocolIdx >= 0)
            defaultProtocol_->setCurrentIndex(protocolIdx);
    }
    if (scpModeDefault_) {
        const auto scpMode = openscp::scpTransferModeFromStorageName(
            s.value("Protocol/scpTransferModeDefault",
                    QString::fromLatin1(openscp::scpTransferModeStorageName(
                        openscp::ScpTransferMode::Auto)))
                .toString()
                .trimmed()
                .toLower()
                .toStdString());
        const int scpModeIdx =
            scpModeDefault_->findData(static_cast<int>(scpMode));
        if (scpModeIdx >= 0)
            scpModeDefault_->setCurrentIndex(scpModeIdx);
    }
    if (defaultKnownHostsPolicy_) {
        int policyIdx = defaultKnownHostsPolicy_->findData(
            s.value("Security/defaultKnownHostsPolicy",
                    static_cast<int>(openscp::KnownHostsPolicy::Strict))
                .toInt());
        if (policyIdx < 0) {
            policyIdx = defaultKnownHostsPolicy_->findData(
                static_cast<int>(openscp::KnownHostsPolicy::Strict));
        }
        if (policyIdx >= 0)
            defaultKnownHostsPolicy_->setCurrentIndex(policyIdx);
    }
    if (defaultIntegrityPolicy_) {
        int integrityIdx = defaultIntegrityPolicy_->findData(
            s.value("Security/defaultTransferIntegrityPolicy",
                    static_cast<int>(openscp::TransferIntegrityPolicy::Optional))
                .toInt());
        if (integrityIdx < 0) {
            integrityIdx = defaultIntegrityPolicy_->findData(
                static_cast<int>(openscp::TransferIntegrityPolicy::Optional));
        }
        if (integrityIdx >= 0)
            defaultIntegrityPolicy_->setCurrentIndex(integrityIdx);
    }
    if (ftpsVerifyPeerDefault_) {
        ftpsVerifyPeerDefault_->setChecked(
            s.value("Security/ftpsVerifyPeerDefault", true).toBool());
    }
    if (ftpsCaCertPathDefaultEdit_) {
        ftpsCaCertPathDefaultEdit_->setText(
            s.value("Security/ftpsCaCertPathDefault", QString())
                .toString()
                .trimmed());
    }
    if (knownHostsHashed_)
        knownHostsHashed_->setChecked(
            s.value("Security/knownHostsHashed", true).toBool());
    if (fpHex_)
        fpHex_->setChecked(s.value("Security/fpHex", false).toBool());
    if (terminalForceInteractiveLogin_) {
        terminalForceInteractiveLogin_->setChecked(
            s.value("Terminal/forceInteractiveLogin", false).toBool());
    }
    if (terminalEnableSftpCliFallback_) {
        terminalEnableSftpCliFallback_->setChecked(
            s.value("Terminal/enableSftpCliFallback", true).toBool());
    }
    if (noHostVerifyTtlMinSpin_)
        noHostVerifyTtlMinSpin_->setValue(
            s.value("Security/noHostVerificationTtlMin", 15).toInt());
#if !defined(__APPLE__) && !defined(Q_OS_MAC) && !defined(Q_OS_MACOS) &&       \
    !defined(HAVE_LIBSECRET) && !defined(OPENSCP_BUILD_SECURE_ONLY)
    if (insecureFallback_)
        insecureFallback_->setChecked(
            s.value("Security/enableInsecureSecretFallback", false).toBool());
#endif
    if (maxConcurrentSpin_)
        maxConcurrentSpin_->setValue(
            s.value("Transfer/maxConcurrent", 2).toInt());
    if (globalSpeedDefaultSpin_)
        globalSpeedDefaultSpin_->setValue(
            s.value("Transfer/globalSpeedKBps", 0).toInt());
    if (queueAutoClearModeDefault_) {
        int queueMode = s.value("Transfer/defaultQueueAutoClearMode",
                                kQueueAutoClearOff)
                            .toInt();
        queueMode = qBound(kQueueAutoClearOff, queueMode,
                           kQueueAutoClearFinished);
        int modeIdx = queueAutoClearModeDefault_->findData(queueMode);
        if (modeIdx < 0)
            modeIdx = queueAutoClearModeDefault_->findData(kQueueAutoClearOff);
        if (modeIdx >= 0)
            queueAutoClearModeDefault_->setCurrentIndex(modeIdx);
    }
    if (queueAutoClearMinutesDefaultSpin_) {
        queueAutoClearMinutesDefaultSpin_->setValue(qBound(
            1, s.value("Transfer/defaultQueueAutoClearMinutes", 15).toInt(),
            1440));
    }
    if (sessionHealthIntervalSecSpin_) {
        sessionHealthIntervalSecSpin_->setValue(
            qBound(60, s.value("Network/sessionHealthIntervalSec", 600).toInt(),
                   86400));
    }
    if (remoteWriteabilityTtlMsSpin_) {
        remoteWriteabilityTtlMsSpin_->setValue(
            qBound(1000,
                   s.value("Network/remoteWriteabilityTtlMs", 15000).toInt(),
                   120000));
    }
    if (stagingRootEdit_)
        stagingRootEdit_->setText(
            s.value("Advanced/stagingRoot",
                    QDir::homePath() + "/Downloads/OpenSCP-Dragged")
                .toString());
    if (autoCleanStaging_)
        autoCleanStaging_->setChecked(
            s.value("Advanced/autoCleanStaging", true).toBool());
    if (stagingRetentionDaysSpin_)
        stagingRetentionDaysSpin_->setValue(
            qBound(1, s.value("Advanced/stagingRetentionDays", 7).toInt(),
                   365));
    if (stagingPrepTimeoutMsSpin_)
        stagingPrepTimeoutMsSpin_->setValue(
            s.value("Advanced/stagingPrepTimeoutMs", 2000).toInt());
    if (stagingConfirmItemsSpin_)
        stagingConfirmItemsSpin_->setValue(
            s.value("Advanced/stagingConfirmItems", 500).toInt());
    if (stagingConfirmMiBSpin_)
        stagingConfirmMiBSpin_->setValue(
            s.value("Advanced/stagingConfirmMiB", 1024).toInt());
    if (maxDepthSpin_)
        maxDepthSpin_->setValue(s.value("Advanced/maxFolderDepth", 32).toInt());
#if defined(Q_OS_MAC) || defined(Q_OS_MACOS) || defined(__APPLE__)
    if (macKeychainRestrictive_)
        macKeychainRestrictive_->setChecked(
            s.value("Security/macKeychainRestrictive", false).toBool());
#endif
    if (queueAutoClearModeDefault_ && queueAutoClearMinutesDefaultSpin_) {
        const bool queueMinutesEnabled =
            queueAutoClearModeDefault_->currentData().toInt() !=
            kQueueAutoClearOff;
        queueAutoClearMinutesDefaultSpin_->setEnabled(queueMinutesEnabled);
    }

    // Bind controls to dirty flag
    bindDirtyFlag(langCombo_, qOverload<int>(&QComboBox::currentIndexChanged));
    bindDirtyFlag(clickMode_, qOverload<int>(&QComboBox::currentIndexChanged));
    bindDirtyFlag(showHidden_, &QCheckBox::toggled);
    bindDirtyFlag(showConnOnStart_, &QCheckBox::toggled);
    bindDirtyFlag(showConnOnDisconnect_, &QCheckBox::toggled);
    bindDirtyFlag(openBehaviorMode_,
                  qOverload<int>(&QComboBox::currentIndexChanged));
    bindDirtyFlag(showQueueOnEnqueue_, &QCheckBox::toggled);
    bindDirtyFlag(defaultDownloadDirEdit_, &QLineEdit::textChanged);
    bindDirtyFlag(deleteSecretsOnRemove_, &QCheckBox::toggled);
    bindDirtyFlag(defaultProtocol_,
                  qOverload<int>(&QComboBox::currentIndexChanged));
    bindDirtyFlag(scpModeDefault_,
                  qOverload<int>(&QComboBox::currentIndexChanged));
    bindDirtyFlag(defaultKnownHostsPolicy_,
                  qOverload<int>(&QComboBox::currentIndexChanged));
    bindDirtyFlag(defaultIntegrityPolicy_,
                  qOverload<int>(&QComboBox::currentIndexChanged));
    bindDirtyFlag(ftpsVerifyPeerDefault_, &QCheckBox::toggled);
    bindDirtyFlag(ftpsCaCertPathDefaultEdit_, &QLineEdit::textChanged);
    bindDirtyFlag(knownHostsHashed_, &QCheckBox::toggled);
    bindDirtyFlag(fpHex_, &QCheckBox::toggled);
    bindDirtyFlag(terminalForceInteractiveLogin_, &QCheckBox::toggled);
    bindDirtyFlag(terminalEnableSftpCliFallback_, &QCheckBox::toggled);
    bindDirtyFlag(noHostVerifyTtlMinSpin_,
                  qOverload<int>(&QSpinBox::valueChanged));
    bindDirtyFlag(maxConcurrentSpin_, qOverload<int>(&QSpinBox::valueChanged));
    bindDirtyFlag(globalSpeedDefaultSpin_,
                  qOverload<int>(&QSpinBox::valueChanged));
    bindDirtyFlag(queueAutoClearModeDefault_,
                  qOverload<int>(&QComboBox::currentIndexChanged));
    bindDirtyFlag(queueAutoClearMinutesDefaultSpin_,
                  qOverload<int>(&QSpinBox::valueChanged));
    bindDirtyFlag(sessionHealthIntervalSecSpin_,
                  qOverload<int>(&QSpinBox::valueChanged));
    bindDirtyFlag(remoteWriteabilityTtlMsSpin_,
                  qOverload<int>(&QSpinBox::valueChanged));
    bindDirtyFlag(stagingRootEdit_, &QLineEdit::textChanged);
    bindDirtyFlag(autoCleanStaging_, &QCheckBox::toggled);
    bindDirtyFlag(stagingRetentionDaysSpin_,
                  qOverload<int>(&QSpinBox::valueChanged));
    bindDirtyFlag(stagingPrepTimeoutMsSpin_,
                  qOverload<int>(&QSpinBox::valueChanged));
    bindDirtyFlag(stagingConfirmItemsSpin_,
                  qOverload<int>(&QSpinBox::valueChanged));
    bindDirtyFlag(stagingConfirmMiBSpin_,
                  qOverload<int>(&QSpinBox::valueChanged));
    bindDirtyFlag(maxDepthSpin_, qOverload<int>(&QSpinBox::valueChanged));
#if defined(Q_OS_MAC) || defined(Q_OS_MACOS) || defined(__APPLE__)
    bindDirtyFlag(macKeychainRestrictive_, &QCheckBox::toggled);
#endif

    if (insecureFallback_) {
        connect(insecureFallback_, &QCheckBox::toggled, this, [this](bool on) {
            if (on) {
                bool prevAutoDefault = false;
                bool prevDefault = false;
                if (applyBtn_) {
                    prevAutoDefault = applyBtn_->autoDefault();
                    prevDefault = applyBtn_->isDefault();
                    applyBtn_->setAutoDefault(false);
                    applyBtn_->setDefault(false);
                }
                const auto ret = UiAlerts::warning(
                    this, tr("Enable insecure fallback"),
                    tr("This stores credentials unencrypted on disk using "
                       "QSettings.\n"
                       "On Linux, it is recommended to install and use "
                       "libsecret/Secret Service for better security.\n\n"
                       "Do you still want to enable insecure fallback?"),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (applyBtn_) {
                    applyBtn_->setAutoDefault(prevAutoDefault);
                    applyBtn_->setDefault(prevDefault);
                }
                if (ret != QMessageBox::Yes) {
                    insecureFallback_->blockSignals(true);
                    insecureFallback_->setChecked(false);
                    insecureFallback_->blockSignals(false);
                }
            }
            updateApplyFromControls();
        });
    }

    updateApplyFromControls();
    QTimer::singleShot(0, this, [this] { refreshWrappedCheckTexts(); });
}

void SettingsDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    refreshWrappedCheckTexts();
}

void SettingsDialog::onApply() {
    const QString chosenLang = langCombo_->currentData().toString();

    QSettings s("OpenSCP", "OpenSCP");
    const QString prevLang = s.value("UI/language", "en").toString();
    s.setValue("UI/language", chosenLang);
    s.setValue("UI/showHidden", showHidden_ && showHidden_->isChecked());
    s.setValue("UI/showConnOnStart",
               showConnOnStart_ && showConnOnStart_->isChecked());
    if (showConnOnDisconnect_)
        s.setValue("UI/openSiteManagerOnDisconnect",
                   showConnOnDisconnect_->isChecked());
    const bool singleClick =
        (clickMode_ && clickMode_->currentData().toInt() == 1);
    s.setValue("UI/singleClick", singleClick);
    const QString openMode = openBehaviorMode_
                                 ? openBehaviorMode_->currentData().toString()
                                 : QStringLiteral("ask");
    s.setValue("UI/openBehaviorMode", openMode);
    if (openMode == QStringLiteral("reveal")) {
        s.setValue("UI/openRevealInFolder", true);
        s.setValue("UI/openBehaviorChosen", true);
    } else if (openMode == QStringLiteral("open")) {
        s.setValue("UI/openRevealInFolder", false);
        s.setValue("UI/openBehaviorChosen", true);
    } else {
        s.setValue("UI/openBehaviorChosen", false);
    }
    if (showQueueOnEnqueue_)
        s.setValue("UI/showQueueOnEnqueue", showQueueOnEnqueue_->isChecked());
    if (defaultDownloadDirEdit_) {
        QString path = defaultDownloadDirEdit_->text().trimmed();
        if (path.isEmpty())
            path = defaultDownloadDirPath();
        s.setValue("UI/defaultDownloadDir", QDir::cleanPath(path));
    }
    if (deleteSecretsOnRemove_)
        s.setValue("Sites/deleteSecretsOnRemove",
                   deleteSecretsOnRemove_->isChecked());
    if (defaultProtocol_) {
        const auto protocol = static_cast<openscp::Protocol>(
            defaultProtocol_->currentData().toInt());
        s.setValue("Protocol/defaultProtocol",
                   QString::fromLatin1(openscp::protocolStorageName(protocol)));
    }
    if (scpModeDefault_) {
        const auto mode = static_cast<openscp::ScpTransferMode>(
            scpModeDefault_->currentData().toInt());
        s.setValue("Protocol/scpTransferModeDefault",
                   QString::fromLatin1(
                       openscp::scpTransferModeStorageName(mode)));
    }
    if (defaultKnownHostsPolicy_) {
        s.setValue("Security/defaultKnownHostsPolicy",
                   defaultKnownHostsPolicy_->currentData().toInt());
    }
    if (defaultIntegrityPolicy_) {
        s.setValue("Security/defaultTransferIntegrityPolicy",
                   defaultIntegrityPolicy_->currentData().toInt());
    }
    if (ftpsVerifyPeerDefault_) {
        s.setValue("Security/ftpsVerifyPeerDefault",
                   ftpsVerifyPeerDefault_->isChecked());
    }
    if (ftpsCaCertPathDefaultEdit_) {
        s.setValue("Security/ftpsCaCertPathDefault",
                   ftpsCaCertPathDefaultEdit_->text().trimmed());
    }
    if (knownHostsHashed_)
        s.setValue("Security/knownHostsHashed", knownHostsHashed_->isChecked());
    if (fpHex_)
        s.setValue("Security/fpHex", fpHex_->isChecked());
    if (terminalForceInteractiveLogin_) {
        s.setValue("Terminal/forceInteractiveLogin",
                   terminalForceInteractiveLogin_->isChecked());
    }
    if (terminalEnableSftpCliFallback_) {
        s.setValue("Terminal/enableSftpCliFallback",
                   terminalEnableSftpCliFallback_->isChecked());
    }
    if (noHostVerifyTtlMinSpin_)
        s.setValue("Security/noHostVerificationTtlMin",
                   noHostVerifyTtlMinSpin_->value());
#if defined(Q_OS_MAC) || defined(Q_OS_MACOS) || defined(__APPLE__)
    if (macKeychainRestrictive_)
        s.setValue("Security/macKeychainRestrictive",
                   macKeychainRestrictive_->isChecked());
#endif
    if (insecureFallback_)
        s.setValue("Security/enableInsecureSecretFallback",
                   insecureFallback_->isChecked());
    if (maxConcurrentSpin_)
        s.setValue("Transfer/maxConcurrent", maxConcurrentSpin_->value());
    if (globalSpeedDefaultSpin_)
        s.setValue("Transfer/globalSpeedKBps",
                   globalSpeedDefaultSpin_->value());
    if (queueAutoClearModeDefault_) {
        s.setValue("Transfer/defaultQueueAutoClearMode",
                   queueAutoClearModeDefault_->currentData().toInt());
    }
    if (queueAutoClearMinutesDefaultSpin_) {
        s.setValue("Transfer/defaultQueueAutoClearMinutes",
                   queueAutoClearMinutesDefaultSpin_->value());
    }
    if (sessionHealthIntervalSecSpin_) {
        s.setValue("Network/sessionHealthIntervalSec",
                   sessionHealthIntervalSecSpin_->value());
    }
    if (remoteWriteabilityTtlMsSpin_) {
        s.setValue("Network/remoteWriteabilityTtlMs",
                   remoteWriteabilityTtlMsSpin_->value());
    }
    if (stagingRootEdit_)
        s.setValue("Advanced/stagingRoot", stagingRootEdit_->text());
    if (autoCleanStaging_)
        s.setValue("Advanced/autoCleanStaging", autoCleanStaging_->isChecked());
    if (stagingRetentionDaysSpin_) {
        s.setValue("Advanced/stagingRetentionDays",
                   stagingRetentionDaysSpin_->value());
    }
    if (stagingPrepTimeoutMsSpin_)
        s.setValue("Advanced/stagingPrepTimeoutMs",
                   stagingPrepTimeoutMsSpin_->value());
    if (stagingConfirmItemsSpin_)
        s.setValue("Advanced/stagingConfirmItems",
                   stagingConfirmItemsSpin_->value());
    if (stagingConfirmMiBSpin_)
        s.setValue("Advanced/stagingConfirmMiB",
                   stagingConfirmMiBSpin_->value());
    if (maxDepthSpin_)
        s.setValue("Advanced/maxFolderDepth", maxDepthSpin_->value());
    s.sync();

    // Only notify if language actually changed
    if (prevLang != chosenLang) {
        UiAlerts::information(
            this, tr("Language"),
            tr("Language changes take effect after restart."));
    }
    if (applyBtn_) {
        applyBtn_->setEnabled(false);
        applyBtn_->setDefault(false);
    }
}

void SettingsDialog::updateApplyFromControls() {
    QSettings s("OpenSCP", "OpenSCP");
    const QString prevLang = s.value("UI/language", "en").toString();
    const bool showHidden = s.value("UI/showHidden", false).toBool();
    const bool showConnOnStart = s.value("UI/showConnOnStart", true).toBool();
    const bool onDisc =
        s.value("UI/openSiteManagerOnDisconnect", true).toBool();
    const bool singleClick = s.value("UI/singleClick", false).toBool();
    QString openMode =
        s.value("UI/openBehaviorMode").toString().trimmed().toLower();
    if (openMode.isEmpty()) {
        const bool revealLegacy =
            s.value("UI/openRevealInFolder", false).toBool();
        openMode =
            revealLegacy ? QStringLiteral("reveal") : QStringLiteral("ask");
    }
    const bool showQueue = s.value("UI/showQueueOnEnqueue", true).toBool();
    QString defaultDownload =
        s.value("UI/defaultDownloadDir", defaultDownloadDirPath())
            .toString()
            .trimmed();
    if (defaultDownload.isEmpty())
        defaultDownload = defaultDownloadDirPath();
    defaultDownload = QDir::cleanPath(defaultDownload);
    const bool deleteSecrets =
        s.value("Sites/deleteSecretsOnRemove", false).toBool();
    const auto defaultProtocol = openscp::protocolFromStorageName(
        s.value("Protocol/defaultProtocol",
                QString::fromLatin1(
                    openscp::protocolStorageName(openscp::Protocol::Sftp)))
            .toString()
            .trimmed()
            .toLower()
            .toStdString());
    const auto scpModeDefault = openscp::scpTransferModeFromStorageName(
        s.value("Protocol/scpTransferModeDefault",
                QString::fromLatin1(openscp::scpTransferModeStorageName(
                    openscp::ScpTransferMode::Auto)))
            .toString()
            .trimmed()
            .toLower()
            .toStdString());
    const int defaultKnownHostsPolicy =
        s.value("Security/defaultKnownHostsPolicy",
                static_cast<int>(openscp::KnownHostsPolicy::Strict))
            .toInt();
    const int defaultIntegrityPolicy =
        s.value("Security/defaultTransferIntegrityPolicy",
                static_cast<int>(openscp::TransferIntegrityPolicy::Optional))
            .toInt();
    const bool ftpsVerifyPeerDefault =
        s.value("Security/ftpsVerifyPeerDefault", true).toBool();
    const QString ftpsCaCertPathDefault =
        s.value("Security/ftpsCaCertPathDefault", QString())
            .toString()
            .trimmed();
    const bool knownHashed =
        s.value("Security/knownHostsHashed", true).toBool();
    const bool fpHex = s.value("Security/fpHex", false).toBool();
    const bool terminalForceInteractiveLogin =
        s.value("Terminal/forceInteractiveLogin", false).toBool();
    const bool terminalEnableSftpCliFallback =
        s.value("Terminal/enableSftpCliFallback", true).toBool();
    const int noHostVerifyTtlMin =
        s.value("Security/noHostVerificationTtlMin", 15).toInt();
// Only compare insecure fallback when available in this build/platform
#if !defined(__APPLE__) && !defined(Q_OS_MAC) && !defined(Q_OS_MACOS) &&       \
    !defined(HAVE_LIBSECRET) && !defined(OPENSCP_BUILD_SECURE_ONLY)
    const bool insecureFb =
        s.value("Security/enableInsecureSecretFallback", false).toBool();
#endif
    const int maxConcurrent = s.value("Transfer/maxConcurrent", 2).toInt();
    const int globalSpeedDefault =
        s.value("Transfer/globalSpeedKBps", 0).toInt();
    const int queueAutoClearModeDefault =
        qBound(kQueueAutoClearOff,
               s.value("Transfer/defaultQueueAutoClearMode", kQueueAutoClearOff)
                   .toInt(),
               kQueueAutoClearFinished);
    const int queueAutoClearMinutesDefault = qBound(
        1, s.value("Transfer/defaultQueueAutoClearMinutes", 15).toInt(), 1440);
    const int sessionHealthIntervalSec = qBound(
        60, s.value("Network/sessionHealthIntervalSec", 600).toInt(), 86400);
    const int remoteWriteabilityTtlMs = qBound(
        1000, s.value("Network/remoteWriteabilityTtlMs", 15000).toInt(),
        120000);
    const QString stagingRoot =
        s.value("Advanced/stagingRoot",
                QDir::homePath() + "/Downloads/OpenSCP-Dragged")
            .toString();
    const bool autoCleanSt =
        s.value("Advanced/autoCleanStaging", true).toBool();
    const int stagingRetentionDays = qBound(
        1, s.value("Advanced/stagingRetentionDays", 7).toInt(), 365);
    const int stagingPrepTimeoutMs =
        s.value("Advanced/stagingPrepTimeoutMs", 2000).toInt();
    const int stagingConfirmItems =
        s.value("Advanced/stagingConfirmItems", 500).toInt();
    const int stagingConfirmMiB =
        s.value("Advanced/stagingConfirmMiB", 1024).toInt();
    const int maxDepthPrev = s.value("Advanced/maxFolderDepth", 32).toInt();

    const QString curLang =
        langCombo_ ? langCombo_->currentData().toString() : prevLang;
    const bool curShowHidden = showHidden_ && showHidden_->isChecked();
    const bool curShowConn = showConnOnStart_ && showConnOnStart_->isChecked();
    const bool curShowConnDisc =
        showConnOnDisconnect_ && showConnOnDisconnect_->isChecked();
    const bool curSingleClick =
        (clickMode_ && clickMode_->currentData().toInt() == 1);
    const QString curOpenMode =
        openBehaviorMode_ ? openBehaviorMode_->currentData().toString()
                          : openMode;
    const bool curShowQueue =
        showQueueOnEnqueue_ && showQueueOnEnqueue_->isChecked();
    QString curDefaultDownload = defaultDownloadDirEdit_
                                     ? defaultDownloadDirEdit_->text().trimmed()
                                     : defaultDownload;
    if (curDefaultDownload.isEmpty())
        curDefaultDownload = defaultDownloadDirPath();
    curDefaultDownload = QDir::cleanPath(curDefaultDownload);
    const bool curDeleteSecrets =
        deleteSecretsOnRemove_ && deleteSecretsOnRemove_->isChecked();
    const auto curDefaultProtocol =
        defaultProtocol_
            ? static_cast<openscp::Protocol>(
                  defaultProtocol_->currentData().toInt())
            : defaultProtocol;
    const auto curScpModeDefault =
        scpModeDefault_
            ? static_cast<openscp::ScpTransferMode>(
                  scpModeDefault_->currentData().toInt())
            : scpModeDefault;
    const int curDefaultKnownHostsPolicy =
        defaultKnownHostsPolicy_
            ? defaultKnownHostsPolicy_->currentData().toInt()
            : defaultKnownHostsPolicy;
    const int curDefaultIntegrityPolicy =
        defaultIntegrityPolicy_ ? defaultIntegrityPolicy_->currentData().toInt()
                                : defaultIntegrityPolicy;
    const bool curFtpsVerifyPeerDefault =
        ftpsVerifyPeerDefault_ ? ftpsVerifyPeerDefault_->isChecked()
                               : ftpsVerifyPeerDefault;
    const QString curFtpsCaCertPathDefault =
        ftpsCaCertPathDefaultEdit_ ? ftpsCaCertPathDefaultEdit_->text().trimmed()
                                   : ftpsCaCertPathDefault;
    const bool curKnownHashed =
        knownHostsHashed_ && knownHostsHashed_->isChecked();
    const bool curFpHex = fpHex_ && fpHex_->isChecked();
    const bool curTerminalForceInteractiveLogin =
        terminalForceInteractiveLogin_ &&
        terminalForceInteractiveLogin_->isChecked();
    const bool curTerminalEnableSftpCliFallback =
        terminalEnableSftpCliFallback_ &&
        terminalEnableSftpCliFallback_->isChecked();
    const int curNoHostVerifyTtlMin = noHostVerifyTtlMinSpin_
                                          ? noHostVerifyTtlMinSpin_->value()
                                          : noHostVerifyTtlMin;
// Current value only when applicable
#if !defined(__APPLE__) && !defined(Q_OS_MAC) && !defined(Q_OS_MACOS) &&       \
    !defined(HAVE_LIBSECRET) && !defined(OPENSCP_BUILD_SECURE_ONLY)
    const bool curInsecureFb =
        insecureFallback_ && insecureFallback_->isChecked();
#endif
    const int curMaxConcurrent =
        maxConcurrentSpin_ ? maxConcurrentSpin_->value() : maxConcurrent;
    const int curGlobalSpeedDefault = globalSpeedDefaultSpin_
                                          ? globalSpeedDefaultSpin_->value()
                                          : globalSpeedDefault;
    const int curQueueAutoClearModeDefault =
        queueAutoClearModeDefault_
            ? queueAutoClearModeDefault_->currentData().toInt()
            : queueAutoClearModeDefault;
    const int curQueueAutoClearMinutesDefault =
        queueAutoClearMinutesDefaultSpin_
            ? queueAutoClearMinutesDefaultSpin_->value()
            : queueAutoClearMinutesDefault;
    const int curSessionHealthIntervalSec = sessionHealthIntervalSecSpin_
                                                ? sessionHealthIntervalSecSpin_
                                                      ->value()
                                                : sessionHealthIntervalSec;
    const int curRemoteWriteabilityTtlMs = remoteWriteabilityTtlMsSpin_
                                               ? remoteWriteabilityTtlMsSpin_
                                                     ->value()
                                               : remoteWriteabilityTtlMs;
    const QString curStagingRoot =
        stagingRootEdit_ ? stagingRootEdit_->text() : stagingRoot;
    const bool curAutoCleanSt =
        autoCleanStaging_ && autoCleanStaging_->isChecked();
    const int curStagingRetentionDays =
        stagingRetentionDaysSpin_ ? stagingRetentionDaysSpin_->value()
                                  : stagingRetentionDays;
    const int curStagingPrepTimeoutMs = stagingPrepTimeoutMsSpin_
                                            ? stagingPrepTimeoutMsSpin_->value()
                                            : stagingPrepTimeoutMs;
    const int curStagingConfirmItems = stagingConfirmItemsSpin_
                                           ? stagingConfirmItemsSpin_->value()
                                           : stagingConfirmItems;
    const int curStagingConfirmMiB = stagingConfirmMiBSpin_
                                         ? stagingConfirmMiBSpin_->value()
                                         : stagingConfirmMiB;
    const int curMaxDepth =
        maxDepthSpin_ ? maxDepthSpin_->value() : maxDepthPrev;

    const bool modified =
        (curLang != prevLang) || (curShowHidden != showHidden) ||
        (curShowConn != showConnOnStart) || (curShowConnDisc != onDisc) ||
        (curSingleClick != singleClick) || (curOpenMode != openMode) ||
        (curShowQueue != showQueue) ||
        (curDefaultDownload != defaultDownload) ||
        (curDeleteSecrets != deleteSecrets) ||
        (curDefaultProtocol != defaultProtocol) ||
        (curScpModeDefault != scpModeDefault) ||
        (curDefaultKnownHostsPolicy != defaultKnownHostsPolicy) ||
        (curDefaultIntegrityPolicy != defaultIntegrityPolicy) ||
        (curFtpsVerifyPeerDefault != ftpsVerifyPeerDefault) ||
        (curFtpsCaCertPathDefault != ftpsCaCertPathDefault) ||
        (curKnownHashed != knownHashed) || (curFpHex != fpHex) ||
        (curTerminalForceInteractiveLogin != terminalForceInteractiveLogin) ||
        (curTerminalEnableSftpCliFallback != terminalEnableSftpCliFallback) ||
        (curNoHostVerifyTtlMin != noHostVerifyTtlMin)
#if !defined(__APPLE__) && !defined(Q_OS_MAC) && !defined(Q_OS_MACOS) &&       \
    !defined(HAVE_LIBSECRET) && !defined(OPENSCP_BUILD_SECURE_ONLY)
        || (curInsecureFb != insecureFb)
#endif
        || (curMaxConcurrent != maxConcurrent) ||
        (curGlobalSpeedDefault != globalSpeedDefault) ||
        (curQueueAutoClearModeDefault != queueAutoClearModeDefault) ||
        (curQueueAutoClearMinutesDefault != queueAutoClearMinutesDefault) ||
        (curSessionHealthIntervalSec != sessionHealthIntervalSec) ||
        (curRemoteWriteabilityTtlMs != remoteWriteabilityTtlMs) ||
        (curStagingRoot != stagingRoot) || (curAutoCleanSt != autoCleanSt) ||
        (curStagingRetentionDays != stagingRetentionDays) ||
        (curStagingPrepTimeoutMs != stagingPrepTimeoutMs) ||
        (curStagingConfirmItems != stagingConfirmItems) ||
        (curStagingConfirmMiB != stagingConfirmMiB) ||
        (curMaxDepth != maxDepthPrev);
    if (applyBtn_) {
        applyBtn_->setEnabled(modified);
        applyBtn_->setDefault(modified);
    }
}
