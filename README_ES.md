<div align="center">
  <img src="assets/program/icon-openscp-2048.png" alt="Icono de OpenSCP" width="128">
  <h1 align="center">OpenSCP</h1>

<p>
  <strong>Cliente SFTP de doble panel enfocado en simplicidad y seguridad</strong>
</p>

<p>
  <a href="README.md"><strong>Read in English</strong></a>
</p>

<p>
  <strong>OpenSCP</strong> es un explorador de archivos estilo <em>two-panel commander</em> escrito en <strong>C++/Qt</strong>, con soporte <strong>SFTP</strong> (libssh2 + OpenSSL). Busca ser una alternativa ligera a herramientas como WinSCP, enfocada en <strong>seguridad</strong>, <strong>claridad</strong> y <strong>extensibilidad</strong>.
</p>

<br>

<img src="assets/screenshots/screenshot-main-window.png" alt="Ventana principal de OpenSCP con doble panel y cola de transferencias" width="900">

</div>

## Lanzamientos

¿Buscas una versión fija/estable? Descarga versiones etiquetadas:
https://github.com/luiscuellar31/openscp/releases

- `main`: código probado y estable (avanza entre lanzamientos)
- `dev`: desarrollo activo (los PR deben apuntar a `dev`)

## Características actuales (v0.7.0)

### Doble panel (local <-> remoto)

- Navegación independiente en ambos lados.
- Arrastrar y soltar entre paneles para flujos de copia/movimiento.
- Acciones de contexto remoto: Descargar, Subir, Renombrar, Eliminar, Nueva carpeta, Cambiar permisos (recursivo).
- Columnas ordenables y redimensionado responsivo.

### Motor de transferencias y cola

- Transferencias concurrentes reales con conexiones aisladas por worker (paralelismo sin un lock global único).
- Soporte de reanudación, pausar/reanudar/cancelar/reintentar, límites de velocidad globales y por tarea.
- UI de cola mejorada con actualizaciones basadas en modelo (sin reconstruir toda la tabla en cada refresh).
- Columnas detalladas por tarea: Tipo, Nombre, Origen, Destino, Estado, Progreso, Transferido, Velocidad, ETA, Intentos, Error.
- Barras de progreso por fila y filtros rápidos: `All / Active / Errors / Completed / Canceled`.
- Acciones de contexto: pausar/reanudar/limitar/cancelar/reintentar seleccionadas, abrir destino, copiar ruta origen/destino, limpiar finalizadas.
- Badges de resumen para total/activas/en ejecución/pausadas/errores/completadas/canceladas + límite global.
- Políticas de limpieza automática para tareas finalizadas (completadas, fallidas/canceladas, o todas tras N minutos).
- Persistencia de geometría de ventana, layout/orden de columnas y filtro activo.

### Arrastrar remoto -> sistema (asíncrono)

- Preparación de drag totalmente asíncrona (sin congelar la UI al preparar URLs/archivos).
- Staging recursivo de carpetas con estructura preservada.
- Umbrales de seguridad para lotes grandes (confirmación por cantidad y tamaño).
- Raíz de staging y limpieza configurables desde Ajustes.
- Nombres robustos ante colisiones (`name (1).ext`) y manejo seguro de Unicode NFC.

### SFTP y endurecimiento de seguridad

- Métodos de autenticación: contraseña, clave privada (passphrase), keyboard-interactive (OTP/2FA), ssh-agent.
- Políticas de host-key:
  - `Strict`
  - `Accept new (TOFU)`
  - `No verification` (endurecida)
- Endurecimiento de `No verification`:
  - doble confirmación
  - excepción temporal por host con TTL
  - banner de riesgo persistente durante la sesión
- Flujo TOFU no modal para mejor respuesta de UI.
- Persistencia atómica de `known_hosts`:
  - POSIX: `mkstemp -> write -> fsync -> rename -> fsync(parent)`
  - En ruta Windows se valida con `FlushFileBuffers` + `MoveFileEx`
- Si falla la persistencia de huella, la conexión de una sola vez requiere confirmación explícita del usuario.
- `known_hosts` usa hostnames hasheados por defecto; modo plano opcional.
- Permisos estrictos en POSIX (`~/.ssh` 0700, archivo 0600).
- Comportamiento keyboard-interactive más seguro: cancelación explícita evita fallback de contraseña.
- Limpieza defensiva en rutas fallidas de `connect()`.
- Verificaciones de integridad de transferencia para reanudar/finalizar (`Off/Optional/Required`), con archivos temporales `.part` y rename final atómico.
- Datos sensibles en logs redactados por defecto; salida sensible solo en modo opt-in.
- Log de auditoría de host-key en `~/.openscp/openscp.auth`.

### Gestor de sitios y manejo de credenciales

- Sitios guardados con identidades estables basadas en UUID (en lugar de nombre únicamente).
- Bloqueo de nombres de sitio duplicados.
- Flujos de renombrar/eliminar limpian secretos legacy/huérfanos.
- Al eliminar un sitio, también puede eliminar credenciales guardadas y entrada relacionada de `known_hosts`.
- Backends seguros de almacenamiento:
  - macOS: Keychain
  - Linux: libsecret (cuando está disponible)
- Builds secure-only reportan estado explícito cuando la persistencia no está disponible.
- Quick Connect puede guardar el sitio automáticamente (y opcionalmente credenciales) sin crear duplicados.

### Mejoras de UX / UI

- Mejoras del diálogo de conexión:
  - sin `host/user` prellenados engañosos
  - placeholders más claros y foco inicial en host
  - composición `host+port` en la misma fila
  - toggles mostrar/ocultar para contraseña y passphrase
  - botones inline para elegir clave privada y `known_hosts`
- Diálogo de Ajustes rediseñado con secciones laterales (`General` y `Advanced`) y grupos avanzados.
- Diálogo de permisos con vista octal y presets comunes (644/755/600/700/664/775).
- Mejoras del diálogo Acerca de:
  - texto fallback más amigable
  - botón para copiar diagnóstico (versión app, Qt, OS, build type, commit, repo)
  - descubrimiento dinámico de rutas docs/licencias
  - botón de licencias habilitado/deshabilitado según disponibilidad

### Línea base de calidad: CI, tests y release

- Workflow CI dividido por intención:
  - push a `dev`: checks rápidos en Linux (build + tests no integración)
  - PR a `main`: compuerta de integración en Linux y macOS
- CI de integración levanta un servidor SFTP local temporal para tests end-to-end.
- Workflow nocturno de calidad incluye:
  - ASan + UBSan
  - TSan
  - análisis estático con `cppcheck`
- Suites de tests incluidas en el repo:
  - tests unitarios/mock del core
  - tests de integración libssh2 (se saltan si no hay entorno de integración)

### Variables de entorno

- `OPEN_SCP_KNOWNHOSTS_PLAIN=1|0` - fuerza hostnames planos vs hasheados en `known_hosts`.
- `OPEN_SCP_FP_HEX_ONLY=1` - muestra huellas en HEX con `:`.
- `OPEN_SCP_TRANSFER_INTEGRITY=off|optional|required` - sobrescribe política de integridad de transferencias.
- `OPEN_SCP_LOG_LEVEL=error|warn|info|debug` - ajusta verbosidad de logs del core.
- `OPEN_SCP_LOG_SENSITIVE=1` - habilita detalles sensibles de depuración (apagado por defecto).
- `OPEN_SCP_ENABLE_INSECURE_FALLBACK=1` - habilita fallback inseguro de secretos solo si el build/plataforma lo permite.

---

## Requisitos

- Qt `6.x` (probado con `6.8.3`)
- libssh2 (recomendado OpenSSL 3)
- CMake `3.22+`
- Compilador C++20

Opcional:

- macOS: Keychain (nativo)
- Linux: libsecret / Secret Service

---

## Compilación

```bash
git clone https://github.com/luiscuellar31/openscp.git
cd openscp
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Binario Linux
./build/openscp_hello
```

### Ejecutar tests localmente

```bash
cmake -S . -B build -DOPEN_SCP_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Nota: `openscp_sftp_integration_tests` puede saltarse localmente si no están definidas las variables de entorno del entorno SFTP de integración.

### Flujo rápido macOS (recomendado)

```bash
# desarrollo diario
./scripts/macos.sh dev

# o paso a paso
./scripts/macos.sh configure
./scripts/macos.sh build
./scripts/macos.sh run

# empaquetado local sin firma
./scripts/macos.sh app   # dist/*.zip (OpenSCP.app comprimida)
./scripts/macos.sh pkg   # dist/*.pkg
./scripts/macos.sh dmg   # dist/*.dmg
./scripts/macos.sh dist  # todo lo anterior
```

Si Qt no está en la ruta por defecto (`$HOME/Qt/<version>/macos`), define una:

```bash
export QT_PREFIX="/ruta/a/Qt/<version>/macos"
# o
export Qt6_DIR="/ruta/a/Qt/<version>/macos/lib/cmake/Qt6"
```

Consulta `assets/macos/README.md` para notas detalladas de empaquetado y notarización.

### Linux (build + AppImage)

Consulta `assets/linux/README.md` para empaquetar AppImage con `scripts/package_appimage.sh`.

---

## Capturas de pantalla

<p align="center">
  <img src="assets/screenshots/screenshot-site-manager.png" alt="Gestor de sitios con servidores guardados" width="32%">
  <img src="assets/screenshots/screenshot-connect.png" alt="Diálogo de conexión con opciones de autenticación" width="32%">
  <img src="assets/screenshots/screenshot-transfer-queue.png" alt="Cola de transferencias con progreso, filtros y acciones" width="32%">
</p>

---

## Roadmap (corto / medio plazo)

- Soporte para Windows planeado para futuras versiones.
- Protocolos: `SCP`, luego `FTP/FTPS/WebDAV`.
- Soporte de proxy/jump: `SOCKS5`, `HTTP CONNECT`, `ProxyJump`.
- Flujos de sincronización: comparar/sincronizar y keep-up-to-date con filtros/ignorados.
- Persistencia de cola entre reinicios.
- Más UX: marcadores, historial, paleta de comandos, temas.

---

## Créditos y licencias

- libssh2, OpenSSL, zlib y Qt pertenecen a sus respectivos autores.
- Textos de licencia: [docs/credits/LICENSES/](docs/credits/LICENSES/)
- Materiales de Qt (LGPL): [docs/credits](docs/credits)

---

## Contribuir

- Las contribuciones son bienvenidas. Lee `CONTRIBUTING.md` para flujo de trabajo, estrategia de ramas y estándares.
- Issues y pull requests son bienvenidos, especialmente en estabilidad macOS/Linux, i18n y robustez SFTP.
