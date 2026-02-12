// Implementation of OpenSCP settings dialog.
#include "SettingsDialog.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QFormLayout>
#include <QPushButton>
#include <QSettings>
#include <QMessageBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QListWidget>
#include <QStackedWidget>
#include <QFileDialog>
#include <QLineEdit>
#include <QSpinBox>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

static QString defaultDownloadDirPath() {
    QString p = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (p.isEmpty()) p = QDir::homePath() + "/Downloads";
    return p;
}

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Ajustes"));
    resize(780, 520);
    setMinimumSize(560, 420);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* contentRow = new QHBoxLayout();
    contentRow->setContentsMargins(0, 0, 0, 0);
    contentRow->setSpacing(10);
    root->addLayout(contentRow, 1);

    auto* sectionList = new QListWidget(this);
    sectionList->setSelectionMode(QAbstractItemView::SingleSelection);
    sectionList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sectionList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    sectionList->setFixedWidth(150);
    sectionList->setStyleSheet(
        "QListWidget { border: 1px solid palette(midlight); border-radius: 8px; padding: 6px; }"
        "QListWidget::item { padding: 8px 10px; border-radius: 6px; margin: 1px 0; }"
        "QListWidget::item:selected { background: palette(highlight); color: palette(highlighted-text); }"
    );
    contentRow->addWidget(sectionList);

    auto* pages = new QStackedWidget(this);
    contentRow->addWidget(pages, 1);

    constexpr int kFieldMinWidth = 320;
    constexpr int kFieldMaxWidth = 520;
    auto createFormPage = [pages, sectionList](const QString& title, QFormLayout*& outForm) -> QWidget* {
        auto* page = new QWidget(pages);
        auto* pageLay = new QVBoxLayout(page);
        pageLay->setContentsMargins(12, 12, 12, 12);
        pageLay->setSpacing(8);
        auto* form = new QFormLayout();
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::DontWrapRows);
        form->setHorizontalSpacing(8);
        form->setVerticalSpacing(8);
        pageLay->addLayout(form);
        pageLay->addStretch(1);
        pages->addWidget(page);
        sectionList->addItem(title);
        outForm = form;
        return page;
    };
    auto addCheckRow = [](QFormLayout* target, QWidget* parent, const QString& text) {
        auto* cb = new QCheckBox(text, parent);
        target->addRow(QString(), cb);
        return cb;
    };
    auto addComboRow = [kFieldMinWidth, kFieldMaxWidth](QFormLayout* target, QWidget* parent, const QString& labelText) {
        auto* combo = new QComboBox(parent);
        combo->setMinimumWidth(kFieldMinWidth);
        combo->setMaximumWidth(kFieldMaxWidth);
        target->addRow(labelText, combo);
        return combo;
    };
    auto bindDirtyFlag = [this]<typename Sender, typename Signal>(Sender* sender, Signal signal) {
        if (sender) connect(sender, signal, this, &SettingsDialog::updateApplyFromControls);
    };

    QFormLayout* generalForm = nullptr;
    QWidget* generalPage = createFormPage(tr("General"), generalForm);

    // General settings
    langCombo_ = addComboRow(generalForm, generalPage, tr("Idioma:"));
    langCombo_->addItem("Español", "es");
    langCombo_->addItem("English", "en");
    clickMode_ = addComboRow(generalForm, generalPage, tr("Apertura con:"));
    clickMode_->addItem(tr("Doble clic"), 2);
    clickMode_->addItem(tr("Un clic"), 1);
    openBehaviorMode_ = addComboRow(generalForm, generalPage, tr("Al abrir archivos:"));
    openBehaviorMode_->addItem(tr("Preguntar siempre"), QStringLiteral("ask"));
    openBehaviorMode_->addItem(tr("Mostrar carpeta"), QStringLiteral("reveal"));
    openBehaviorMode_->addItem(tr("Abrir archivo"), QStringLiteral("open"));

    showHidden_ = addCheckRow(generalForm, generalPage, tr("Mostrar archivos ocultos"));
    showConnOnStart_ = addCheckRow(generalForm, generalPage, tr("Abrir Gestor de Sitios al iniciar"));
    showConnOnDisconnect_ = addCheckRow(generalForm, generalPage, tr("Abrir Gestor de Sitios al desconectar"));
    showQueueOnEnqueue_ = addCheckRow(generalForm, generalPage, tr("Abrir cola al encolar transferencias"));
    {
        auto* rowWidget = new QWidget(generalPage);
        auto* row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        defaultDownloadDirEdit_ = new QLineEdit(generalPage);
        defaultDownloadDirEdit_->setMinimumWidth(kFieldMinWidth);
        defaultDownloadDirEdit_->setMaximumWidth(kFieldMaxWidth);
        defaultDownloadBrowseBtn_ = new QPushButton(tr("Elegir…"), generalPage);
        row->addWidget(defaultDownloadDirEdit_, 1);
        row->addWidget(defaultDownloadBrowseBtn_);
        generalForm->addRow(tr("Carpeta descargas:"), rowWidget);
        connect(defaultDownloadBrowseBtn_, &QPushButton::clicked, this, [this] {
            const QString cur = defaultDownloadDirEdit_ ? defaultDownloadDirEdit_->text() : QString();
            const QString pick = QFileDialog::getExistingDirectory(
                this, tr("Selecciona carpeta de descargas"),
                cur.isEmpty() ? QDir::homePath() : cur
            );
            if (!pick.isEmpty() && defaultDownloadDirEdit_) defaultDownloadDirEdit_->setText(pick);
        });
    }

    QFormLayout* advancedForm = nullptr;
    QWidget* advancedPage = createFormPage(tr("Avanzado"), advancedForm);
    advancedForm->setVerticalSpacing(10);

    // Advanced/Transfers
    auto* transferGroup = new QGroupBox(tr("Transferencias"), advancedPage);
    auto* transferForm = new QFormLayout(transferGroup);
    transferForm->setContentsMargins(12, 10, 12, 10);
    transferForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    transferForm->setHorizontalSpacing(8);
    transferForm->setVerticalSpacing(8);
    maxConcurrentSpin_ = new QSpinBox(transferGroup);
    maxConcurrentSpin_->setRange(1, 8);
    maxConcurrentSpin_->setValue(2);
    maxConcurrentSpin_->setMinimumWidth(90);
    maxConcurrentSpin_->setToolTip(tr("Cantidad máxima de transferencias simultáneas."));
    transferForm->addRow(tr("Simultáneas:"), maxConcurrentSpin_);
    globalSpeedDefaultSpin_ = new QSpinBox(transferGroup);
    globalSpeedDefaultSpin_->setRange(0, 1'000'000);
    globalSpeedDefaultSpin_->setValue(0);
    globalSpeedDefaultSpin_->setSuffix(" KB/s");
    globalSpeedDefaultSpin_->setMinimumWidth(120);
    globalSpeedDefaultSpin_->setToolTip(tr("0 = sin límite global de velocidad."));
    transferForm->addRow(tr("Límite global por defecto:"), globalSpeedDefaultSpin_);
    advancedForm->addRow(QString(), transferGroup);

    // Advanced/Sites
    auto* sitesGroup = new QGroupBox(tr("Sitios"), advancedPage);
    auto* sitesLay = new QVBoxLayout(sitesGroup);
    sitesLay->setContentsMargins(12, 10, 12, 10);
    sitesLay->setSpacing(6);
    deleteSecretsOnRemove_ = new QCheckBox(tr("Al eliminar un sitio, borrar también sus credenciales guardadas."), sitesGroup);
    sitesLay->addWidget(deleteSecretsOnRemove_);
    advancedForm->addRow(QString(), sitesGroup);

    // Advanced/Security
    auto* securityGroup = new QGroupBox(tr("Seguridad"), advancedPage);
    auto* securityLay = new QVBoxLayout(securityGroup);
    securityLay->setContentsMargins(12, 10, 12, 10);
    securityLay->setSpacing(6);
    knownHostsHashed_ = new QCheckBox(tr("Guardar hostnames en known_hosts como hash (recomendado)."), securityGroup);
    fpHex_ = new QCheckBox(tr("Mostrar huella en HEX colonado (solo visual)."), securityGroup);
    securityLay->addWidget(knownHostsHashed_);
    securityLay->addWidget(fpHex_);
#if defined(Q_OS_MAC) || defined(Q_OS_MACOS) || defined(__APPLE__)
    macKeychainRestrictive_ = new QCheckBox(tr("Usar accesibilidad de llavero más restrictiva (solo en este dispositivo)."), securityGroup);
    securityLay->addWidget(macKeychainRestrictive_);
#endif
#if !defined(__APPLE__) && !defined(Q_OS_MAC) && !defined(Q_OS_MACOS) && !defined(HAVE_LIBSECRET) && !defined(OPEN_SCP_BUILD_SECURE_ONLY)
    insecureFallback_ = new QCheckBox(tr("Permitir fallback inseguro de credenciales (no recomendado)."), securityGroup);
    securityLay->addWidget(insecureFallback_);
#endif
    {
        auto* ttlRow = new QWidget(securityGroup);
        auto* ttlLay = new QHBoxLayout(ttlRow);
        ttlLay->setContentsMargins(0, 0, 0, 0);
        ttlLay->setSpacing(6);
        noHostVerifyTtlMinSpin_ = new QSpinBox(securityGroup);
        noHostVerifyTtlMinSpin_->setRange(1, 120);
        noHostVerifyTtlMinSpin_->setValue(15);
        noHostVerifyTtlMinSpin_->setSuffix(tr(" min"));
        noHostVerifyTtlMinSpin_->setMinimumWidth(100);
        noHostVerifyTtlMinSpin_->setToolTip(tr("Duración de la excepción temporal para la política sin verificación de host key."));
        ttlLay->addWidget(noHostVerifyTtlMinSpin_);
        ttlLay->addStretch(1);
        auto* ttlForm = new QFormLayout();
        ttlForm->setContentsMargins(0, 0, 0, 0);
        ttlForm->addRow(tr("TTL sin verificación:"), ttlRow);
        securityLay->addLayout(ttlForm);
    }
    advancedForm->addRow(QString(), securityGroup);

    // Advanced/Staging and drag-out
    auto* stagingGroup = new QGroupBox(tr("Staging y arrastre"), advancedPage);
    auto* stagingForm = new QFormLayout(stagingGroup);
    stagingForm->setContentsMargins(12, 10, 12, 10);
    stagingForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    stagingForm->setHorizontalSpacing(8);
    stagingForm->setVerticalSpacing(8);
    {
        auto* rowWidget = new QWidget(stagingGroup);
        auto* row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        stagingRootEdit_ = new QLineEdit(stagingGroup);
        stagingRootEdit_->setMinimumWidth(kFieldMinWidth);
        stagingRootEdit_->setMaximumWidth(kFieldMaxWidth);
        stagingBrowseBtn_ = new QPushButton(tr("Elegir…"), stagingGroup);
        row->addWidget(stagingRootEdit_, 1);
        row->addWidget(stagingBrowseBtn_);
        stagingForm->addRow(tr("Carpeta staging:"), rowWidget);
        connect(stagingBrowseBtn_, &QPushButton::clicked, this, [this] {
            const QString cur = stagingRootEdit_ ? stagingRootEdit_->text() : QString();
            const QString pick = QFileDialog::getExistingDirectory(
                this, tr("Selecciona carpeta de staging"),
                cur.isEmpty() ? QDir::homePath() : cur
            );
            if (!pick.isEmpty() && stagingRootEdit_) stagingRootEdit_->setText(pick);
        });
    }
    autoCleanStaging_ = new QCheckBox(tr("Eliminar automáticamente la carpeta staging tras completar el arrastre (recomendado)."), stagingGroup);
    stagingForm->addRow(QString(), autoCleanStaging_);
    stagingPrepTimeoutMsSpin_ = new QSpinBox(stagingGroup);
    stagingPrepTimeoutMsSpin_->setRange(250, 60000);
    stagingPrepTimeoutMsSpin_->setSingleStep(250);
    stagingPrepTimeoutMsSpin_->setValue(2000);
    stagingPrepTimeoutMsSpin_->setSuffix(tr(" ms"));
    stagingPrepTimeoutMsSpin_->setMinimumWidth(110);
    stagingPrepTimeoutMsSpin_->setToolTip(tr("Tiempo antes de mostrar el diálogo Esperar/Cancelar."));
    stagingForm->addRow(tr("Timeout preparación:"), stagingPrepTimeoutMsSpin_);
    stagingConfirmItemsSpin_ = new QSpinBox(stagingGroup);
    stagingConfirmItemsSpin_->setRange(50, 100000);
    stagingConfirmItemsSpin_->setValue(500);
    stagingConfirmItemsSpin_->setMinimumWidth(110);
    stagingConfirmItemsSpin_->setToolTip(tr("Cantidad de elementos para pedir confirmación en lotes grandes."));
    stagingForm->addRow(tr("Confirmar desde elementos:"), stagingConfirmItemsSpin_);
    stagingConfirmMiBSpin_ = new QSpinBox(stagingGroup);
    stagingConfirmMiBSpin_->setRange(128, 65536);
    stagingConfirmMiBSpin_->setValue(1024);
    stagingConfirmMiBSpin_->setSuffix(tr(" MiB"));
    stagingConfirmMiBSpin_->setMinimumWidth(120);
    stagingConfirmMiBSpin_->setToolTip(tr("Tamaño estimado para pedir confirmación en lotes grandes."));
    stagingForm->addRow(tr("Confirmar desde tamaño:"), stagingConfirmMiBSpin_);

    // Maximum folder recursion depth
    {
        auto* rowWidget = new QWidget(stagingGroup);
        auto* row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        maxDepthSpin_ = new QSpinBox(stagingGroup);
        maxDepthSpin_->setRange(4, 256);
        maxDepthSpin_->setValue(32);
        maxDepthSpin_->setMinimumWidth(90);
        maxDepthSpin_->setToolTip(tr("Límite para arrastre recursivo y evitar árboles muy profundos y bucles."));
        auto* hint = new QLabel(tr("Recomendado: 32"), stagingGroup);
        hint->setStyleSheet("color: palette(mid);");
        row->addWidget(maxDepthSpin_);
        row->addWidget(hint);
        row->addStretch(1);
        stagingForm->addRow(tr("Profundidad máxima:"), rowWidget);
    }
    advancedForm->addRow(QString(), stagingGroup);
    connect(sectionList, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    sectionList->setCurrentRow(0);

    // Buttons row: align to right, order: Close (left) then Apply (right)
    auto* btnRow = new QWidget(this);
    auto* hb = new QHBoxLayout(btnRow);
    hb->setContentsMargins(0, 0, 0, 0);
    hb->addStretch();
    closeBtn_ = new QPushButton(tr("Cerrar"), btnRow);
    applyBtn_ = new QPushButton(tr("Aplicar"), btnRow);
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
    const QString lang = s.value("UI/language", "es").toString();
    const int li = langCombo_->findData(lang);
    if (li >= 0) langCombo_->setCurrentIndex(li);
    if (showHidden_) showHidden_->setChecked(s.value("UI/showHidden", false).toBool());
    if (showConnOnStart_) showConnOnStart_->setChecked(s.value("UI/showConnOnStart", true).toBool());
    if (showConnOnDisconnect_) showConnOnDisconnect_->setChecked(s.value("UI/openSiteManagerOnDisconnect", true).toBool());
    clickMode_->setCurrentIndex(s.value("UI/singleClick", false).toBool() ? 1 : 0);
    QString openBehaviorMode = s.value("UI/openBehaviorMode").toString().trimmed().toLower();
    if (openBehaviorMode.isEmpty()) {
        const bool revealLegacy = s.value("UI/openRevealInFolder", false).toBool();
        openBehaviorMode = revealLegacy ? QStringLiteral("reveal") : QStringLiteral("ask");
    }
    if (openBehaviorMode_ ) {
        int modeIdx = openBehaviorMode_->findData(openBehaviorMode);
        if (modeIdx < 0) modeIdx = openBehaviorMode_->findData(QStringLiteral("ask"));
        if (modeIdx >= 0) openBehaviorMode_->setCurrentIndex(modeIdx);
    }
    if (showQueueOnEnqueue_) showQueueOnEnqueue_->setChecked(s.value("UI/showQueueOnEnqueue", true).toBool());
    if (defaultDownloadDirEdit_) defaultDownloadDirEdit_->setText(s.value("UI/defaultDownloadDir", defaultDownloadDirPath()).toString());
    if (deleteSecretsOnRemove_) deleteSecretsOnRemove_->setChecked(s.value("Sites/deleteSecretsOnRemove", false).toBool());
    if (knownHostsHashed_) knownHostsHashed_->setChecked(s.value("Security/knownHostsHashed", true).toBool());
    if (fpHex_) fpHex_->setChecked(s.value("Security/fpHex", false).toBool());
    if (noHostVerifyTtlMinSpin_) noHostVerifyTtlMinSpin_->setValue(s.value("Security/noHostVerificationTtlMin", 15).toInt());
#if !defined(__APPLE__) && !defined(Q_OS_MAC) && !defined(Q_OS_MACOS) && !defined(HAVE_LIBSECRET) && !defined(OPEN_SCP_BUILD_SECURE_ONLY)
    if (insecureFallback_) insecureFallback_->setChecked(s.value("Security/enableInsecureSecretFallback", false).toBool());
#endif
    if (maxConcurrentSpin_) maxConcurrentSpin_->setValue(s.value("Transfer/maxConcurrent", 2).toInt());
    if (globalSpeedDefaultSpin_) globalSpeedDefaultSpin_->setValue(s.value("Transfer/globalSpeedKBps", 0).toInt());
    if (stagingRootEdit_) stagingRootEdit_->setText(s.value("Advanced/stagingRoot", QDir::homePath() + "/Downloads/OpenSCP-Dragged").toString());
    if (autoCleanStaging_) autoCleanStaging_->setChecked(s.value("Advanced/autoCleanStaging", true).toBool());
    if (stagingPrepTimeoutMsSpin_) stagingPrepTimeoutMsSpin_->setValue(s.value("Advanced/stagingPrepTimeoutMs", 2000).toInt());
    if (stagingConfirmItemsSpin_) stagingConfirmItemsSpin_->setValue(s.value("Advanced/stagingConfirmItems", 500).toInt());
    if (stagingConfirmMiBSpin_) stagingConfirmMiBSpin_->setValue(s.value("Advanced/stagingConfirmMiB", 1024).toInt());
    if (maxDepthSpin_) maxDepthSpin_->setValue(s.value("Advanced/maxFolderDepth", 32).toInt());
#if defined(Q_OS_MAC) || defined(Q_OS_MACOS) || defined(__APPLE__)
    if (macKeychainRestrictive_) macKeychainRestrictive_->setChecked(s.value("Security/macKeychainRestrictive", false).toBool());
#endif

    // Bind controls to dirty flag
    bindDirtyFlag(langCombo_, qOverload<int>(&QComboBox::currentIndexChanged));
    bindDirtyFlag(clickMode_, qOverload<int>(&QComboBox::currentIndexChanged));
    bindDirtyFlag(showHidden_, &QCheckBox::toggled);
    bindDirtyFlag(showConnOnStart_, &QCheckBox::toggled);
    bindDirtyFlag(showConnOnDisconnect_, &QCheckBox::toggled);
    bindDirtyFlag(openBehaviorMode_, qOverload<int>(&QComboBox::currentIndexChanged));
    bindDirtyFlag(showQueueOnEnqueue_, &QCheckBox::toggled);
    bindDirtyFlag(defaultDownloadDirEdit_, &QLineEdit::textChanged);
    bindDirtyFlag(deleteSecretsOnRemove_, &QCheckBox::toggled);
    bindDirtyFlag(knownHostsHashed_, &QCheckBox::toggled);
    bindDirtyFlag(fpHex_, &QCheckBox::toggled);
    bindDirtyFlag(noHostVerifyTtlMinSpin_, qOverload<int>(&QSpinBox::valueChanged));
    bindDirtyFlag(maxConcurrentSpin_, qOverload<int>(&QSpinBox::valueChanged));
    bindDirtyFlag(globalSpeedDefaultSpin_, qOverload<int>(&QSpinBox::valueChanged));
    bindDirtyFlag(stagingRootEdit_, &QLineEdit::textChanged);
    bindDirtyFlag(autoCleanStaging_, &QCheckBox::toggled);
    bindDirtyFlag(stagingPrepTimeoutMsSpin_, qOverload<int>(&QSpinBox::valueChanged));
    bindDirtyFlag(stagingConfirmItemsSpin_, qOverload<int>(&QSpinBox::valueChanged));
    bindDirtyFlag(stagingConfirmMiBSpin_, qOverload<int>(&QSpinBox::valueChanged));
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
                const auto ret = QMessageBox::warning(
                    this,
                    tr("Activar fallback inseguro"),
                    tr("Esto almacenará credenciales sin cifrar en el disco usando QSettings.\n"
                       "En Linux, se recomienda instalar y usar libsecret/Secret Service para mayor seguridad.\n\n"
                       "¿Deseas activar el fallback inseguro igualmente?"),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No
                );
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
}

void SettingsDialog::onApply() {
    const QString chosenLang  = langCombo_->currentData().toString();

    QSettings s("OpenSCP", "OpenSCP");
    const QString prevLang = s.value("UI/language", "es").toString();
    s.setValue("UI/language", chosenLang);
    s.setValue("UI/showHidden", showHidden_ && showHidden_->isChecked());
    s.setValue("UI/showConnOnStart", showConnOnStart_ && showConnOnStart_->isChecked());
    if (showConnOnDisconnect_) s.setValue("UI/openSiteManagerOnDisconnect", showConnOnDisconnect_->isChecked());
    const bool singleClick = (clickMode_ && clickMode_->currentData().toInt() == 1);
    s.setValue("UI/singleClick", singleClick);
    const QString openMode = openBehaviorMode_ ? openBehaviorMode_->currentData().toString() : QStringLiteral("ask");
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
    if (showQueueOnEnqueue_) s.setValue("UI/showQueueOnEnqueue", showQueueOnEnqueue_->isChecked());
    if (defaultDownloadDirEdit_) {
        QString path = defaultDownloadDirEdit_->text().trimmed();
        if (path.isEmpty()) path = defaultDownloadDirPath();
        s.setValue("UI/defaultDownloadDir", QDir::cleanPath(path));
    }
    if (deleteSecretsOnRemove_) s.setValue("Sites/deleteSecretsOnRemove", deleteSecretsOnRemove_->isChecked());
    if (knownHostsHashed_) s.setValue("Security/knownHostsHashed", knownHostsHashed_->isChecked());
    if (fpHex_) s.setValue("Security/fpHex", fpHex_->isChecked());
    if (noHostVerifyTtlMinSpin_) s.setValue("Security/noHostVerificationTtlMin", noHostVerifyTtlMinSpin_->value());
#if defined(Q_OS_MAC) || defined(Q_OS_MACOS) || defined(__APPLE__)
    if (macKeychainRestrictive_) s.setValue("Security/macKeychainRestrictive", macKeychainRestrictive_->isChecked());
#endif
    if (insecureFallback_) s.setValue("Security/enableInsecureSecretFallback", insecureFallback_->isChecked());
    if (maxConcurrentSpin_) s.setValue("Transfer/maxConcurrent", maxConcurrentSpin_->value());
    if (globalSpeedDefaultSpin_) s.setValue("Transfer/globalSpeedKBps", globalSpeedDefaultSpin_->value());
    if (stagingRootEdit_) s.setValue("Advanced/stagingRoot", stagingRootEdit_->text());
    if (autoCleanStaging_) s.setValue("Advanced/autoCleanStaging", autoCleanStaging_->isChecked());
    if (stagingPrepTimeoutMsSpin_) s.setValue("Advanced/stagingPrepTimeoutMs", stagingPrepTimeoutMsSpin_->value());
    if (stagingConfirmItemsSpin_) s.setValue("Advanced/stagingConfirmItems", stagingConfirmItemsSpin_->value());
    if (stagingConfirmMiBSpin_) s.setValue("Advanced/stagingConfirmMiB", stagingConfirmMiBSpin_->value());
    if (maxDepthSpin_) s.setValue("Advanced/maxFolderDepth", maxDepthSpin_->value());
    s.sync();

    // Only notify if language actually changed
    if (prevLang != chosenLang) {
        QMessageBox::information(this, tr("Idioma"), tr("El cambio de idioma se aplicará al reiniciar."));
    }
    if (applyBtn_) { applyBtn_->setEnabled(false); applyBtn_->setDefault(false); }
}

void SettingsDialog::updateApplyFromControls() {
    QSettings s("OpenSCP", "OpenSCP");
    const QString prevLang = s.value("UI/language", "es").toString();
    const bool showHidden = s.value("UI/showHidden", false).toBool();
    const bool showConnOnStart = s.value("UI/showConnOnStart", true).toBool();
    const bool onDisc = s.value("UI/openSiteManagerOnDisconnect", true).toBool();
    const bool singleClick = s.value("UI/singleClick", false).toBool();
    QString openMode = s.value("UI/openBehaviorMode").toString().trimmed().toLower();
    if (openMode.isEmpty()) {
        const bool revealLegacy = s.value("UI/openRevealInFolder", false).toBool();
        openMode = revealLegacy ? QStringLiteral("reveal") : QStringLiteral("ask");
    }
    const bool showQueue = s.value("UI/showQueueOnEnqueue", true).toBool();
    QString defaultDownload = s.value("UI/defaultDownloadDir", defaultDownloadDirPath()).toString().trimmed();
    if (defaultDownload.isEmpty()) defaultDownload = defaultDownloadDirPath();
    defaultDownload = QDir::cleanPath(defaultDownload);
    const bool deleteSecrets = s.value("Sites/deleteSecretsOnRemove", false).toBool();
    const bool knownHashed = s.value("Security/knownHostsHashed", true).toBool();
    const bool fpHex = s.value("Security/fpHex", false).toBool();
    const int noHostVerifyTtlMin = s.value("Security/noHostVerificationTtlMin", 15).toInt();
// Only compare insecure fallback when available in this build/platform
#if !defined(__APPLE__) && !defined(Q_OS_MAC) && !defined(Q_OS_MACOS) && !defined(HAVE_LIBSECRET) && !defined(OPEN_SCP_BUILD_SECURE_ONLY)
    const bool insecureFb = s.value("Security/enableInsecureSecretFallback", false).toBool();
#endif
    const int maxConcurrent = s.value("Transfer/maxConcurrent", 2).toInt();
    const int globalSpeedDefault = s.value("Transfer/globalSpeedKBps", 0).toInt();
    const QString stagingRoot = s.value("Advanced/stagingRoot", QDir::homePath() + "/Downloads/OpenSCP-Dragged").toString();
    const bool autoCleanSt = s.value("Advanced/autoCleanStaging", true).toBool();
    const int stagingPrepTimeoutMs = s.value("Advanced/stagingPrepTimeoutMs", 2000).toInt();
    const int stagingConfirmItems = s.value("Advanced/stagingConfirmItems", 500).toInt();
    const int stagingConfirmMiB = s.value("Advanced/stagingConfirmMiB", 1024).toInt();
    const int  maxDepthPrev = s.value("Advanced/maxFolderDepth", 32).toInt();

    const QString curLang = langCombo_ ? langCombo_->currentData().toString() : prevLang;
    const bool curShowHidden = showHidden_ && showHidden_->isChecked();
    const bool curShowConn = showConnOnStart_ && showConnOnStart_->isChecked();
    const bool curShowConnDisc = showConnOnDisconnect_ && showConnOnDisconnect_->isChecked();
    const bool curSingleClick = (clickMode_ && clickMode_->currentData().toInt() == 1);
    const QString curOpenMode = openBehaviorMode_ ? openBehaviorMode_->currentData().toString() : openMode;
    const bool curShowQueue = showQueueOnEnqueue_ && showQueueOnEnqueue_->isChecked();
    QString curDefaultDownload = defaultDownloadDirEdit_ ? defaultDownloadDirEdit_->text().trimmed() : defaultDownload;
    if (curDefaultDownload.isEmpty()) curDefaultDownload = defaultDownloadDirPath();
    curDefaultDownload = QDir::cleanPath(curDefaultDownload);
    const bool curDeleteSecrets = deleteSecretsOnRemove_ && deleteSecretsOnRemove_->isChecked();
    const bool curKnownHashed = knownHostsHashed_ && knownHostsHashed_->isChecked();
    const bool curFpHex = fpHex_ && fpHex_->isChecked();
    const int curNoHostVerifyTtlMin = noHostVerifyTtlMinSpin_ ? noHostVerifyTtlMinSpin_->value() : noHostVerifyTtlMin;
// Current value only when applicable
#if !defined(__APPLE__) && !defined(Q_OS_MAC) && !defined(Q_OS_MACOS) && !defined(HAVE_LIBSECRET) && !defined(OPEN_SCP_BUILD_SECURE_ONLY)
    const bool curInsecureFb = insecureFallback_ && insecureFallback_->isChecked();
#endif
    const int curMaxConcurrent = maxConcurrentSpin_ ? maxConcurrentSpin_->value() : maxConcurrent;
    const int curGlobalSpeedDefault = globalSpeedDefaultSpin_ ? globalSpeedDefaultSpin_->value() : globalSpeedDefault;
    const QString curStagingRoot = stagingRootEdit_ ? stagingRootEdit_->text() : stagingRoot;
    const bool curAutoCleanSt = autoCleanStaging_ && autoCleanStaging_->isChecked();
    const int curStagingPrepTimeoutMs = stagingPrepTimeoutMsSpin_ ? stagingPrepTimeoutMsSpin_->value() : stagingPrepTimeoutMs;
    const int curStagingConfirmItems = stagingConfirmItemsSpin_ ? stagingConfirmItemsSpin_->value() : stagingConfirmItems;
    const int curStagingConfirmMiB = stagingConfirmMiBSpin_ ? stagingConfirmMiBSpin_->value() : stagingConfirmMiB;
    const int  curMaxDepth   = maxDepthSpin_ ? maxDepthSpin_->value() : maxDepthPrev;

    const bool modified = (curLang != prevLang) ||
                          (curShowHidden != showHidden) ||
                          (curShowConn != showConnOnStart) || (curShowConnDisc != onDisc) ||
                          (curSingleClick != singleClick) ||
                          (curOpenMode != openMode) ||
                          (curShowQueue != showQueue) ||
                          (curDefaultDownload != defaultDownload) ||
                          (curDeleteSecrets != deleteSecrets) ||
                          (curKnownHashed != knownHashed) ||
                          (curFpHex != fpHex) ||
                          (curNoHostVerifyTtlMin != noHostVerifyTtlMin)
#if !defined(__APPLE__) && !defined(Q_OS_MAC) && !defined(Q_OS_MACOS) && !defined(HAVE_LIBSECRET) && !defined(OPEN_SCP_BUILD_SECURE_ONLY)
                          || (curInsecureFb != insecureFb)
#endif
                          || (curMaxConcurrent != maxConcurrent)
                          || (curGlobalSpeedDefault != globalSpeedDefault)
                          || (curStagingRoot != stagingRoot)
                          || (curAutoCleanSt != autoCleanSt)
                          || (curStagingPrepTimeoutMs != stagingPrepTimeoutMs)
                          || (curStagingConfirmItems != stagingConfirmItems)
                          || (curStagingConfirmMiB != stagingConfirmMiB)
                          || (curMaxDepth != maxDepthPrev)
                          ;
    if (applyBtn_) {
        applyBtn_->setEnabled(modified);
        applyBtn_->setDefault(modified);
    }
}
