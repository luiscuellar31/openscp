// Interfaz abstracta para operaciones SFTP. Implementaciones concretas (p.ej. libssh2)
// deben respetar esta API para mantener la UI desacoplada del backend.
#pragma once
#include "SftpTypes.hpp"
#include <functional>

namespace openscp {

class SftpClient {
public:
    using ProgressCB = std::function<void(double)>; // 0..1 (no usado actualmente)

    virtual ~SftpClient() = default;

    // Conectar y desconectar
    virtual bool connect(const SessionOptions& opt, std::string& err) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    // Listado de directorio remoto
    virtual bool list(const std::string& remote_path,
                      std::vector<FileInfo>& out,
                      std::string& err) = 0;

    // Descargar archivo remoto a local; si resume=true intenta continuar parcial
    virtual bool get(const std::string& remote,
                     const std::string& local,
                     std::string& err,
                     std::function<void(std::size_t /*done*/, std::size_t /*total*/)> progress = {},
                     std::function<bool()> shouldCancel = {},
                     bool resume = false) = 0;

    // Subir archivo local a remoto; si resume=true intenta continuar parcial
    virtual bool put(const std::string& local,
                     const std::string& remote,
                     std::string& err,
                     std::function<void(std::size_t /*done*/, std::size_t /*total*/)> progress = {},
                     std::function<bool()> shouldCancel = {},
                     bool resume = false) = 0;

    // Comprobar existencia (deja err vacío si "no existe")
    virtual bool exists(const std::string& remote_path,
                        bool& isDir,
                        std::string& err) = 0;

    // Metadatos detallados (stat). Devuelve true si existe.
    virtual bool stat(const std::string& remote_path,
                      FileInfo& info,
                      std::string& err) = 0;

    // Cambiar permisos (modo POSIX, p.ej. 0644)
    virtual bool chmod(const std::string& remote_path,
                       std::uint32_t mode,
                       std::string& err) = 0;

    // Cambiar propietario/grupo (si el servidor lo permite)
    virtual bool chown(const std::string& remote_path,
                       std::uint32_t uid,
                       std::uint32_t gid,
                       std::string& err) = 0;

    // Ajustar tiempos (atime/mtime) remotos si el servidor lo permite
    virtual bool setTimes(const std::string& remote_path,
                          std::uint64_t atime,
                          std::uint64_t mtime,
                          std::string& err) = 0;

    // Operaciones de archivos/carpetas (lado remoto)
    virtual bool mkdir(const std::string& remote_dir,
                       std::string& err,
                       unsigned int mode = 0755) = 0;

    virtual bool removeFile(const std::string& remote_path,
                            std::string& err) = 0;

    virtual bool removeDir(const std::string& remote_dir,
                           std::string& err) = 0;

    virtual bool rename(const std::string& from,
                        const std::string& to,
                        std::string& err,
                        bool overwrite = false) = 0;

    // Crear una nueva conexión del mismo tipo con opciones dadas.
    virtual std::unique_ptr<SftpClient> newConnectionLike(const SessionOptions& opt,
                                                          std::string& err) = 0;
};

} // namespace openscp
