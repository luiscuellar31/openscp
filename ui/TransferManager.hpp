// Gestor de cola de transferencias (secuenciales) con pausa/reintento/reanudación.
#pragma once
#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <thread>
#include <optional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "openscp/SftpTypes.hpp"

namespace openscp { class SftpClient; }

// Elemento de cola de transferencias.
// Representa una operación de subida o descarga con su estado y opciones.
struct TransferTask {
    enum class Type { Upload, Download } type;
    quint64 id = 0;  // identificador estable para actualizaciones cross-thread
    QString src;     // local para subidas, remoto para descargas
    QString dst;     // remoto para subidas, local para descargas
    bool resumeHint = false;    // si true, intentar reanudar en el próximo intento
    int speedLimitKBps = 0;     // 0 = sin límite; KB/s
    int progress = 0;           // 0..100
    int attempts = 0;
    int maxAttempts = 3;
    // Estado de la tarea:
    //  - Queued: en cola, pendiente de ejecución
    //  - Running: en progreso
    //  - Paused: pausada por el usuario
    //  - Done: completada con éxito
    //  - Error: terminó con error
    //  - Canceled: cancelada por el usuario
    enum class Status { Queued, Running, Paused, Done, Error, Canceled } status = Status::Queued;
    QString error;
};

class TransferManager : public QObject {
    Q_OBJECT
public:
    explicit TransferManager(QObject* parent = nullptr);
    ~TransferManager();

    // Inyecta el cliente SFTP a usar (no es propiedad del manager)
    void setClient(openscp::SftpClient* c) { client_ = c; }
    void clearClient();
    // Opciones de sesión para reconexión automática
    void setSessionOptions(const openscp::SessionOptions& opt) { sessionOpt_ = opt; }
    // Concurrencia: número máximo de tareas simultáneas
    void setMaxConcurrent(int n) { if (n < 1) n = 1; maxConcurrent_ = n; }
    int maxConcurrent() const { return maxConcurrent_; }
    // Límite de velocidad global (KB/s). 0 = sin límite
    void setGlobalSpeedLimitKBps(int kbps) { globalSpeedKBps_.store(kbps); }
    int globalSpeedLimitKBps() const { return globalSpeedKBps_.load(); }

    // Pausa/Reanuda por tarea
    void pauseTask(quint64 id);
    void resumeTask(quint64 id);
    // Cancela una tarea (pasa a estado Canceled)
    void cancelTask(quint64 id);
    // Cancela todas las tareas activas o en cola
    void cancelAll();
    // Ajusta límite de velocidad por tarea (KB/s). 0 = sin límite
    void setTaskSpeedLimit(quint64 id, int kbps);

    void enqueueUpload(const QString& local, const QString& remote);
    void enqueueDownload(const QString& remote, const QString& local);

    const QVector<TransferTask>& tasks() const { return tasks_; }

    // Pausa/Reanuda toda la cola
    void pauseAll();
    void resumeAll();
    void retryFailed();
    void clearCompleted();

signals:
    // Emitida cuando cambia el estado/lista de tareas (para refrescar la UI)
    void tasksChanged();

public slots:
    void processNext(); // procesa en orden; una a la vez
    void schedule();    // intenta lanzar hasta maxConcurrent

private:
    openscp::SftpClient* client_ = nullptr; // no es propiedad del manager
    QVector<TransferTask> tasks_;
    std::atomic<bool> paused_{false};
    std::atomic<int> running_{0};
    int maxConcurrent_ = 2;
    std::atomic<int> globalSpeedKBps_{0};

    // worker threads por tarea
    std::unordered_map<quint64, std::thread> workers_;
    // Estados auxiliares: ids pausados/cancelados para cooperación en worker
    std::unordered_set<quint64> pausedTasks_;
    std::unordered_set<quint64> canceledTasks_;
    // sincronización
    mutable std::mutex mtx_;   // protege tasks_ y sets auxiliares
    std::mutex sftpMutex_;     // serializa llamadas a libssh2 (no thread-safe)
    quint64 nextId_ = 1;

    int indexForId(quint64 id) const;
    // Reconecta el cliente si está desconectado (con backoff). Devuelve true si quedó conectado.
    bool ensureConnected(std::string& err);
    std::optional<openscp::SessionOptions> sessionOpt_;
};
