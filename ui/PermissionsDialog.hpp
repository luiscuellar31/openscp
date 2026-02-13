// Simple dialog to edit permissions (POSIX mode) and mark recursive
// application.
#pragma once
#include <QDialog>

class QCheckBox;
class QLabel;
class QComboBox;

class PermissionsDialog : public QDialog {
    Q_OBJECT
    public:
    explicit PermissionsDialog(QWidget *parent = nullptr);
    void setMode(unsigned int mode);
    unsigned int mode() const;
    bool recursive() const;

    private:
    void updateOctalPreviewAndPreset();

    QCheckBox *ur_, *uw_, *ux_;
    QCheckBox *gr_, *gw_, *gx_;
    QCheckBox *or_, *ow_, *ox_;
    QCheckBox *recursive_;
    QLabel *octalPreview_ = nullptr;
    QComboBox *presets_ = nullptr;
};
