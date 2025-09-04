// UI de checkboxes por permisos de usuario/grupo/otros.
#include "PermissionsDialog.hpp"
#include <QCheckBox>
#include <QGridLayout>
#include <QDialogButtonBox>
#include <QLabel>

PermissionsDialog::PermissionsDialog(QWidget* parent)
    : QDialog(parent), ur_(nullptr), uw_(nullptr), ux_(nullptr),
      gr_(nullptr), gw_(nullptr), gx_(nullptr), or_(nullptr), ow_(nullptr), ox_(nullptr), recursive_(nullptr) {
    setWindowTitle("Cambiar permisos");
    auto* lay = new QGridLayout(this);
    lay->addWidget(new QLabel("Usuario"), 0, 1);
    lay->addWidget(new QLabel("Grupo"), 0, 2);
    lay->addWidget(new QLabel("Otros"), 0, 3);

    lay->addWidget(new QLabel("Leer"), 1, 0);
    ur_ = new QCheckBox(this); gr_ = new QCheckBox(this); or_ = new QCheckBox(this);
    lay->addWidget(ur_, 1, 1); lay->addWidget(gr_, 1, 2); lay->addWidget(or_, 1, 3);

    lay->addWidget(new QLabel("Escribir"), 2, 0);
    uw_ = new QCheckBox(this); gw_ = new QCheckBox(this); ow_ = new QCheckBox(this);
    lay->addWidget(uw_, 2, 1); lay->addWidget(gw_, 2, 2); lay->addWidget(ow_, 2, 3);

    lay->addWidget(new QLabel("Ejecutar"), 3, 0);
    ux_ = new QCheckBox(this); gx_ = new QCheckBox(this); ox_ = new QCheckBox(this);
    lay->addWidget(ux_, 3, 1); lay->addWidget(gx_, 3, 2); lay->addWidget(ox_, 3, 3);

    recursive_ = new QCheckBox("Aplicar recursivo a subcarpetas", this);
    lay->addWidget(recursive_, 4, 0, 1, 4);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(bb, 5, 0, 1, 4);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void PermissionsDialog::setMode(unsigned int m) {
    ur_->setChecked(m & 0400); uw_->setChecked(m & 0200); ux_->setChecked(m & 0100);
    gr_->setChecked(m & 0040); gw_->setChecked(m & 0020); gx_->setChecked(m & 0010);
    or_->setChecked(m & 0004); ow_->setChecked(m & 0002); ox_->setChecked(m & 0001);
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