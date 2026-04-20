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
#include <QKeySequence>
#include <QKeySequenceEdit>
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
constexpr const char *kShortcutTransfersKey = "Shortcuts/openTransfers";
constexpr const char *kShortcutHistoryKey = "Shortcuts/openHistory";

QVector<SettingsDialog::SettingBinding>
SettingsDialog::buildSettingBindings() const {
    QVector<SettingBinding> bindings;
    bindings.reserve(40);

    using StringNormalizer = std::function<QString(const QString &)>;
    using IntNormalizer = std::function<int(int)>;

    auto makeBoolCheckBinding = [](const QString &key, bool defaultValue,
                                   QCheckBox *check) {
        return SettingBinding{
            key,
            [key, defaultValue](const QSettings &settings) {
                return settings.value(key, defaultValue).toBool();
            },
            [check, defaultValue] {
                return check ? QVariant(check->isChecked())
                             : QVariant(defaultValue);
            },
            [check](const QVariant &value) {
                if (check)
                    check->setChecked(value.toBool());
            },
            [key](QSettings &settings, const QVariant &value) {
                settings.setValue(key, value.toBool());
            },
        };
    };

    auto makeSpinBinding = [](const QString &key, int defaultValue,
                              QSpinBox *spin, int minValue = 1,
                              int maxValue = 0) {
        return SettingBinding{
            key,
            [key, defaultValue, minValue, maxValue](const QSettings &settings) {
                int value = settings.value(key, defaultValue).toInt();
                if (minValue <= maxValue)
                    value = qBound(minValue, value, maxValue);
                return value;
            },
            [spin, defaultValue] {
                return spin ? QVariant(spin->value()) : QVariant(defaultValue);
            },
            [spin](const QVariant &value) {
                if (spin)
                    spin->setValue(value.toInt());
            },
            [key](QSettings &settings, const QVariant &value) {
                settings.setValue(key, value.toInt());
            },
        };
    };

    auto makeLineEditBinding = [](const QString &key, const QString &defaultValue,
                                  QLineEdit *edit,
                                  const StringNormalizer &normalize = {}) {
        return SettingBinding{
            key,
            [key, defaultValue, normalize](const QSettings &settings) {
                QString value =
                    settings.value(key, defaultValue).toString();
                return normalize ? normalize(value) : value;
            },
            [edit, defaultValue, normalize] {
                QString value = edit ? edit->text() : defaultValue;
                return normalize ? normalize(value) : value;
            },
            [edit](const QVariant &value) {
                if (edit)
                    edit->setText(value.toString());
            },
            [key](QSettings &settings, const QVariant &value) {
                settings.setValue(key, value);
            },
        };
    };

    auto makeShortcutBinding = [](const QString &key,
                                  const QString &defaultValue,
                                  QKeySequenceEdit *editor) {
        return SettingBinding{
            key,
            [key, defaultValue](const QSettings &settings) {
                return settings.value(key, defaultValue).toString().trimmed();
            },
            [editor, defaultValue] {
                return editor
                           ? editor->keySequence()
                                 .toString(QKeySequence::PortableText)
                                 .trimmed()
                           : defaultValue;
            },
            [editor](const QVariant &value) {
                if (editor) {
                    editor->setKeySequence(QKeySequence::fromString(
                        value.toString(), QKeySequence::PortableText));
                }
            },
            [key](QSettings &settings, const QVariant &value) {
                settings.setValue(key, value.toString());
            },
        };
    };

    auto makeIntComboBinding = [](const QString &key, int defaultValue,
                                  QComboBox *combo,
                                  const IntNormalizer &normalize = {}) {
        return SettingBinding{
            key,
            [key, defaultValue, combo, normalize](const QSettings &settings) {
                int value = settings.value(key, defaultValue).toInt();
                if (normalize)
                    value = normalize(value);
                if (combo && combo->findData(value) < 0)
                    value = defaultValue;
                return value;
            },
            [combo, defaultValue] {
                return combo ? QVariant(combo->currentData().toInt())
                             : QVariant(defaultValue);
            },
            [combo, defaultValue](const QVariant &value) {
                if (!combo)
                    return;
                int index = combo->findData(value.toInt());
                if (index < 0)
                    index = combo->findData(defaultValue);
                if (index >= 0)
                    combo->setCurrentIndex(index);
            },
            [key](QSettings &settings, const QVariant &value) {
                settings.setValue(key, value.toInt());
            },
        };
    };

    auto makeStringComboBinding = [](const QString &key,
                                     const QString &defaultValue,
                                     QComboBox *combo,
                                     const StringNormalizer &normalize = {}) {
        return SettingBinding{
            key,
            [key, defaultValue, combo, normalize](const QSettings &settings) {
                QString value = settings.value(key, defaultValue).toString();
                if (normalize)
                    value = normalize(value);
                if (combo && combo->findData(value) < 0)
                    value = defaultValue;
                return value;
            },
            [combo, defaultValue] {
                return combo ? QVariant(combo->currentData().toString())
                             : QVariant(defaultValue);
            },
            [combo, defaultValue](const QVariant &value) {
                if (!combo)
                    return;
                int index = combo->findData(value.toString());
                if (index < 0)
                    index = combo->findData(defaultValue);
                if (index >= 0)
                    combo->setCurrentIndex(index);
            },
            [key](QSettings &settings, const QVariant &value) {
                settings.setValue(key, value.toString());
            },
        };
    };

    auto addBool = [&](const char *key, bool defaultValue, QCheckBox *check) {
        bindings.push_back(makeBoolCheckBinding(QString::fromLatin1(key),
                                                defaultValue, check));
    };
    auto addSpin = [&](const char *key, int defaultValue, QSpinBox *spin,
                       int minValue = 1, int maxValue = 0) {
        bindings.push_back(makeSpinBinding(QString::fromLatin1(key), defaultValue,
                                           spin, minValue, maxValue));
    };
    auto addLineEdit = [&](const char *key, const QString &defaultValue,
                           QLineEdit *edit,
                           const StringNormalizer &normalize = {}) {
        bindings.push_back(makeLineEditBinding(QString::fromLatin1(key),
                                               defaultValue, edit, normalize));
    };
    auto addShortcut = [&](const char *key, const QString &defaultValue,
                           QKeySequenceEdit *editor) {
        bindings.push_back(makeShortcutBinding(QString::fromLatin1(key),
                                               defaultValue, editor));
    };
    auto addIntCombo = [&](const char *key, int defaultValue, QComboBox *combo,
                           const IntNormalizer &normalize = {}) {
        bindings.push_back(makeIntComboBinding(QString::fromLatin1(key),
                                               defaultValue, combo, normalize));
    };
    auto addStringCombo = [&](const char *key, const QString &defaultValue,
                              QComboBox *combo,
                              const StringNormalizer &normalize = {}) {
        bindings.push_back(makeStringComboBinding(QString::fromLatin1(key),
                                                  defaultValue, combo,
                                                  normalize));
    };

    struct BoolBindingSpec {
        const char *key;
        bool defaultValue;
        QCheckBox *check;
    };
    const BoolBindingSpec generalBoolBindings[] = {
        {"UI/showHidden", false, showHidden_},
        {"UI/showConnOnStart", true, showConnOnStart_},
        {"UI/openSiteManagerOnDisconnect", true, showConnOnDisconnect_},
    };

    addStringCombo("UI/language", QStringLiteral("en"), langCombo_);
    for (const BoolBindingSpec &spec : generalBoolBindings)
        addBool(spec.key, spec.defaultValue, spec.check);

    addBool("UI/singleClick", false, nullptr);
    bindings.back().readCurrent = [this] {
        return clickMode_ && clickMode_->currentData().toInt() == 1;
    };
    bindings.back().applyToControl = [this](const QVariant &value) {
        if (clickMode_)
            clickMode_->setCurrentIndex(value.toBool() ? 1 : 0);
    };

    bindings.push_back({
        QStringLiteral("UI/openBehaviorMode"),
        [](const QSettings &settings) {
            QString mode = settings
                               .value("UI/openBehaviorMode")
                               .toString()
                               .trimmed()
                               .toLower();
            if (mode.isEmpty()) {
                const bool revealLegacy =
                    settings.value("UI/openRevealInFolder", false).toBool();
                mode = revealLegacy ? QStringLiteral("reveal")
                                    : QStringLiteral("ask");
            }
            return mode;
        },
        [this] {
            return openBehaviorMode_
                       ? openBehaviorMode_->currentData().toString()
                       : QStringLiteral("ask");
        },
        [this](const QVariant &value) {
            if (!openBehaviorMode_)
                return;
            int modeIdx = openBehaviorMode_->findData(value.toString());
            if (modeIdx < 0)
                modeIdx = openBehaviorMode_->findData(QStringLiteral("ask"));
            if (modeIdx >= 0)
                openBehaviorMode_->setCurrentIndex(modeIdx);
        },
        [](QSettings &settings, const QVariant &value) {
            const QString openMode = value.toString();
            settings.setValue("UI/openBehaviorMode", openMode);
            if (openMode == QStringLiteral("reveal")) {
                settings.setValue("UI/openRevealInFolder", true);
                settings.setValue("UI/openBehaviorChosen", true);
            } else if (openMode == QStringLiteral("open")) {
                settings.setValue("UI/openRevealInFolder", false);
                settings.setValue("UI/openBehaviorChosen", true);
            } else {
                settings.setValue("UI/openBehaviorChosen", false);
            }
        },
    });

    addBool("UI/showQueueOnEnqueue", true, showQueueOnEnqueue_);
    addShortcut(kShortcutTransfersKey, QStringLiteral("F12"),
                queueShortcutEdit_);
    addShortcut(kShortcutHistoryKey, QStringLiteral("Ctrl+Shift+H"),
                historyShortcutEdit_);
    addLineEdit("UI/defaultDownloadDir", defaultDownloadDirPath(),
                defaultDownloadDirEdit_, [](const QString &raw) {
            QString path = raw.trimmed();
            if (path.isEmpty())
                path = defaultDownloadDirPath();
            return QDir::cleanPath(path);
        });
    addBool("Sites/deleteSecretsOnRemove", false, deleteSecretsOnRemove_);

    bindings.push_back({
        QStringLiteral("Protocol/defaultProtocol"),
        [](const QSettings &settings) {
            const auto protocol = openscp::protocolFromStorageName(
                settings
                    .value("Protocol/defaultProtocol",
                           QString::fromLatin1(openscp::protocolStorageName(
                               openscp::Protocol::Sftp)))
                    .toString()
                    .trimmed()
                    .toLower()
                    .toStdString());
            return static_cast<int>(protocol);
        },
        [this] {
            return defaultProtocol_
                       ? defaultProtocol_->currentData().toInt()
                       : static_cast<int>(openscp::Protocol::Sftp);
        },
        [this](const QVariant &value) {
            if (!defaultProtocol_)
                return;
            const int index = defaultProtocol_->findData(value.toInt());
            if (index >= 0)
                defaultProtocol_->setCurrentIndex(index);
        },
        [](QSettings &settings, const QVariant &value) {
            const auto protocol =
                static_cast<openscp::Protocol>(value.toInt());
            settings.setValue("Protocol/defaultProtocol",
                              QString::fromLatin1(
                                  openscp::protocolStorageName(protocol)));
        },
    });
    bindings.push_back({
        QStringLiteral("Protocol/scpTransferModeDefault"),
        [](const QSettings &settings) {
            const auto mode = openscp::scpTransferModeFromStorageName(
                settings
                    .value("Protocol/scpTransferModeDefault",
                           QString::fromLatin1(
                               openscp::scpTransferModeStorageName(
                                   openscp::ScpTransferMode::Auto)))
                    .toString()
                    .trimmed()
                    .toLower()
                    .toStdString());
            return static_cast<int>(mode);
        },
        [this] {
            return scpModeDefault_
                       ? scpModeDefault_->currentData().toInt()
                       : static_cast<int>(openscp::ScpTransferMode::Auto);
        },
        [this](const QVariant &value) {
            if (!scpModeDefault_)
                return;
            const int index = scpModeDefault_->findData(value.toInt());
            if (index >= 0)
                scpModeDefault_->setCurrentIndex(index);
        },
        [](QSettings &settings, const QVariant &value) {
            const auto mode =
                static_cast<openscp::ScpTransferMode>(value.toInt());
            settings.setValue(
                "Protocol/scpTransferModeDefault",
                QString::fromLatin1(openscp::scpTransferModeStorageName(mode)));
        },
    });

    const int strictKhPolicy =
        static_cast<int>(openscp::KnownHostsPolicy::Strict);
    const int optionalIntegrity =
        static_cast<int>(openscp::TransferIntegrityPolicy::Optional);
    addIntCombo("Security/defaultKnownHostsPolicy", strictKhPolicy,
                defaultKnownHostsPolicy_, [strictKhPolicy](int value) {
            const int acceptNew =
                static_cast<int>(openscp::KnownHostsPolicy::AcceptNew);
            const int off =
                static_cast<int>(openscp::KnownHostsPolicy::Off);
            return (value == strictKhPolicy || value == acceptNew ||
                    value == off)
                       ? value
                       : strictKhPolicy;
        });
    addIntCombo("Security/defaultTransferIntegrityPolicy", optionalIntegrity,
                defaultIntegrityPolicy_, [optionalIntegrity](int value) {
            const int required =
                static_cast<int>(openscp::TransferIntegrityPolicy::Required);
            const int off =
                static_cast<int>(openscp::TransferIntegrityPolicy::Off);
            return (value == optionalIntegrity || value == required ||
                    value == off)
                       ? value
                       : optionalIntegrity;
        });

    const BoolBindingSpec securityBoolBindings[] = {
        {"Security/ftpsVerifyPeerDefault", true, ftpsVerifyPeerDefault_},
        {"Security/knownHostsHashed", true, knownHostsHashed_},
        {"Security/fpHex", false, fpHex_},
        {"Terminal/forceInteractiveLogin", false,
         terminalForceInteractiveLogin_},
        {"Terminal/enableSftpCliFallback", true,
         terminalEnableSftpCliFallback_},
    };
    for (const BoolBindingSpec &spec : securityBoolBindings)
        addBool(spec.key, spec.defaultValue, spec.check);

    addLineEdit("Security/ftpsCaCertPathDefault", QString(),
                ftpsCaCertPathDefaultEdit_,
                [](const QString &raw) { return raw.trimmed(); });
    addSpin("Security/noHostVerificationTtlMin", 15, noHostVerifyTtlMinSpin_);

#if defined(Q_OS_MAC) || defined(Q_OS_MACOS) || defined(__APPLE__)
    addBool("Security/macKeychainRestrictive", false, macKeychainRestrictive_);
#endif

#if !defined(__APPLE__) && !defined(Q_OS_MAC) && !defined(Q_OS_MACOS) &&       \
    !defined(HAVE_LIBSECRET) && !defined(OPENSCP_BUILD_SECURE_ONLY)
    addBool("Security/enableInsecureSecretFallback", false, insecureFallback_);
#endif

    struct SpinBindingSpec {
        const char *key;
        int defaultValue;
        QSpinBox *spin;
    };
    const SpinBindingSpec plainSpinBindings[] = {
        {"Transfer/maxConcurrent", 2, maxConcurrentSpin_},
        {"Transfer/globalSpeedKBps", 0, globalSpeedDefaultSpin_},
        {"Advanced/stagingPrepTimeoutMs", 2000, stagingPrepTimeoutMsSpin_},
        {"Advanced/stagingConfirmItems", 500, stagingConfirmItemsSpin_},
        {"Advanced/stagingConfirmMiB", 1024, stagingConfirmMiBSpin_},
        {"Advanced/maxFolderDepth", 32, maxDepthSpin_},
    };
    for (const SpinBindingSpec &spec : plainSpinBindings)
        addSpin(spec.key, spec.defaultValue, spec.spin);

    addIntCombo("Transfer/defaultQueueAutoClearMode", kQueueAutoClearOff,
                queueAutoClearModeDefault_, [](int value) {
                    return qBound(kQueueAutoClearOff, value,
                                  kQueueAutoClearFinished);
                });

    struct BoundedSpinBindingSpec {
        const char *key;
        int defaultValue;
        QSpinBox *spin;
        int minValue;
        int maxValue;
    };
    const BoundedSpinBindingSpec boundedSpinBindings[] = {
        {"Transfer/defaultQueueAutoClearMinutes", 15,
         queueAutoClearMinutesDefaultSpin_, 1, 1440},
        {"Network/sessionHealthIntervalSec", 600, sessionHealthIntervalSecSpin_,
         60, 86400},
        {"Network/remoteWriteabilityTtlMs", 15000, remoteWriteabilityTtlMsSpin_,
         1000, 120000},
        {"Advanced/stagingRetentionDays", 7, stagingRetentionDaysSpin_, 1, 365},
    };
    for (const BoundedSpinBindingSpec &spec : boundedSpinBindings) {
        addSpin(spec.key, spec.defaultValue, spec.spin, spec.minValue,
                spec.maxValue);
    }

    addLineEdit("Advanced/stagingRoot",
                QDir::homePath() + "/Downloads/OpenSCP-Dragged",
                stagingRootEdit_);
    addBool("Advanced/autoCleanStaging", true, autoCleanStaging_);

    return bindings;
}

QVariantMap SettingsDialog::readPersistedSnapshot(
    const QSettings &settings, const QVector<SettingBinding> &bindings) const {
    QVariantMap snapshot;
    for (const auto &binding : bindings) {
        if (!binding.readPersisted)
            continue;
        snapshot.insert(binding.id, binding.readPersisted(settings));
    }
    return snapshot;
}

QVariantMap
SettingsDialog::readCurrentSnapshot(const QVector<SettingBinding> &bindings) const {
    QVariantMap snapshot;
    for (const auto &binding : bindings) {
        if (!binding.readCurrent)
            continue;
        snapshot.insert(binding.id, binding.readCurrent());
    }
    return snapshot;
}

void SettingsDialog::applySnapshotToControls(
    const QVariantMap &snapshot, const QVector<SettingBinding> &bindings) {
    for (const auto &binding : bindings) {
        if (!binding.applyToControl)
            continue;
        binding.applyToControl(snapshot.value(binding.id));
    }
}

void SettingsDialog::writeSnapshot(
    QSettings &settings, const QVariantMap &snapshot,
    const QVector<SettingBinding> &bindings) const {
    for (const auto &binding : bindings) {
        if (!binding.writePersisted)
            continue;
        binding.writePersisted(settings, snapshot.value(binding.id));
    }
}

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

    QFormLayout *shortcutsForm = nullptr;
    QWidget *shortcutsPage = createFormPage(tr("Shortcuts"), shortcutsForm);
    shortcutsForm->setVerticalSpacing(8);
    {
        auto *shortcutsHint = new QLabel(
            tr("Select an action and press the new key combination directly in "
               "the field."),
            shortcutsPage);
        shortcutsHint->setWordWrap(true);
        shortcutsForm->addRow(QString(), shortcutsHint);
    }
    queueShortcutEdit_ = new QKeySequenceEdit(shortcutsPage);
    queueShortcutEdit_->setMinimumWidth(kFieldMinWidth);
    queueShortcutEdit_->setMaximumWidth(kFieldMaxWidth);
    addLabeledRow(shortcutsForm, shortcutsPage, tr("Transfers shortcut:"),
                  queueShortcutEdit_);
    historyShortcutEdit_ = new QKeySequenceEdit(shortcutsPage);
    historyShortcutEdit_->setMinimumWidth(kFieldMinWidth);
    historyShortcutEdit_->setMaximumWidth(kFieldMaxWidth);
    addLabeledRow(shortcutsForm, shortcutsPage, tr("History shortcut:"),
                  historyShortcutEdit_);

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

    // Load from QSettings through declarative bindings
    QSettings s("OpenSCP", "OpenSCP");
    const auto bindings = buildSettingBindings();
    const QVariantMap persistedSnapshot =
        readPersistedSnapshot(s, bindings);
    applySnapshotToControls(persistedSnapshot, bindings);
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
    if (queueShortcutEdit_) {
        connect(queueShortcutEdit_, &QKeySequenceEdit::keySequenceChanged, this,
                &SettingsDialog::updateApplyFromControls);
    }
    if (historyShortcutEdit_) {
        connect(historyShortcutEdit_, &QKeySequenceEdit::keySequenceChanged,
                this, &SettingsDialog::updateApplyFromControls);
    }
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
    QSettings s("OpenSCP", "OpenSCP");
    const auto bindings = buildSettingBindings();
    const QVariantMap persistedSnapshot = readPersistedSnapshot(s, bindings);
    const QVariantMap currentSnapshot = readCurrentSnapshot(bindings);
    const QString prevLang =
        persistedSnapshot.value(QStringLiteral("UI/language")).toString();

    writeSnapshot(s, currentSnapshot, bindings);
    s.sync();

    // Only notify if language actually changed
    const QString chosenLang =
        currentSnapshot.value(QStringLiteral("UI/language")).toString();
    if (prevLang != chosenLang) {
        UiAlerts::information(
            this, tr("Language"),
            tr("Language changes take effect after restart."));
    }
    if (applyBtn_) {
        applyBtn_->setEnabled(false);
        applyBtn_->setDefault(false);
    }
    emit settingsApplied();
}

void SettingsDialog::updateApplyFromControls() {
    QSettings s("OpenSCP", "OpenSCP");
    const auto bindings = buildSettingBindings();
    const QVariantMap persistedSnapshot = readPersistedSnapshot(s, bindings);
    const QVariantMap currentSnapshot = readCurrentSnapshot(bindings);

    bool modified = false;
    for (const auto &binding : bindings) {
        if (currentSnapshot.value(binding.id) !=
            persistedSnapshot.value(binding.id)) {
            modified = true;
            break;
        }
    }

    if (applyBtn_) {
        applyBtn_->setEnabled(modified);
        applyBtn_->setDefault(modified);
    }
}
