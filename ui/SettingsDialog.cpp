// Implementation of OpenSCP settings dialog.
#include "SettingsDialog.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QSettings>
#include <QMessageBox>
#include <QCheckBox>
#include <QToolButton>
#include <QFileDialog>
#include <QLineEdit>
#include <QSpinBox>

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Ajustes"));

    auto* root = new QVBoxLayout(this);

    // Language
    {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(tr("Idioma:"), this));
        langCombo_ = new QComboBox(this);
        langCombo_->addItem("Español", "es");
        langCombo_->addItem("English", "en");
        row->addWidget(langCombo_, 1);
        root->addLayout(row);
    }

    // Click mode (single or double)
    {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(tr("Apertura con:"), this));
        clickMode_ = new QComboBox(this);
        clickMode_->addItem(tr("Doble clic"), 2);
        clickMode_->addItem(tr("Un clic"), 1);
        row->addWidget(clickMode_, 1);
        root->addLayout(row);
    }

    // Show hidden files (at the end)
    {
        auto* row = new QHBoxLayout();
        showHidden_ = new QCheckBox(tr("Mostrar archivos ocultos"), this);
        row->addWidget(showHidden_);
        row->addStretch();
        root->addLayout(row);
    }

    // Show connection window on startup / when the last session closes
    {
        auto* row = new QHBoxLayout();
        showConnOnStart_ = new QCheckBox(tr("Mostrar ventana de conexión al inicio y cuando se cierre la última sesión."), this);
        row->addWidget(showConnOnStart_);
        row->addStretch();
        root->addLayout(row);
    }

    // Open behavior for downloaded/activated files: reveal in folder vs open directly
    {
        auto* row = new QHBoxLayout();
        openInFolder_ = new QCheckBox(tr("Abrir archivos mostrando su carpeta en el sistema (recomendado)."), this);
        row->addWidget(openInFolder_);
        row->addStretch();
        root->addLayout(row);
    }

    // Collapsible Advanced section
    auto* advHeader = new QToolButton(this);
    advHeader->setText(tr("Avanzado"));
    advHeader->setCheckable(true);
    advHeader->setChecked(false);
    advHeader->setArrowType(Qt::RightArrow);
    advHeader->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    root->addWidget(advHeader);

    QWidget* advPanel = new QWidget(this);
    auto* adv = new QVBoxLayout(advPanel);
    adv->setContentsMargins(0,0,0,0);
    adv->setSpacing(root->spacing());
    // Sites: remove stored credentials when deleting a site (optional, default OFF) — first in advanced
    deleteSecretsOnRemove_ = new QCheckBox(tr("Al eliminar un sitio, borrar también sus credenciales guardadas."), advPanel);
    {
        auto* row = new QHBoxLayout();
        row->addWidget(deleteSecretsOnRemove_);
        row->addStretch();
        adv->addLayout(row);
    }
    // known_hosts: save hostnames hashed (recommended)
    knownHostsHashed_ = new QCheckBox(tr("Guardar hostnames en known_hosts como hash (recomendado)."), advPanel);
    {
        auto* row = new QHBoxLayout();
        row->addWidget(knownHostsHashed_);
        row->addStretch();
        adv->addLayout(row);
    }
    // Fingerprint visual: show HEX colon format (visual only)
    fpHex_ = new QCheckBox(tr("Mostrar huella en HEX colonado (solo visual)."), advPanel);
    {
        auto* row = new QHBoxLayout();
        row->addWidget(fpHex_);
        row->addStretch();
        adv->addLayout(row);
    }
#if defined(Q_OS_MAC) || defined(Q_OS_MACOS) || defined(__APPLE__)
    // macOS: Keychain accessibility policy (stricter vs more convenient)
    macKeychainRestrictive_ = new QCheckBox(tr("Usar accesibilidad de llavero más restrictiva (solo en este dispositivo)."), advPanel);
    {
        auto* row = new QHBoxLayout();
        row->addWidget(macKeychainRestrictive_);
        row->addStretch();
        adv->addLayout(row);
    }
#endif
    // Insecure fallback (not recommended): allow QSettings storage
    insecureFallback_ = new QCheckBox(tr("Permitir fallback inseguro de credenciales (no recomendado)."), advPanel);
    {
        auto* row = new QHBoxLayout();
        row->addWidget(insecureFallback_);
        row->addStretch();
        adv->addLayout(row);
    }
    adv->addStretch();
    advPanel->setVisible(false);
    root->addWidget(advPanel);
    connect(advHeader, &QToolButton::toggled, this, [this, advPanel, advHeader](bool on){
        advPanel->setVisible(on);
        advHeader->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        this->adjustSize();
    });
    // Do not persist expansion state across sessions; default collapsed

    // Load from QSettings
    QSettings s("OpenSCP", "OpenSCP");
    const QString lang  = s.value("UI/language", "es").toString();
    const int li = langCombo_->findData(lang);
    if (li >= 0) langCombo_->setCurrentIndex(li);
    const bool showHidden = s.value("UI/showHidden", false).toBool();
    showHidden_->setChecked(showHidden);
    const bool showConnOnStart = s.value("UI/showConnOnStart", true).toBool();
    if (showConnOnStart_) showConnOnStart_->setChecked(showConnOnStart);
    const bool singleClick = s.value("UI/singleClick", false).toBool();
    clickMode_->setCurrentIndex(singleClick ? 1 : 0);
    const bool openInFolder = s.value("UI/openRevealInFolder", false).toBool();
    if (openInFolder_) openInFolder_->setChecked(openInFolder);
    const bool deleteSecrets = s.value("Sites/deleteSecretsOnRemove", false).toBool();
    if (deleteSecretsOnRemove_) deleteSecretsOnRemove_->setChecked(deleteSecrets);
    // Staging path controls
    {
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0,0,0,0);
        row->addWidget(new QLabel(tr("Carpeta de staging:"), advPanel));
        stagingRootEdit_ = new QLineEdit(advPanel);
        stagingBrowseBtn_ = new QPushButton(tr("Elegir…"), advPanel);
        row->addWidget(stagingRootEdit_, 1);
        row->addWidget(stagingBrowseBtn_);
        adv->addLayout(row);
        connect(stagingBrowseBtn_, &QPushButton::clicked, this, [this]{
            const QString cur = stagingRootEdit_ ? stagingRootEdit_->text() : QString();
            const QString pick = QFileDialog::getExistingDirectory(this, tr("Selecciona carpeta de staging"), cur.isEmpty() ? QDir::homePath() : cur);
            if (!pick.isEmpty() && stagingRootEdit_) stagingRootEdit_->setText(pick);
        });
    }
    // Auto clean
    autoCleanStaging_ = new QCheckBox(tr("Eliminar automáticamente la carpeta staging tras completar el arrastre (recomendado)."), advPanel);
    {
        auto* row = new QHBoxLayout();
        row->addWidget(autoCleanStaging_);
        row->addStretch();
        adv->addLayout(row);
    }

    // Maximum folder recursion depth (Advanced/maxFolderDepth)
    {
        auto* row = new QHBoxLayout();
        row->setContentsMargins(0,0,0,0);
        auto* lbl = new QLabel(tr("Profundidad máxima de recursión de carpetas (recomendado: 32)"), advPanel);
        lbl->setToolTip(tr("Límite para arrastre recursivo y evitar árboles muy profundos y bucles."));
        maxDepthSpin_ = new QSpinBox(advPanel);
        maxDepthSpin_->setRange(4, 256);
        maxDepthSpin_->setValue(32);
        maxDepthSpin_->setToolTip(tr("Límite para arrastre recursivo y evitar árboles muy profundos y bucles."));
        row->addWidget(lbl);
        row->addWidget(maxDepthSpin_);
        row->addStretch();
        adv->addLayout(row);
    }

    const bool knownHashed = s.value("Security/knownHostsHashed", true).toBool();
    if (knownHostsHashed_) knownHostsHashed_->setChecked(knownHashed);
    const bool fpHex = s.value("Security/fpHex", false).toBool();
    if (fpHex_) fpHex_->setChecked(fpHex);
#ifndef __APPLE__
    const bool insecureFb = s.value("Security/enableInsecureSecretFallback", false).toBool();
    if (insecureFallback_) insecureFallback_->setChecked(insecureFb);
#endif
    // Load staging settings
    stagingRootEdit_->setText(s.value("Advanced/stagingRoot", QDir::homePath() + "/Downloads/OpenSCP-Dragged").toString());
    autoCleanStaging_->setChecked(s.value("Advanced/autoCleanStaging", true).toBool());
    if (maxDepthSpin_) maxDepthSpin_->setValue(s.value("Advanced/maxFolderDepth", 32).toInt());
#if defined(Q_OS_MAC) || defined(Q_OS_MACOS) || defined(__APPLE__)
    const bool macRestrictiveLoad = s.value("Security/macKeychainRestrictive", false).toBool();
    if (macKeychainRestrictive_) macKeychainRestrictive_->setChecked(macRestrictiveLoad);
#endif
    // Advanced starts collapsed by default
    this->adjustSize();

    // Make the dialog auto-fit contents (shrink/grow when Advanced is collapsed/expanded)
    root->setSizeConstraint(QLayout::SetFixedSize);

    // Buttons row: align to right, order: Close (left) then Apply (right)
    auto* btnRow = new QWidget(this);
    auto* hb = new QHBoxLayout(btnRow);
    hb->setContentsMargins(0,0,0,0);
    hb->addStretch();
    closeBtn_ = new QPushButton(tr("Cerrar"), btnRow);
    applyBtn_ = new QPushButton(tr("Aplicar"), btnRow);
    hb->addWidget(closeBtn_);
    hb->addWidget(applyBtn_);
    root->addWidget(btnRow);

    // Visual priority: Apply is the primary (default) only when enabled.
    applyBtn_->setEnabled(false); // disabled until something changes
    applyBtn_->setAutoDefault(true);
    applyBtn_->setDefault(false);
    closeBtn_->setAutoDefault(false);
    closeBtn_->setDefault(false);
    connect(applyBtn_, &QPushButton::clicked, this, &SettingsDialog::onApply);
    connect(closeBtn_, &QPushButton::clicked, this, &SettingsDialog::reject);

    // Enable Apply when any control differs from persisted values
    connect(langCombo_, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateApplyFromControls);
    connect(clickMode_, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateApplyFromControls);
    connect(showHidden_, &QCheckBox::toggled, this, &SettingsDialog::updateApplyFromControls);
    connect(showConnOnStart_, &QCheckBox::toggled, this, &SettingsDialog::updateApplyFromControls);
    if (openInFolder_) connect(openInFolder_, &QCheckBox::toggled, this, &SettingsDialog::updateApplyFromControls);
    if (deleteSecretsOnRemove_) connect(deleteSecretsOnRemove_, &QCheckBox::toggled, this, &SettingsDialog::updateApplyFromControls);
    if (knownHostsHashed_) connect(knownHostsHashed_, &QCheckBox::toggled, this, &SettingsDialog::updateApplyFromControls);
    if (fpHex_) connect(fpHex_, &QCheckBox::toggled, this, &SettingsDialog::updateApplyFromControls);
    if (stagingRootEdit_) connect(stagingRootEdit_, &QLineEdit::textChanged, this, &SettingsDialog::updateApplyFromControls);
    if (autoCleanStaging_) connect(autoCleanStaging_, &QCheckBox::toggled, this, &SettingsDialog::updateApplyFromControls);
    if (maxDepthSpin_) connect(maxDepthSpin_, qOverload<int>(&QSpinBox::valueChanged), this, &SettingsDialog::updateApplyFromControls);
#if defined(Q_OS_MAC) || defined(Q_OS_MACOS) || defined(__APPLE__)
    if (macKeychainRestrictive_) connect(macKeychainRestrictive_, &QCheckBox::toggled, this, &SettingsDialog::updateApplyFromControls);
#endif
    if (insecureFallback_) {
        connect(insecureFallback_, &QCheckBox::toggled, this, [this](bool on){
            if (on) {
                auto ret = QMessageBox::warning(this,
                                                tr("Activar fallback inseguro"),
                                                tr("Esto almacenará credenciales sin cifrar en el disco usando QSettings.\n"
                                                   "En Linux, se recomienda instalar y usar libsecret/Secret Service para mayor seguridad.\n\n"
                                                   "¿Deseas activar el fallback inseguro igualmente?"),
                                                QMessageBox::Yes | QMessageBox::No,
                                                QMessageBox::No);
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
    const bool singleClick = (clickMode_ && clickMode_->currentData().toInt() == 1);
    s.setValue("UI/singleClick", singleClick);
    if (openInFolder_) s.setValue("UI/openRevealInFolder", openInFolder_->isChecked());
    if (deleteSecretsOnRemove_) s.setValue("Sites/deleteSecretsOnRemove", deleteSecretsOnRemove_->isChecked());
    if (knownHostsHashed_) s.setValue("Security/knownHostsHashed", knownHostsHashed_->isChecked());
    if (fpHex_) s.setValue("Security/fpHex", fpHex_->isChecked());
#if defined(Q_OS_MAC) || defined(Q_OS_MACOS) || defined(__APPLE__)
    if (macKeychainRestrictive_) s.setValue("Security/macKeychainRestrictive", macKeychainRestrictive_->isChecked());
#endif
    if (insecureFallback_) s.setValue("Security/enableInsecureSecretFallback", insecureFallback_->isChecked());
    if (stagingRootEdit_) s.setValue("Advanced/stagingRoot", stagingRootEdit_->text());
    if (autoCleanStaging_) s.setValue("Advanced/autoCleanStaging", autoCleanStaging_->isChecked());
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
    const bool singleClick = s.value("UI/singleClick", false).toBool();
    const bool openInFolder = s.value("UI/openRevealInFolder", false).toBool();
    const bool deleteSecrets = s.value("Sites/deleteSecretsOnRemove", false).toBool();
    const bool knownHashed = s.value("Security/knownHostsHashed", true).toBool();
    const bool fpHex = s.value("Security/fpHex", false).toBool();
#ifndef __APPLE__
    const bool insecureFb = s.value("Security/enableInsecureSecretFallback", false).toBool();
#endif
    const QString stagingRoot = s.value("Advanced/stagingRoot", QDir::homePath() + "/Downloads/OpenSCP-Dragged").toString();
    const bool autoCleanSt = s.value("Advanced/autoCleanStaging", true).toBool();
    const int  maxDepthPrev = s.value("Advanced/maxFolderDepth", 32).toInt();

    const QString curLang = langCombo_ ? langCombo_->currentData().toString() : prevLang;
    const bool curShowHidden = showHidden_ && showHidden_->isChecked();
    const bool curShowConn = showConnOnStart_ && showConnOnStart_->isChecked();
    const bool curSingleClick = (clickMode_ && clickMode_->currentData().toInt() == 1);
    const bool curOpenInFolder = openInFolder_ && openInFolder_->isChecked();
    const bool curDeleteSecrets = deleteSecretsOnRemove_ && deleteSecretsOnRemove_->isChecked();
    const bool curKnownHashed = knownHostsHashed_ && knownHostsHashed_->isChecked();
    const bool curFpHex = fpHex_ && fpHex_->isChecked();
#ifndef __APPLE__
    const bool curInsecureFb = insecureFallback_ && insecureFallback_->isChecked();
#endif
    const QString curStagingRoot = stagingRootEdit_ ? stagingRootEdit_->text() : stagingRoot;
    const bool curAutoCleanSt = autoCleanStaging_ && autoCleanStaging_->isChecked();
    const int  curMaxDepth   = maxDepthSpin_ ? maxDepthSpin_->value() : maxDepthPrev;

    const bool modified = (curLang != prevLang) ||
                          (curShowHidden != showHidden) ||
                          (curShowConn != showConnOnStart) ||
                          (curSingleClick != singleClick) ||
                          (curOpenInFolder != openInFolder) ||
                          (curDeleteSecrets != deleteSecrets) ||
                          (curKnownHashed != knownHashed) ||
                          (curFpHex != fpHex)
#ifndef __APPLE__
                          || (curInsecureFb != insecureFb)
#endif
                          || (curStagingRoot != stagingRoot)
                          || (curAutoCleanSt != autoCleanSt)
                          || (curMaxDepth != maxDepthPrev)
                          ;
    if (applyBtn_) {
        applyBtn_->setEnabled(modified);
        applyBtn_->setDefault(modified);
    }
}
