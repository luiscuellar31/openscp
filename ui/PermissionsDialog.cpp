// Checkbox UI for user/group/others permissions.
#include "PermissionsDialog.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QSignalBlocker>
#include <QGridLayout>
#include <QDialogButtonBox>
#include <QLabel>

PermissionsDialog::PermissionsDialog(QWidget* parent)
    : QDialog(parent), ur_(nullptr), uw_(nullptr), ux_(nullptr),
      gr_(nullptr), gw_(nullptr), gx_(nullptr), or_(nullptr), ow_(nullptr), ox_(nullptr), recursive_(nullptr) {
    setWindowTitle(tr("Cambiar permisos"));
    auto* lay = new QGridLayout(this);
    lay->addWidget(new QLabel(tr("Usuario")), 0, 1);
    lay->addWidget(new QLabel(tr("Grupo")), 0, 2);
    lay->addWidget(new QLabel(tr("Otros")), 0, 3);

    lay->addWidget(new QLabel(tr("Leer")), 1, 0);
    ur_ = new QCheckBox(this); gr_ = new QCheckBox(this); or_ = new QCheckBox(this);
    lay->addWidget(ur_, 1, 1); lay->addWidget(gr_, 1, 2); lay->addWidget(or_, 1, 3);

    lay->addWidget(new QLabel(tr("Escribir")), 2, 0);
    uw_ = new QCheckBox(this); gw_ = new QCheckBox(this); ow_ = new QCheckBox(this);
    lay->addWidget(uw_, 2, 1); lay->addWidget(gw_, 2, 2); lay->addWidget(ow_, 2, 3);

    lay->addWidget(new QLabel(tr("Ejecutar")), 3, 0);
    ux_ = new QCheckBox(this); gx_ = new QCheckBox(this); ox_ = new QCheckBox(this);
    lay->addWidget(ux_, 3, 1); lay->addWidget(gx_, 3, 2); lay->addWidget(ox_, 3, 3);

    lay->addWidget(new QLabel(tr("Preset")), 4, 0);
    presets_ = new QComboBox(this);
    presets_->addItem(tr("Personalizado"), -1);
    presets_->addItem(tr("Archivo (644)"), 0644);
    presets_->addItem(tr("Archivo ejecutable (755)"), 0755);
    presets_->addItem(tr("Privado (600)"), 0600);
    presets_->addItem(tr("Directorio privado (700)"), 0700);
    presets_->addItem(tr("Compartido (664)"), 0664);
    presets_->addItem(tr("Directorio compartido (775)"), 0775);
    lay->addWidget(presets_, 4, 1, 1, 3);

    octalPreview_ = new QLabel(this);
    lay->addWidget(octalPreview_, 5, 0, 1, 4);

    recursive_ = new QCheckBox(tr("Aplicar recursivo a subcarpetas"), this);
    lay->addWidget(recursive_, 6, 0, 1, 4);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(bb, 7, 0, 1, 4);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto onFlagsChanged = [this](bool) { updateOctalPreviewAndPreset(); };
    connect(ur_, &QCheckBox::toggled, this, onFlagsChanged);
    connect(uw_, &QCheckBox::toggled, this, onFlagsChanged);
    connect(ux_, &QCheckBox::toggled, this, onFlagsChanged);
    connect(gr_, &QCheckBox::toggled, this, onFlagsChanged);
    connect(gw_, &QCheckBox::toggled, this, onFlagsChanged);
    connect(gx_, &QCheckBox::toggled, this, onFlagsChanged);
    connect(or_, &QCheckBox::toggled, this, onFlagsChanged);
    connect(ow_, &QCheckBox::toggled, this, onFlagsChanged);
    connect(ox_, &QCheckBox::toggled, this, onFlagsChanged);

    connect(presets_, &QComboBox::currentIndexChanged, this, [this](int) {
        const QVariant v = presets_->currentData();
        if (!v.isValid()) return;
        const int presetMode = v.toInt();
        if (presetMode < 0) return;
        setMode(static_cast<unsigned int>(presetMode));
    });

    updateOctalPreviewAndPreset();
}

void PermissionsDialog::setMode(unsigned int m) {
    const QSignalBlocker b1(ur_), b2(uw_), b3(ux_), b4(gr_), b5(gw_), b6(gx_), b7(or_), b8(ow_), b9(ox_);
    ur_->setChecked(m & 0400); uw_->setChecked(m & 0200); ux_->setChecked(m & 0100);
    gr_->setChecked(m & 0040); gw_->setChecked(m & 0020); gx_->setChecked(m & 0010);
    or_->setChecked(m & 0004); ow_->setChecked(m & 0002); ox_->setChecked(m & 0001);
    updateOctalPreviewAndPreset();
}

unsigned int PermissionsDialog::mode() const {
    unsigned int m = 0;
    if (ur_->isChecked()) m |= 0400; if (uw_->isChecked()) m |= 0200; if (ux_->isChecked()) m |= 0100;
    if (gr_->isChecked()) m |= 0040; if (gw_->isChecked()) m |= 0020; if (gx_->isChecked()) m |= 0010;
    if (or_->isChecked()) m |= 0004; if (ow_->isChecked()) m |= 0002; if (ox_->isChecked()) m |= 0001;
    return m;
}

bool PermissionsDialog::recursive() const {
    return recursive_->isChecked();
}

void PermissionsDialog::updateOctalPreviewAndPreset() {
    const unsigned int m = mode() & 0777;
    if (octalPreview_) {
        octalPreview_->setText(tr("Modo octal: %1").arg(QString("%1").arg(m, 3, 8, QLatin1Char('0'))));
    }
    if (presets_) {
        const int idx = presets_->findData(static_cast<int>(m));
        const QSignalBlocker guard(presets_);
        presets_->setCurrentIndex(idx >= 0 ? idx : 0);
    }
}
