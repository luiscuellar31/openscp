# OpenSCP (Pre-alpha)

**OpenSCP** es un explorador de archivos estilo *two-panel commander* escrito en **C++/Qt**, con soporte **SFTP** (libssh2 + OpenSSL).
Busca ser una alternativa ligera y multiplataforma a herramientas como WinSCP, enfocado en **simplicidad**, **seguridad** y **extensibilidad**.

---

## Características actuales

### Doble panel (local ↔ remoto)

* Panel izquierdo y derecho navegables de forma independiente.
* Arrastrar-y-soltar entre paneles para copiar/mover.
* Menú contextual remoto: Descargar, Subir, Renombrar, Borrar, Nueva carpeta, Cambiar permisos (recursivo).
* Columnas ordenables con ajuste automático de ancho.

### Transferencias

* **Cola visible** con pausar/reanudar/cancelar/reintentar.
* Reanudación por archivo; límites de velocidad (global y por tarea).
* Reconexión automática con backoff.
* Progreso global y por archivo.
* Tiempos: preserva **mtime** remoto al descargar (la UI muestra **hora local**, las operaciones usan **UTC**).
* Tamaños desconocidos: bandera end-to-end; la UI muestra “—” con tooltip “Size: unknown”.

### SFTP (libssh2)

* Autenticación: usuario/contraseña, clave privada (con passphrase), **keyboard-interactive** (OTP/2FA), **ssh-agent**.
* Políticas de verificación de host key:

  * **Estricto**
  * **Aceptar nuevo (TOFU)**
  * **Desactivado**
* **TOFU no modal** (macOS/Linux/Windows): sin `exec()`, sin bloqueos; la ventana “Conectando…” no roba el foco.
* `known_hosts` robusto:

  * Escrituras **atómicas** (POSIX: `mkstemp → fsync → rename`; Windows: `FlushFileBuffers + MoveFileEx`).
  * Permisos estrictos (`~/.ssh` **0700**, archivo **0600**).
  * **Hostnames hasheados** por defecto (OpenSSH/TYPE\_SHA1); opción para texto plano.
  * Huellas por defecto `SHA256:<base64>` (estilo OpenSSH); vista alternativa **HEX con dos puntos**.
* Cripto moderna:

  * Excluye `ssh-dss` y `ssh-rsa` (SHA-1).
  * Usa hostkeys ED25519/ECDSA/RSA-SHA2 y KEX/cifrados modernos (curve25519, chacha20-poly1305, AES-GCM/CTR, HMAC-SHA-2).
* Seguridad TOFU:

  * Si no se puede persistir la huella, requiere confirmación explícita para **“conectar solo esta vez”**.
  * **Auditoría** en `~/.openscp/openscp.auth`: host, algoritmo, huella y estado (`saved|skipped|save_failed|rejected`).
* Limpieza: al eliminar un sitio con “quitar credenciales guardadas”, también se purga su entrada en `known_hosts`.

### Arrastrar-y-soltar remoto → sistema (asíncrono)

* **Asíncrono real**: el drag inicia solo cuando las URLs están preparadas (sin bloquear la UI).
* **Carpetas (recursivo)** con estructura preservada; confirmación si > **500** ítems o > **1 GiB** estimado.
* **Staging** por lote:

  * Raíz por defecto `~/Downloads/OpenSCP-Dragged`.
  * Autolimpieza al finalizar (opcional); limpieza diferida de lotes > **7 días** al arrancar.
  * Si falla o cancelas: **no** inicia el drag; se muestra enlace clicable al lote (no se borra).
* Colisiones y nombres: estilo “`name (1).ext`”, seguro para múltiples extensiones y Unicode **NFC**.
* MIME: `text/uri-list` + `application/x-openscp-staging-batch` (ID de lote).

### Gestor de sitios

* Lista de servidores con credenciales guardadas.
* Preferencias independientes:

  * **Abrir al iniciar** (opcional).
  * **Abrir al desconectar** (opcional).
    Ambas **ON por defecto** y no modales; si hay un modal activo, la apertura se **difiere**.
* Edición cómoda: elipsis solo al pintar; al editar ves el **nombre completo**.
* Keychain (macOS) y Libsecret (Linux) para credenciales; fallback **inseguro** solo con confirmación (banner **rojo** permanente cuando está activo).
  - macOS: la opción de fallback inseguro no se muestra (siempre se usa Llavero).
  - Linux/Windows: el fallback está disponible solo cuando el build no enlaza Libsecret y no se compila con `OPEN_SCP_BUILD_SECURE_ONLY`.

### UX / UI (Qt)

* Reemplazo de varios botones por **iconos** consistentes; menús más limpios y atajos corregidos.
* Diálogos **no modales** y estables en macOS (los nativos **no se mueven/centran** en `QEvent::Show`).
* Tamaño de ventanas estable: nunca se reduce por debajo de `sizeHint()`, respeta `resize()/minimumSize()`.
* i18n: ES/EN actualizados (terminología y puntuación unificadas).

### Configuración / Ajustes

* Panel **Avanzado** plegable (▸/▾).
* Opciones nuevas:

  * `known_hosts`: hasheado por defecto; alternar a texto plano si se necesita.
  * Formato de huella: `SHA256:<base64>` o **HEX con “:”**.
  * Staging: raíz, autolimpieza, límites de profundidad y timeout de preparación (vía QSettings `Advanced/stagingPrepTimeoutMs`, en milisegundos).
  * “Al eliminar un sitio, quitar credenciales guardadas.” (primero en la lista).

### Variables de entorno

* `OPEN_SCP_KNOWNHOSTS_PLAIN=1|0` — Forzar hostnames en texto plano vs. hasheados (por defecto: **hasheado**).
* `OPEN_SCP_FP_HEX_ONLY=1` — Mostrar huellas solo en HEX con “:”.
* `OPEN_SCP_ENABLE_INSECURE_FALLBACK=1` — Habilitar fallback inseguro de secretos cuando lo soporta el build/plataforma (no Apple, sin Libsecret y sin `OPEN_SCP_BUILD_SECURE_ONLY`); muestra **banner rojo** cuando está activo.

---

## Requisitos

* Qt **6.x**
* libssh2 (**OpenSSL 3** recomendado)
* CMake **3.22+**
* Compilador **C++20**

**Opcional**

* macOS: **Keychain** (nativo).
* Linux: **Libsecret/Secret Service** para guardar credenciales.

---

## Compilación

```bash
git clone https://github.com/tuusuario/openscp.git
cd openscp
cmake -S . -B build
cmake --build build
./build/openscp_hello
```
---

## Roadmap (corto / medio plazo)

* Protocolos: **SCP**; plan para **FTP/FTPS/WebDAV**.
* Concurrencia real en la cola (múltiples conexiones en paralelo sin bloqueo global).
* Proxy / Jump host: **SOCKS5**, **HTTP CONNECT**, **ProxyJump**.
* Sincronización: comparar/sincronizar y “mantener actualizado” con filtros/ignorados.
* Persistencia de cola: reanudación tras reinicio; checksums opcionales.
* Mejoras UX: marcadores, historial, paleta de comandos, temas.

---

## Créditos y licencias

* Atribución de terceros iniciada (ver **docs/**).
* libssh2, OpenSSL y Qt son propiedad de sus respectivos autores.

---

## Estado del proyecto

OpenSCP actualmente es utilizable en la mayoría de los casos.
Se agradecen *issues* y *pull requests*, especialmente sobre estabilidad en macOS y Linux, i18n y flujo de arrastrar-y-soltar.
