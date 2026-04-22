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
    QString downloadPath =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (downloadPath.isEmpty())
        downloadPath = QDir::homePath() + "/Downloads";
    return downloadPath;
}

constexpr int kQueueAutoClearOff = 0;
constexpr int kQueueAutoClearCompleted = 1;
constexpr int kQueueAutoClearFailedCanceled = 2;
constexpr int kQueueAutoClearFinished = 3;
constexpr const char *kShortcutTransfersKey = "Shortcuts/openTransfers";
constexpr const char *kShortcutHistoryKey = "Shortcuts/openHistory";
constexpr int kFieldMinWidth = 320;
constexpr int kFieldMaxWidth = 520;

using ComboItem = QPair<QString, QVariant>;

static void addComboItems(QComboBox *combo,
                          std::initializer_list<ComboItem> items) {
    if (!combo)
        return;
    for (const auto &item : items)
        combo->addItem(item.first, item.second);
}

static void removeSettingsKeys(QSettings *settings,
                               std::initializer_list<const char *> keys) {
    if (!settings)
        return;
    for (const char *key : keys)
        settings->remove(QString::fromLatin1(key));
}

QVector<SettingsDialog::SettingBinding>
SettingsDialog::buildSettingBindings() const {
    // Build a single source of truth for settings I/O and control sync.
    QVector<SettingBinding> bindings;
    bindings.reserve(40);

    using StringNormalizer = std::function<QString(const QString &)>;
    using IntNormalizer = std::function<int(int)>;

    // Factory helpers keep each binding consistent (read current/apply/write).
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
        snapshot.insert(binding.bindingKey, binding.readPersisted(settings));
    }
    return snapshot;
}

QVariantMap
SettingsDialog::readCurrentSnapshot(const QVector<SettingBinding> &bindings) const {
    QVariantMap snapshot;
    for (const auto &binding : bindings) {
        if (!binding.readCurrent)
            continue;
        snapshot.insert(binding.bindingKey, binding.readCurrent());
    }
    return snapshot;
}

void SettingsDialog::applySnapshotToControls(
    const QVariantMap &snapshot, const QVector<SettingBinding> &bindings) {
    for (const auto &binding : bindings) {
        if (!binding.applyToControl)
            continue;
        binding.applyToControl(snapshot.value(binding.bindingKey));
    }
}

void SettingsDialog::writeSnapshot(
    QSettings &settings, const QVariantMap &snapshot,
    const QVector<SettingBinding> &bindings) const {
    for (const auto &binding : bindings) {
        if (!binding.writePersisted)
            continue;
        binding.writePersisted(settings, snapshot.value(binding.bindingKey));
    }
}

QString SettingsDialog::wrapTextToWidth(const QString &text,
                                        const QFontMetrics &fontMetrics,
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
            if (current.isEmpty() ||
                fontMetrics.horizontalAdvance(candidate) <= maxWidth) {
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

void SettingsDialog::trackWrappedCheck(QCheckBox *checkBox) {
    if (!checkBox)
        return;
    checkBox->setProperty("rawText", checkBox->text());
    checkBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    wrappedChecks_.push_back(checkBox);
}

void SettingsDialog::refreshWrappedCheckTexts() {
    for (QCheckBox *checkBox : wrappedChecks_) {
        if (!checkBox)
            continue;
        const QString raw = checkBox->property("rawText").toString();
        if (raw.isEmpty())
            continue;

        int widgetWidth = checkBox->width();
        if (widgetWidth <= 0)
            widgetWidth = checkBox->contentsRect().width();
        if (widgetWidth <= 0) {
            int viewportWidth = 0;
            for (QWidget *ancestorWidget = checkBox->parentWidget();
                 ancestorWidget; ancestorWidget = ancestorWidget->parentWidget()) {
                if (auto *scrollArea =
                        qobject_cast<QScrollArea *>(ancestorWidget)) {
                    viewportWidth = scrollArea->viewport()
                                        ? scrollArea->viewport()->width()
                                        : scrollArea->width();
                    break;
                }
            }
            widgetWidth = viewportWidth > 0 ? viewportWidth : width();
        }

        const int indicatorW =
            checkBox->style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr,
                                           checkBox);
        const int indicatorH =
            checkBox->style()->pixelMetric(QStyle::PM_IndicatorHeight, nullptr,
                                           checkBox);
        const int spacingW = checkBox->style()->pixelMetric(
            QStyle::PM_CheckBoxLabelSpacing, nullptr, checkBox);
        const int textWidth = qMax(100, widgetWidth - indicatorW - spacingW - 10);
        const QString wrapped = wrapTextToWidth(raw, QFontMetrics(checkBox->font()),
                                                textWidth);
        if (checkBox->text() != wrapped) {
            checkBox->setText(wrapped);
            checkBox->updateGeometry();
        }
        const int lineCount = wrapped.count('\n') + 1;
        const int textHeight =
            lineCount * QFontMetrics(checkBox->font()).lineSpacing();
        const int minHeight = qMax(indicatorH, textHeight) + 6;
        if (checkBox->minimumHeight() != minHeight)
            checkBox->setMinimumHeight(minHeight);
    }
}

void SettingsDialog::setupSectionList(QListWidget *sectionList) const {
    if (!sectionList)
        return;
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
}

void SettingsDialog::recalcSectionListWidth(QListWidget *sectionList) const {
    if (!sectionList)
        return;
    const QFontMetrics fm(sectionList->font());
    int maxTextWidth = 0;
    for (int rowIndex = 0; rowIndex < sectionList->count(); ++rowIndex) {
        if (auto *item = sectionList->item(rowIndex))
            maxTextWidth = qMax(maxTextWidth, fm.horizontalAdvance(item->text()));
    }
    const int width = qBound(150, maxTextWidth + 48, 260);
    sectionList->setFixedWidth(width);
}

QWidget *SettingsDialog::createFormPage(const PageBuildContext &ctx,
                                        const QString &title,
                                        QFormLayout *&outForm) const {
    auto *scroll = new QScrollArea(ctx.pages);
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
    if (ctx.pages)
        ctx.pages->addWidget(scroll);
    if (ctx.sectionList)
        ctx.sectionList->addItem(title);
    recalcSectionListWidth(ctx.sectionList);

    outForm = form;
    return page;
}

void SettingsDialog::setFieldWidth(QWidget *field) const {
    if (!field)
        return;
    field->setMinimumWidth(kFieldMinWidth);
    field->setMaximumWidth(kFieldMaxWidth);
}

void SettingsDialog::addLabeledRow(QFormLayout *target, QWidget *parent,
                                   const QString &labelText,
                                   QWidget *field) const {
    auto *label = new QLabel(labelText, parent);
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    target->addRow(label, field);
}

void SettingsDialog::addTrackedCheckRows(
    QFormLayout *target, QWidget *parent,
    std::initializer_list<QPair<QCheckBox **, QString>> rows) {
    for (const auto &row : rows) {
        if (!row.first)
            continue;
        auto *check = new QCheckBox(row.second, parent);
        trackWrappedCheck(check);
        target->addRow(QString(), check);
        *row.first = check;
    }
}

QComboBox *SettingsDialog::addComboRow(QFormLayout *target, QWidget *parent,
                                       const QString &labelText) const {
    auto *combo = new QComboBox(parent);
    setFieldWidth(combo);
    addLabeledRow(target, parent, labelText, combo);
    return combo;
}

QSpinBox *SettingsDialog::addSpinRow(QFormLayout *target, QWidget *parent,
                                     const QString &labelText, int minValue,
                                     int maxValue, int defaultValue,
                                     const QString &suffix, int minWidth,
                                     int step,
                                     const QString &toolTip) const {
    auto *spin = new QSpinBox(parent);
    spin->setRange(minValue, maxValue);
    spin->setValue(defaultValue);
    if (!suffix.isEmpty())
        spin->setSuffix(suffix);
    spin->setMinimumWidth(minWidth);
    if (step > 1)
        spin->setSingleStep(step);
    if (!toolTip.isEmpty())
        spin->setToolTip(toolTip);
    addLabeledRow(target, parent, labelText, spin);
    return spin;
}

void SettingsDialog::addBrowsePathRow(QFormLayout *target, QWidget *parent,
                                      const QString &labelText,
                                      const QString &dialogTitle, bool pickFile,
                                      QLineEdit *&editOut,
                                      QPushButton *&browseButtonOut,
                                      const QString &placeholder) {
    auto *rowWidget = new QWidget(parent);
    auto *row = new QHBoxLayout(rowWidget);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);

    auto *edit = new QLineEdit(parent);
    setFieldWidth(edit);
    if (!placeholder.isEmpty())
        edit->setPlaceholderText(placeholder);

    auto *browseButton = new QPushButton(tr("Choose…"), parent);
    row->addWidget(edit, 1);
    row->addWidget(browseButton);
    addLabeledRow(target, parent, labelText, rowWidget);

    connect(browseButton, &QPushButton::clicked, this,
            [this, edit, dialogTitle, pickFile] {
                const QString currentPath = edit ? edit->text() : QString();
                const QString basePath =
                    currentPath.isEmpty() ? QDir::homePath() : currentPath;
                const QString pickedPath =
                    pickFile
                        ? QFileDialog::getOpenFileName(this, dialogTitle, basePath)
                        : QFileDialog::getExistingDirectory(this, dialogTitle,
                                                            basePath);
                if (!pickedPath.isEmpty() && edit)
                    edit->setText(pickedPath);
            });

    editOut = edit;
    browseButtonOut = browseButton;
}

void SettingsDialog::buildGeneralPage(const PageBuildContext &ctx) {
    QFormLayout *generalForm = nullptr;
    QWidget *generalPage = createFormPage(ctx, tr("General"), generalForm);
    langCombo_ = addComboRow(generalForm, generalPage, tr("Language:"));
    addComboItems(langCombo_, {{tr("Spanish"), QStringLiteral("es")},
                               {tr("English"), QStringLiteral("en")},
                               {tr("French"), QStringLiteral("fr")},
                               {tr("Portuguese"), QStringLiteral("pt")}});
    clickMode_ = addComboRow(generalForm, generalPage, tr("Open with:"));
    addComboItems(clickMode_, {{tr("Double click"), 2},
                               {tr("Single click"), 1}});
    openBehaviorMode_ =
        addComboRow(generalForm, generalPage, tr("On file open:"));
    addComboItems(openBehaviorMode_, {{tr("Always ask"), QStringLiteral("ask")},
                                      {tr("Show folder"),
                                       QStringLiteral("reveal")},
                                      {tr("Open file"), QStringLiteral("open")}});
    addTrackedCheckRows(
        generalForm, generalPage,
        {{&showHidden_, tr("Show hidden files")},
         {&showConnOnStart_, tr("Open Site Manager on startup")},
         {&showConnOnDisconnect_, tr("Open Site Manager on disconnect")},
         {&showQueueOnEnqueue_, tr("Open queue when enqueuing transfers")}});
    addBrowsePathRow(generalForm, generalPage, tr("Download folder:"),
                     tr("Select download folder"), false,
                     defaultDownloadDirEdit_, defaultDownloadBrowseBtn_);

    resetMainLayoutBtn_ = new QPushButton(tr("Restore default sizes"), generalPage);
    addLabeledRow(generalForm, generalPage, tr("Window layout:"),
                  resetMainLayoutBtn_);
    connect(resetMainLayoutBtn_, &QPushButton::clicked, this, [this] {
        const auto ret = UiAlerts::question(
            this, tr("Restore layout"),
            tr("Restore the main window layout and column sizes to their "
               "defaults?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes)
            return;

        QSettings settings("OpenSCP", "OpenSCP");
        removeSettingsKeys(&settings, {"UI/mainWindow/geometry",
                                       "UI/mainWindow/windowState",
                                       "UI/mainWindow/splitterState",
                                       "UI/mainWindow/leftHeaderState",
                                       "UI/mainWindow/rightHeaderLocal",
                                       "UI/mainWindow/rightHeaderRemote"});
        settings.sync();
        bool appliedNow = false;
        if (QWidget *parentWindow = parentWidget()) {
            appliedNow = QMetaObject::invokeMethod(
                parentWindow, "resetMainWindowLayoutToDefaults",
                Qt::DirectConnection);
        }
        UiAlerts::information(
            this, tr("Restore layout"),
            appliedNow ? tr("Default layout restored.")
                       : tr("Default layout will be used the next time the app "
                            "starts."));
    });
}

void SettingsDialog::buildShortcutsPage(const PageBuildContext &ctx) {
    QFormLayout *shortcutsForm = nullptr;
    QWidget *shortcutsPage = createFormPage(ctx, tr("Shortcuts"), shortcutsForm);
    shortcutsForm->setVerticalSpacing(8);
    auto *shortcutsHint = new QLabel(
        tr("Select an action and press the new key combination directly in the "
           "field."),
        shortcutsPage);
    shortcutsHint->setWordWrap(true);
    shortcutsForm->addRow(QString(), shortcutsHint);
    queueShortcutEdit_ = new QKeySequenceEdit(shortcutsPage);
    setFieldWidth(queueShortcutEdit_);
    addLabeledRow(shortcutsForm, shortcutsPage, tr("Transfers shortcut:"),
                  queueShortcutEdit_);
    historyShortcutEdit_ = new QKeySequenceEdit(shortcutsPage);
    setFieldWidth(historyShortcutEdit_);
    addLabeledRow(shortcutsForm, shortcutsPage, tr("History shortcut:"),
                  historyShortcutEdit_);
}

void SettingsDialog::buildTransfersPage(const PageBuildContext &ctx) {
    QFormLayout *transfersForm = nullptr;
    QWidget *transfersPage = createFormPage(ctx, tr("Transfers"), transfersForm);
    transfersForm->setVerticalSpacing(10);
    maxConcurrentSpin_ = addSpinRow(
        transfersForm, transfersPage, tr("Parallel tasks:"), 1, 8, 2,
        QString(), 90, 1, tr("Maximum number of concurrent transfers."));
    globalSpeedDefaultSpin_ = addSpinRow(
        transfersForm, transfersPage, tr("Default global limit:"), 0, 1'000'000,
        0, tr(" KB/s"), 120, 1, tr("0 = no global speed limit."));
    queueAutoClearModeDefault_ = new QComboBox(transfersPage);
    setFieldWidth(queueAutoClearModeDefault_);
    addComboItems(queueAutoClearModeDefault_,
                  {{tr("Off"), kQueueAutoClearOff},
                   {tr("Completed"), kQueueAutoClearCompleted},
                   {tr("Failed/Canceled"), kQueueAutoClearFailedCanceled},
                   {tr("All finished"), kQueueAutoClearFinished}});
    addLabeledRow(transfersForm, transfersPage, tr("Queue auto-clear default:"),
                  queueAutoClearModeDefault_);
    queueAutoClearMinutesDefaultSpin_ = addSpinRow(
        transfersForm, transfersPage, tr("Queue auto-clear after:"), 1, 1440, 15,
        tr(" min"));
    connect(queueAutoClearModeDefault_, &QComboBox::currentIndexChanged, this,
            [this](int) {
                updateQueueAutoClearDefaultsUi();
                updateApplyFromControls();
            });
}

void SettingsDialog::buildSitesPage(const PageBuildContext &ctx) {
    QFormLayout *sitesForm = nullptr;
    QWidget *sitesPage = createFormPage(ctx, tr("Sites"), sitesForm);
    sitesForm->setVerticalSpacing(8);
    defaultProtocol_ = new QComboBox(sitesPage);
    setFieldWidth(defaultProtocol_);
    addComboItems(defaultProtocol_,
                  {{tr("SFTP"), static_cast<int>(openscp::Protocol::Sftp)},
                   {tr("SCP"), static_cast<int>(openscp::Protocol::Scp)},
                   {tr("FTP"), static_cast<int>(openscp::Protocol::Ftp)},
                   {tr("FTPS"), static_cast<int>(openscp::Protocol::Ftps)},
                   {tr("WebDAV"), static_cast<int>(openscp::Protocol::WebDav)}});
    addLabeledRow(sitesForm, sitesPage, tr("Default protocol:"), defaultProtocol_);
    scpModeDefault_ = new QComboBox(sitesPage);
    setFieldWidth(scpModeDefault_);
    addComboItems(scpModeDefault_,
                  {{tr("Automatic (SCP with SFTP fallback)"),
                    static_cast<int>(openscp::ScpTransferMode::Auto)},
                   {tr("SCP only (disable SFTP fallback)"),
                    static_cast<int>(openscp::ScpTransferMode::ScpOnly)}});
    addLabeledRow(sitesForm, sitesPage, tr("Default SCP mode:"), scpModeDefault_);
    addTrackedCheckRows(
        sitesForm, sitesPage,
        {{&deleteSecretsOnRemove_,
          tr("When deleting a site, also remove its stored credentials.")}});
}

void SettingsDialog::buildSecurityPage(const PageBuildContext &ctx) {
    QFormLayout *securityForm = nullptr;
    QWidget *securityPage = createFormPage(ctx, tr("Security"), securityForm);
    securityForm->setVerticalSpacing(8);
    defaultKnownHostsPolicy_ = new QComboBox(securityPage);
    setFieldWidth(defaultKnownHostsPolicy_);
    addComboItems(defaultKnownHostsPolicy_,
                  {{tr("Strict"),
                    static_cast<int>(openscp::KnownHostsPolicy::Strict)},
                   {tr("Accept new (TOFU)"),
                    static_cast<int>(openscp::KnownHostsPolicy::AcceptNew)},
                   {tr("No verification (double confirmation, expires in 15 min)"),
                    static_cast<int>(openscp::KnownHostsPolicy::Off)}});
    addLabeledRow(securityForm, securityPage, tr("Default known_hosts policy:"),
                  defaultKnownHostsPolicy_);
    defaultIntegrityPolicy_ = new QComboBox(securityPage);
    setFieldWidth(defaultIntegrityPolicy_);
    addComboItems(defaultIntegrityPolicy_,
                  {{tr("Optional (recommended)"),
                    static_cast<int>(
                        openscp::TransferIntegrityPolicy::Optional)},
                   {tr("Required (strict)"),
                    static_cast<int>(
                        openscp::TransferIntegrityPolicy::Required)},
                   {tr("Off (not recommended)"),
                    static_cast<int>(openscp::TransferIntegrityPolicy::Off)}});
    addLabeledRow(securityForm, securityPage, tr("Default integrity policy:"),
                  defaultIntegrityPolicy_);
    addTrackedCheckRows(
        securityForm, securityPage,
        {{&ftpsVerifyPeerDefault_,
          tr("Verify FTPS server certificate by default (recommended).")}});
    addBrowsePathRow(securityForm, securityPage, tr("Default FTPS CA bundle:"),
                     tr("Select FTPS CA bundle"), true,
                     ftpsCaCertPathDefaultEdit_,
                     ftpsCaCertPathDefaultBrowseBtn_,
                     tr("System CA bundle"));
    addTrackedCheckRows(
        securityForm, securityPage,
        {{&knownHostsHashed_, tr("Hash hostnames in known_hosts (recommended).")},
         {&fpHex_, tr("Show fingerprint in HEX (colon) format (visual only).")},
         {&terminalForceInteractiveLogin_,
          tr("Force interactive login when using Open in terminal "
             "(disable key/agent auth).")},
         {&terminalEnableSftpCliFallback_,
          tr("Enable automatic SFTP CLI fallback when using Open in terminal.")}});
#if defined(Q_OS_MAC) || defined(Q_OS_MACOS) || defined(__APPLE__)
    addTrackedCheckRows(
        securityForm, securityPage,
        {{&macKeychainRestrictive_,
          tr("Use stricter Keychain accessibility (this device only).")}});
#endif
#if !defined(__APPLE__) && !defined(Q_OS_MAC) && !defined(Q_OS_MACOS) &&       \
    !defined(HAVE_LIBSECRET) && !defined(OPENSCP_BUILD_SECURE_ONLY)
    addTrackedCheckRows(
        securityForm, securityPage,
        {{&insecureFallback_,
          tr("Allow insecure credentials fallback (not recommended).")}});
#endif
    noHostVerifyTtlMinSpin_ = addSpinRow(
        securityForm, securityPage, tr("No-verification TTL:"), 1, 120, 15,
        tr(" min"), 100, 1,
        tr("Duration of the temporary exception for no host-key verification "
           "policy."));
}

void SettingsDialog::buildNetworkPage(const PageBuildContext &ctx) {
    QFormLayout *networkForm = nullptr;
    QWidget *networkPage = createFormPage(ctx, tr("Network"), networkForm);
    networkForm->setVerticalSpacing(8);
    sessionHealthIntervalSecSpin_ = addSpinRow(
        networkForm, networkPage, tr("Session health check interval:"), 60, 86400,
        600, tr(" s"));
    remoteWriteabilityTtlMsSpin_ = addSpinRow(
        networkForm, networkPage, tr("Remote writeability cache TTL:"), 1000,
        120000, 15000, tr(" ms"), 110, 500);
}

void SettingsDialog::buildStagingPage(const PageBuildContext &ctx) {
    QFormLayout *stagingForm = nullptr;
    QWidget *stagingPage =
        createFormPage(ctx, tr("Staging and drag-out"), stagingForm);
    stagingForm->setVerticalSpacing(8);
    addBrowsePathRow(stagingForm, stagingPage, tr("Staging folder:"),
                     tr("Select staging folder"), false, stagingRootEdit_,
                     stagingBrowseBtn_);
    addTrackedCheckRows(
        stagingForm, stagingPage,
        {{&autoCleanStaging_,
          tr("Auto-clean staging after successful drag-out (recommended).")}});
    stagingRetentionDaysSpin_ = addSpinRow(
        stagingForm, stagingPage, tr("Startup cleanup retention:"), 1, 365, 7,
        tr(" days"), 110);
    stagingPrepTimeoutMsSpin_ = addSpinRow(
        stagingForm, stagingPage, tr("Preparation timeout:"), 250, 60000, 2000,
        tr(" ms"), 110, 250,
        tr("Time before showing the Wait/Cancel dialog."));
    stagingConfirmItemsSpin_ = addSpinRow(
        stagingForm, stagingPage, tr("Confirm from items:"), 50, 100000, 500,
        QString(), 110, 1,
        tr("Item count threshold to request confirmation for large batches."));
    stagingConfirmMiBSpin_ = addSpinRow(
        stagingForm, stagingPage, tr("Confirm from size:"), 128, 65536, 1024,
        tr(" MiB"), 120, 1,
        tr("Estimated size threshold to request confirmation for large "
           "batches."));
    auto *rowWidget = new QWidget(stagingPage);
    auto *row = new QHBoxLayout(rowWidget);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    maxDepthSpin_ = new QSpinBox(stagingPage);
    maxDepthSpin_->setRange(4, 256);
    maxDepthSpin_->setValue(32);
    maxDepthSpin_->setMinimumWidth(90);
    maxDepthSpin_->setToolTip(
        tr("Limit for recursive folder drag-out to avoid deep trees and loops."));
    auto *hint = new QLabel(tr("Recommended: 32"), stagingPage);
    hint->setStyleSheet("color: palette(window-text);");
    row->addWidget(maxDepthSpin_);
    row->addWidget(hint);
    row->addStretch(1);
    addLabeledRow(stagingForm, stagingPage, tr("Maximum depth:"), rowWidget);
}

void SettingsDialog::setupSectionNavigation(const PageBuildContext &ctx) {
    if (!ctx.sectionList || !ctx.pages)
        return;
    connect(ctx.sectionList, &QListWidget::currentRowChanged, ctx.pages,
            &QStackedWidget::setCurrentIndex);
    connect(ctx.sectionList, &QListWidget::currentRowChanged, this, [this](int) {
        refreshWrappedCheckTexts();
        QTimer::singleShot(0, this, [this] { refreshWrappedCheckTexts(); });
    });
    ctx.sectionList->setCurrentRow(0);
}

void SettingsDialog::buildBottomButtons(QVBoxLayout *root) {
    auto *btnRow = new QWidget(this);
    auto *buttonLayout = new QHBoxLayout(btnRow);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->addStretch();
    closeBtn_ = new QPushButton(tr("Close"), btnRow);
    applyBtn_ = new QPushButton(tr("Apply"), btnRow);
    buttonLayout->addWidget(closeBtn_);
    buttonLayout->addWidget(applyBtn_);
    root->addWidget(btnRow);

    applyBtn_->setEnabled(false);
    applyBtn_->setAutoDefault(true);
    applyBtn_->setDefault(false);
    closeBtn_->setAutoDefault(false);
    closeBtn_->setDefault(false);
    connect(applyBtn_, &QPushButton::clicked, this, &SettingsDialog::onApply);
    connect(closeBtn_, &QPushButton::clicked, this, &SettingsDialog::reject);
}

void SettingsDialog::loadPersistedSettings() {
    QSettings settings("OpenSCP", "OpenSCP");
    const auto bindings = buildSettingBindings();
    const QVariantMap persistedSnapshot =
        readPersistedSnapshot(settings, bindings);
    applySnapshotToControls(persistedSnapshot, bindings);
    updateQueueAutoClearDefaultsUi();
}

void SettingsDialog::updateQueueAutoClearDefaultsUi() {
    if (!queueAutoClearModeDefault_ || !queueAutoClearMinutesDefaultSpin_)
        return;
    const bool enabled =
        queueAutoClearModeDefault_->currentData().toInt() != kQueueAutoClearOff;
    queueAutoClearMinutesDefaultSpin_->setEnabled(enabled);
}

void SettingsDialog::connectDirtyTracking() {
    auto bindDirtyFlag = [this]<typename Sender, typename Signal>(
                             Sender *sender, Signal signal) {
        if (sender)
            connect(sender, signal, this,
                    &SettingsDialog::updateApplyFromControls);
    };

    for (QComboBox *combo :
         {langCombo_, clickMode_, openBehaviorMode_, defaultProtocol_,
          scpModeDefault_, defaultKnownHostsPolicy_, defaultIntegrityPolicy_,
          queueAutoClearModeDefault_}) {
        bindDirtyFlag(combo, qOverload<int>(&QComboBox::currentIndexChanged));
    }
    for (QCheckBox *check : {showHidden_,
                             showConnOnStart_,
                             showConnOnDisconnect_,
                             showQueueOnEnqueue_,
                             deleteSecretsOnRemove_,
                             ftpsVerifyPeerDefault_,
                             knownHostsHashed_,
                             fpHex_,
                             terminalForceInteractiveLogin_,
                             terminalEnableSftpCliFallback_,
                             autoCleanStaging_}) {
        bindDirtyFlag(check, &QCheckBox::toggled);
    }
    for (QLineEdit *edit :
         {defaultDownloadDirEdit_, ftpsCaCertPathDefaultEdit_, stagingRootEdit_}) {
        bindDirtyFlag(edit, &QLineEdit::textChanged);
    }
    for (QSpinBox *spin : {noHostVerifyTtlMinSpin_,
                           maxConcurrentSpin_,
                           globalSpeedDefaultSpin_,
                           queueAutoClearMinutesDefaultSpin_,
                           sessionHealthIntervalSecSpin_,
                           remoteWriteabilityTtlMsSpin_,
                           stagingRetentionDaysSpin_,
                           stagingPrepTimeoutMsSpin_,
                           stagingConfirmItemsSpin_,
                           stagingConfirmMiBSpin_,
                           maxDepthSpin_}) {
        bindDirtyFlag(spin, qOverload<int>(&QSpinBox::valueChanged));
    }
    for (QKeySequenceEdit *editor : {queueShortcutEdit_, historyShortcutEdit_}) {
        if (editor) {
            connect(editor, &QKeySequenceEdit::keySequenceChanged, this,
                    &SettingsDialog::updateApplyFromControls);
        }
    }
#if defined(Q_OS_MAC) || defined(Q_OS_MACOS) || defined(__APPLE__)
    bindDirtyFlag(macKeychainRestrictive_, &QCheckBox::toggled);
#endif
}

void SettingsDialog::connectInsecureFallbackGuard() {
    if (!insecureFallback_)
        return;
    connect(insecureFallback_, &QCheckBox::toggled, this,
            [this](bool enabled) {
        if (enabled) {
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
    setupSectionList(sectionList);
    contentRow->addWidget(sectionList);

    auto *pages = new QStackedWidget(this);
    contentRow->addWidget(pages, 1);
    const PageBuildContext ctx{sectionList, pages};

    buildGeneralPage(ctx);
    buildShortcutsPage(ctx);
    buildTransfersPage(ctx);
    buildSitesPage(ctx);
    buildSecurityPage(ctx);
    buildNetworkPage(ctx);
    buildStagingPage(ctx);
    setupSectionNavigation(ctx);
    buildBottomButtons(root);
    loadPersistedSettings();
    connectDirtyTracking();
    connectInsecureFallbackGuard();

    updateApplyFromControls();
    QTimer::singleShot(0, this, [this] { refreshWrappedCheckTexts(); });
}

void SettingsDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    refreshWrappedCheckTexts();
}

void SettingsDialog::onApply() {
    QSettings settings("OpenSCP", "OpenSCP");
    const auto bindings = buildSettingBindings();
    const QVariantMap persistedSnapshot =
        readPersistedSnapshot(settings, bindings);
    const QVariantMap currentSnapshot = readCurrentSnapshot(bindings);
    const QString prevLang =
        persistedSnapshot.value(QStringLiteral("UI/language")).toString();

    writeSnapshot(settings, currentSnapshot, bindings);
    settings.sync();

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
    QSettings settings("OpenSCP", "OpenSCP");
    const auto bindings = buildSettingBindings();
    const QVariantMap persistedSnapshot =
        readPersistedSnapshot(settings, bindings);
    const QVariantMap currentSnapshot = readCurrentSnapshot(bindings);

    bool modified = false;
    for (const auto &binding : bindings) {
        if (currentSnapshot.value(binding.bindingKey) !=
            persistedSnapshot.value(binding.bindingKey)) {
            modified = true;
            break;
        }
    }

    if (applyBtn_) {
        applyBtn_->setEnabled(modified);
        applyBtn_->setDefault(modified);
    }
}
