#include "openscp/Libssh2SftpClient.hpp"
#include <libssh2.h>
#include <libssh2_sftp.h>

#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

// POSIX sockets
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

namespace openscp {

static bool g_libssh2_inited = false;

Libssh2SftpClient::Libssh2SftpClient() {
  if (!g_libssh2_inited) {
    int rc = libssh2_init(0);
    (void)rc;
    g_libssh2_inited = true;
  }
}

Libssh2SftpClient::~Libssh2SftpClient() {
  disconnect();
}

bool Libssh2SftpClient::tcpConnect(const std::string& host, uint16_t port, std::string& err) {
  struct addrinfo hints{};
  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  char portStr[16];
  snprintf(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));

  struct addrinfo* res = nullptr;
  int gai = getaddrinfo(host.c_str(), portStr, &hints, &res);
  if (gai != 0) {
    err = std::string("getaddrinfo: ") + gai_strerror(gai);
    return false;
  }

  int s = -1;
  for (auto rp = res; rp != nullptr; rp = rp->ai_next) {
    s = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (s == -1) continue;
    if (::connect(s, rp->ai_addr, rp->ai_addrlen) == 0) {
      // conectado
      sock_ = s;
      freeaddrinfo(res);
      return true;
    }
    ::close(s);
    s = -1;
  }
  freeaddrinfo(res);
  err = "No se pudo conectar al host/puerto.";
  return false;
}

bool Libssh2SftpClient::sshHandshakeAuth(const SessionOptions& opt, std::string& err) {
  session_ = libssh2_session_init();
  if (!session_) { err = "libssh2_session_init falló"; return false; }

  // Handshake
  if (libssh2_session_handshake(session_, sock_) != 0) {
    err = "SSH handshake falló";
    return false;
  }

  // (Opcional futuro) Validación de host key / known_hosts
  // Por ahora: omitido (v0.3.0). IMPORTANTE: añadir en v0.3.x.

  // Autenticación
  // Preferimos clave si está presente; si no, password.
  if (opt.private_key_path.has_value()) {
    const char* passphrase = opt.private_key_passphrase ? opt.private_key_passphrase->c_str() : nullptr;
    int rc = libssh2_userauth_publickey_fromfile(session_,
          opt.username.c_str(),
          NULL,  // public key path (NULL: se deriva de la privada)
          opt.private_key_path->c_str(),
          passphrase);
    if (rc != 0) {
      err = "Auth por clave falló";
      return false;
    }
  } else if (opt.password.has_value()) {
    int rc = libssh2_userauth_password(session_,
          opt.username.c_str(),
          opt.password->c_str());
    if (rc != 0) {
      err = "Auth por password falló";
      return false;
    }
  } else {
    err = "Sin credenciales: provee password o ruta de clave privada.";
    return false;
  }

  // SFTP init
  sftp_ = libssh2_sftp_init(session_);
  if (!sftp_) {
    err = "No se pudo inicializar SFTP";
    return false;
  }

  return true;
}

bool Libssh2SftpClient::connect(const SessionOptions& opt, std::string& err) {
  if (connected_) { err = "Ya conectado"; return false; }
  if (!tcpConnect(opt.host, opt.port, err)) return false;
  if (!sshHandshakeAuth(opt, err)) return false;

  connected_ = true;
  return true;
}

void Libssh2SftpClient::disconnect() {
  if (sftp_) {
    libssh2_sftp_shutdown(sftp_);
    sftp_ = nullptr;
  }
  if (session_) {
    libssh2_session_disconnect(session_, "bye");
    libssh2_session_free(session_);
    session_ = nullptr;
  }
  if (sock_ != -1) {
    ::close(sock_);
    sock_ = -1;
  }
  connected_ = false;
}

bool Libssh2SftpClient::list(const std::string& remote_path,
                             std::vector<FileInfo>& out,
                             std::string& err) {
  if (!connected_ || !sftp_) { err = "No conectado"; return false; }

  std::string path = remote_path.empty() ? "/" : remote_path;

  LIBSSH2_SFTP_HANDLE* dir = libssh2_sftp_opendir(sftp_, path.c_str());
  if (!dir) {
    err = "sftp_opendir falló para: " + path;
    return false;
  }

  out.clear();
  out.reserve(64);

  char filename[512];
  char longentry[1024];
  LIBSSH2_SFTP_ATTRIBUTES attrs;

  while (true) {
    memset(&attrs, 0, sizeof(attrs));
    int rc = libssh2_sftp_readdir_ex(dir,
              filename, sizeof(filename),
              longentry, sizeof(longentry),
              &attrs);
    if (rc > 0) {
      // rc = nombre_len
      FileInfo fi{};
      fi.name = std::string(filename, rc);
      fi.is_dir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) ?
                  ((attrs.permissions & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFDIR) : false;
      if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)  fi.size  = attrs.filesize;
      if (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) fi.mtime = attrs.mtime;
      if (fi.name == "." || fi.name == "..") continue;
      out.push_back(std::move(fi));
    } else if (rc == 0) {
      // fin de directorio
      break;
    } else {
      err = "sftp_readdir_ex falló";
      libssh2_sftp_closedir(dir);
      return false;
    }
  }

  libssh2_sftp_closedir(dir);
  return true;
}

bool Libssh2SftpClient::get(const std::string& remote,
                            const std::string& local,
                            std::string& err,
                            std::function<void(std::size_t,std::size_t)> progress) {
  if (!connected_ || !sftp_) { err = "No conectado"; return false; }

  // Tamaño remoto (para progreso)
  LIBSSH2_SFTP_ATTRIBUTES st{};
  if (libssh2_sftp_stat_ex(sftp_, remote.c_str(), (unsigned)remote.size(),
                           LIBSSH2_SFTP_STAT, &st) != 0) {
    err = "No se pudo obtener stat remoto";
    return false;
  }
  std::size_t total = (st.flags & LIBSSH2_SFTP_ATTR_SIZE) ? (std::size_t)st.filesize : 0;

  // Abrir remoto para lectura
  LIBSSH2_SFTP_HANDLE* rh = libssh2_sftp_open_ex(
      sftp_, remote.c_str(), (unsigned)remote.size(),
      LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
  if (!rh) { err = "No se pudo abrir remoto para lectura"; return false; }

  // Abrir local para escritura (trunc)
  FILE* lf = ::fopen(local.c_str(), "wb");
  if (!lf) {
    libssh2_sftp_close(rh);
    err = "No se pudo abrir archivo local para escribir";
    return false;
  }

  const std::size_t CHUNK = 64 * 1024;
  std::vector<char> buf(CHUNK);
  std::size_t done = 0;

  while (true) {
    ssize_t n = libssh2_sftp_read(rh, buf.data(), (size_t)buf.size());
    if (n > 0) {
      if (std::fwrite(buf.data(), 1, (size_t)n, lf) != (size_t)n) {
        err = "Escritura local falló";
        std::fclose(lf);
        libssh2_sftp_close(rh);
        return false;
      }
      done += (std::size_t)n;
      if (progress && total) progress(done, total);
    } else if (n == 0) {
      break; // EOF
    } else {
      err = "Lectura remota falló";
      std::fclose(lf);
      libssh2_sftp_close(rh);
      return false;
    }
  }

  std::fclose(lf);
  libssh2_sftp_close(rh);
  return true;
}

bool Libssh2SftpClient::put(const std::string& local,
                            const std::string& remote,
                            std::string& err,
                            std::function<void(std::size_t,std::size_t)> progress) {
  if (!connected_ || !sftp_) { err = "No conectado"; return false; }

  // Abrir local para lectura
  FILE* lf = ::fopen(local.c_str(), "rb");
  if (!lf) { err = "No se pudo abrir archivo local para lectura"; return false; }

  // Tamaño local
  std::fseek(lf, 0, SEEK_END);
  long fsz = std::ftell(lf);
  std::fseek(lf, 0, SEEK_SET);
  std::size_t total = fsz > 0 ? (std::size_t)fsz : 0;

  // Abrir remoto para escritura (crea/trunca)
  LIBSSH2_SFTP_HANDLE* wh = libssh2_sftp_open_ex(
      sftp_, remote.c_str(), (unsigned)remote.size(),
      LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
      0644, LIBSSH2_SFTP_OPENFILE);
  if (!wh) {
    std::fclose(lf);
    err = "No se pudo abrir remoto para escritura";
    return false;
  }

  const std::size_t CHUNK = 64 * 1024;
  std::vector<char> buf(CHUNK);
  std::size_t done = 0;

  while (true) {
    size_t n = std::fread(buf.data(), 1, buf.size(), lf);
    if (n > 0) {
      char* p = buf.data();
      size_t remain = n;
      while (remain > 0) {
        ssize_t w = libssh2_sftp_write(wh, p, remain);
        if (w < 0) {
          err = "Escritura remota falló";
          libssh2_sftp_close(wh);
          std::fclose(lf);
          return false;
        }
        remain -= (size_t)w;
        p += w;
        done += (size_t)w;
        if (progress && total) progress(done, total);
      }
    } else {
      if (std::ferror(lf)) {
        err = "Lectura local falló";
        libssh2_sftp_close(wh);
        std::fclose(lf);
        return false;
      }
      break; // EOF
    }
  }

  libssh2_sftp_close(wh);
  std::fclose(lf);
  return true;
}
} // namespace openscp