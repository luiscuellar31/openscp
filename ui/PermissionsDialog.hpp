// Diálogo simple para editar permisos (modo POSIX) y marcar aplicación recursiva.
#pragma once
#include <QDialog>

class QCheckBox;

class PermissionsDialog : public QDialog {
    Q_OBJECT
public:
    explicit PermissionsDialog(QWidget* parent = nullptr);
    void setMode(unsigned int mode);
    unsigned int mode() const;
    bool recursive() const;

private:
    QCheckBox *ur_, *uw_, *ux_;
    QCheckBox *gr_, *gw_, *gx_;
    QCheckBox *or_, *ow_, *ox_;
    QCheckBox *recursive_;
};
