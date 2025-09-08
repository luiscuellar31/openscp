// Diálogo de Configuración: idioma.
#pragma once
#include <QDialog>

class QComboBox;
class QPushButton;
class QCheckBox;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

private slots:
    void accept() override; // guarda y aplica

private:
    QComboBox* langCombo_  = nullptr;   // es/en
    QCheckBox* showHidden_ = nullptr;   // mostrar archivos ocultos
    QComboBox* clickMode_  = nullptr;   // 1 clic vs 2 clics
    QCheckBox* showConnOnStart_ = nullptr; // mostrar gestor de sitios al iniciar y al cerrar última sesión
};
