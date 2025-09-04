// Diálogo para visualizar y gestionar la cola de transferencias.
#pragma once
#include <QDialog>
#include <QTableWidget>
#include "TransferManager.hpp"

class QLabel;
class QPushButton;

// Diálogo para monitorear y controlar la cola de transferencias.
// Permite pausar/reanudar, cancelar y limitar la velocidad por tarea.
class TransferQueueDialog : public QDialog {
    Q_OBJECT
public:
    explicit TransferQueueDialog(TransferManager* mgr, QWidget* parent = nullptr);

private slots:
    void refresh();           // refresca la tabla a partir del manager
    void onPause();           // pausa toda la cola
    void onResume();          // reanuda la cola (y tareas pausadas)
    void onRetry();           // reintenta fallidas/canceladas
    void onClearDone();       // limpia completadas
    void onPauseSelected();   // pausa tareas seleccionadas
    void onResumeSelected();  // reanuda tareas seleccionadas
    void onApplyGlobalSpeed();// aplica límite global
    void onLimitSelected();   // limita velocidad a seleccionadas
    void onStopSelected();    // cancela seleccionadas
    void onStopAll();         // cancela toda la cola en progreso

private:
    void updateSummary();

    TransferManager* mgr_;             // fuente de verdad de la cola
    QTableWidget* table_;              // tabla de tareas
    QLabel* summaryLabel_ = nullptr;   // resumen al pie
    QPushButton* pauseBtn_ = nullptr;  // pausa global
    QPushButton* resumeBtn_ = nullptr; // reanuda global
    QPushButton* retryBtn_ = nullptr;  // reintentar
    QPushButton* clearBtn_ = nullptr;  // limpiar completados
    QPushButton* closeBtn_ = nullptr;  // cerrar diálogo
    QPushButton* pauseSelBtn_ = nullptr;   // pausa sel.
    QPushButton* resumeSelBtn_ = nullptr;  // reanuda sel.
    QPushButton* limitSelBtn_ = nullptr;   // limitar sel.
    QPushButton* stopSelBtn_ = nullptr;    // cancelar sel.
    QPushButton* stopAllBtn_ = nullptr;    // cancelar global
    class QSpinBox* speedSpin_ = nullptr;  // valor límite global
    QPushButton* applySpeedBtn_ = nullptr; // aplicar límite global
}; 
