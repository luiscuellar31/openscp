// Implementación del diálogo de configuración de OpenSCP.
#include "SettingsDialog.hpp"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QSettings>
#include <QMessageBox>
#include <QCheckBox>

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Configuración"));

    auto* root = new QVBoxLayout(this);

    // Idioma
    {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(tr("Idioma:"), this));
        langCombo_ = new QComboBox(this);
        langCombo_->addItem("Español", "es");
        langCombo_->addItem("English", "en");
        row->addWidget(langCombo_, 1);
        root->addLayout(row);
    }

    // Modo de clic (1 o 2)
    {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(tr("Apertura con:"), this));
        clickMode_ = new QComboBox(this);
        clickMode_->addItem(tr("Doble clic"), 2);
        clickMode_->addItem(tr("Un clic"), 1);
        row->addWidget(clickMode_, 1);
        root->addLayout(row);
    }

    // Mostrar archivos ocultos (al final)
    {
        auto* row = new QHBoxLayout();
        showHidden_ = new QCheckBox(tr("Mostrar archivos ocultos"), this);
        row->addWidget(showHidden_);
        row->addStretch();
        root->addLayout(row);
    }

    // Mostrar ventana de conexión al inicio/cierre última sesión
    {
        auto* row = new QHBoxLayout();
        showConnOnStart_ = new QCheckBox(tr("Mostrar ventana de conexión al inicio y cuando se cierre la última sesión."), this);
        row->addWidget(showConnOnStart_);
        row->addStretch();
        root->addLayout(row);
    }

    // Cargar desde QSettings
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

    // Botones
    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btns, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    root->addWidget(btns);
}

void SettingsDialog::accept() {
    const QString chosenLang  = langCombo_->currentData().toString();

    QSettings s("OpenSCP", "OpenSCP");
    const QString prevLang = s.value("UI/language", "es").toString();
    s.setValue("UI/language", chosenLang);
    s.setValue("UI/showHidden", showHidden_ && showHidden_->isChecked());
    s.setValue("UI/showConnOnStart", showConnOnStart_ && showConnOnStart_->isChecked());
    const bool singleClick = (clickMode_ && clickMode_->currentData().toInt() == 1);
    s.setValue("UI/singleClick", singleClick);
    s.sync();

    // Solo avisar si realmente cambió el idioma
    if (prevLang != chosenLang) {
        QMessageBox::information(this, tr("Idioma"), tr("El cambio de idioma se aplicará al reiniciar."));
    }

    QDialog::accept();
}
