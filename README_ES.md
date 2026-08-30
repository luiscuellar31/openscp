<div align="center">
    <img src="assets/program/icon-openscp-2048.png" alt="Icono de OpenSCP" width="128">
    <h1 align="center">OpenSCP</h1>

<p>
    <strong>Cliente SFTP/SCP/FTP/FTPS/WebDAV de doble panel enfocado en simplicidad y seguridad</strong>
</p>

<p>
    <a href="README.md"><strong>Read in English</strong></a>
</p>

<p>
    <strong>OpenSCP</strong> es un explorador de archivos estilo two-panel commander escrito en <strong>C++/Qt</strong>, con soporte <strong>SFTP</strong>, soporte inicial para <strong>SCP</strong>, <strong>FTP/FTPS</strong> y <strong>WebDAV</strong>. Busca ser una alternativa ligera a herramientas como WinSCP, enfocada en <strong>seguridad</strong>, <strong>claridad</strong> y <strong>extensibilidad</strong>.
</p>

<br>

<img src="assets/screenshots/screenshot-main-window.png" alt="Ventana principal de OpenSCP con doble panel y cola de transferencias" width="900">

</div>

## Lanzamientos y Ramas

Versiones estables etiquetadas:
https://github.com/luiscuellar31/openscp/releases

- `main`: rama estable y probada
- `dev`: rama de desarrollo activo (destino de PRs)

## Inicio Rapido

```bash
git clone https://github.com/luiscuellar31/openscp.git
cd openscp
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Linux
./build/openscp_hello

# macOS (flujo recomendado de configurar + compilar + abrir)
./scripts/macos.sh dev
```

## Lo que Ofrece OpenSCP (v1.0.0)

### 1. Flujo de doble panel

- Navegacion independiente local/remoto.
- Navegacion rapida con boton `Home` en las barras de panel (siempre en panel local izquierdo; en el panel derecho usa `HOME` en modo local y fallback a `/` en modo remoto).
- El panel derecho incluye `Open in terminal` en modo remoto para abrir una terminal SSH en la ruta remota actual usando el transporte activo (directo, proxy o jump host); si el shell SSH falla con error de sesion (por ejemplo PTY denegado), hace fallback automatico a `sftp` CLI en la misma terminal. Si ese transporte no se puede reproducir de forma segura, la app muestra un error explicito en lugar de degradar a un SSH directo basico. En `Ajustes > Seguridad` puedes forzar login interactivo (password/keyboard-interactive) y activar/desactivar el fallback automatico a `sftp` CLI para estos comandos.
- Copia y movimiento entre paneles con drag-and-drop.
- Operaciones remotas de contexto: descargar, subir, renombrar, eliminar, nueva carpeta/archivo y permisos.
- Breadcrumbs clicables y busqueda por panel (boton de barra o `Ctrl/Cmd+F`) con patrones wildcard/regex y modo recursivo opcional.
- Cada barra de panel incluye un menu `Favoritos` de apertura inmediata. En
  modo local/local ambos paneles comparten los favoritos locales globales; al
  conectar, el panel derecho cambia a los favoritos remotos del sitio o
  endpoint.
- El panel remoto usa deteccion de iconos por MIME (y proveedor nativo en macOS) para mayor paridad con iconos locales.

### 2. Motor de transferencias y cola

- Transferencias paralelas reales con conexiones aisladas por worker (2 por
  defecto, configurables de 1 a 8 en `Ajustes > Transferencias > Simultáneas`).
- Los prechecks costosos de cola se ejecutan fuera del hilo UI; fairness de scheduling y metricas de cola reducen starvation en alta concurrencia.
- Pausar/reanudar/cancelar/reintentar, limites por tarea/global y soporte de resume.
- Acciones de cola segun estado: los controles solo se habilitan cuando la seleccion/tarea permite la accion (por ejemplo, reintentar en `Error`/`Canceled`, reanudar en `Paused`).
- UI de cola con porcentaje de progreso por fila, filtros y columnas detalladas (`Speed`, `ETA`, `Transferred`, `Error`, etc.).
- Acciones de contexto como reintentar seleccionadas, abrir destino, copiar rutas y politicas de limpieza.
- Persistencia de ventana/layout/filtro de la cola.
- La barra de estado principal muestra avisos de transferencias completadas (subidas/descargas exitosas).
- Las tareas no terminales se persisten sin credenciales y se restauran
  pausadas al reiniciar; las de otro sitio esperan hasta reconectar esa sesion.
- Los lotes recursivos descubren en bloques acotados, aplican backpressure,
  representan carpetas vacias y piden confirmacion al alcanzar los umbrales de
  arboles muy grandes.
- Los conflictos se serializan por lote y permiten preguntar, sobrescribir,
  omitir, reanudar, renombrar o copiar solo si el origen es mas nuevo.
- Descargas y subidas usan archivos `.part` deterministas y finalizacion
  atomica cuando el servidor la soporta; cancelar conserva los datos parciales
  para decidir explicitamente si se reanudan o eliminan.
- Las transferencias usan sesiones de worker interrumpibles y tiempos de espera acotados de lectura/escritura en socket para evitar bloqueos indefinidos cuando la red se estanca.
- El flujo de finalizacion de subidas esta endurecido y las vistas remotas se refrescan de forma confiable al terminar uploads.
- La sesion remota principal se valida de forma periodica y al volver de suspension/bloqueo; si el transporte ya no es valido, OpenSCP se desconecta de forma segura con aviso claro.

### 3. Endurecimiento de seguridad del transporte remoto

- Auth: contrasena, clave privada (+passphrase), keyboard-interactive (OTP/2FA), ssh-agent.
- Selector de protocolo por sitio/sesion (`SFTP`, `SCP`, `FTP`, `FTPS`, `WebDAV`).
- FTP/FTPS soportan listado, fallback de metadata, creacion de carpetas,
  borrado y renombrado, incluidas rutas con espacios y rechazo de inyeccion de
  comandos.
- FTPS soporta modos automatico, TLS explicito y TLS implicito.
- WebDAV incluye listado remoto (`PROPFIND`), ruta base configurable y
  confinada, y operaciones de archivos (`GET`, `PUT`, `MKCOL`, `DELETE`,
  `MOVE`).
- Politica de modo SCP por sitio/sesion: `Automatico` usa subidas SFTP
  temporales seguras con renombrado atomico; `Solo SCP` sigue disponible para
  servidores antiguos, pero escribe directamente en la ruta remota final y,
  por lo tanto, no es atomico.
- La verificacion de certificado FTPS (peer+host) viene activa por defecto, con CA bundle personalizado opcional por sitio/sesion.
- FTP y WebDAV HTTP requieren confirmacion temporal de transporte inseguro;
  desactivar la verificacion TLS exige una confirmacion adicional escribiendo
  `UNSAFE` y deja un aviso visible durante la sesion.
- Politicas de host-key: `Strict`, `Accept new (TOFU)`, `No verification` (endurecida).
- El transporte por sitio puede usar TCP directo, proxy `SOCKS5` o tunel `HTTP CONNECT`.
- Se soporta tunel por sitio via SSH jump host (`ProxyJump`/bastion).
- La implementacion actual trata proxy y jump host como opciones mutuamente excluyentes por sesion.
- Flujo endurecido para no-verificacion: doble confirmacion, excepcion temporal con TTL y banner de riesgo.
- Persistencia atomica de `known_hosts` y permisos POSIX estrictos (`~/.ssh` 0700, archivo 0600).
- Confirmacion explicita de conexion de una sola vez cuando falla persistir huella.
- Cancelacion segura en keyboard-interactive (sin fallback accidental de contrasena).
- Politica de integridad de transferencias (`off/optional|required`) por sitio/sesion (y sobrescritura por variable de entorno) con `.part` + finalize atomico.
- Redaccion de datos sensibles en logs de produccion por defecto.

### 4. Sitios guardados y credenciales

- Sitios guardados con identidad estable por UUID.
- Los sitios guardados persisten por sitio el tipo/endpoint/usuario de proxy.
- Los sitios guardados persisten por sitio configuracion de jump host SSH (host/puerto/usuario/ruta de llave).
- Los sitios guardados persisten por sitio la politica de modo SCP.
- Los sitios guardados persisten por sitio la configuracion FTPS de certificado (toggle de verificacion y ruta de CA bundle opcional).
- Los sitios pueden definir rutas local/remota iniciales y recordar
  opcionalmente las ultimas rutas tras una desconexion correcta.
- El administrador de sitios permite buscar por nombre, protocolo, host o
  usuario y duplicar con un UUID nuevo; copiar credenciales seguras es opcional
  y viene desactivado por defecto.
- Bloqueo de nombres de sitio duplicados.
- Flujos de renombrar/eliminar limpian secretos legacy o huerfanos.
- Eliminacion opcional de credenciales guardadas y entradas relacionadas en `known_hosts` al borrar sitios.
- Backends seguros:
    - macOS: Keychain
    - Linux: libsecret (si esta disponible)
- Las contrasenas de proxy se guardan en backend seguro (nunca en texto plano en ajustes del sitio).
- Feedback claro de persistencia en builds secure-only.
- Quick Connect puede guardar/actualizar datos del sitio sin duplicados.
- Los favoritos locales son globales; favoritos e historial remotos se aislan
  por UUID de sitio o identidad de endpoint. El historial legacy sin alcance
  queda separado y exige confirmacion.

### 5. Calidad de UX/UI

- Dialogo de conexion mejorado (campos mas claros, selectores inline para key/known_hosts, mostrar/ocultar contrasena).
- Dialogo de conexion con configuracion de proxy por sitio (`Direct`, `SOCKS5`, `HTTP CONNECT`) y auth opcional.
- Dialogo de conexion con configuracion opcional de SSH jump host (bastion) por sitio.
- Dialogo de conexion con controles FTPS de certificado (verificacion + selector opcional de CA bundle).
- Selector de idioma de la UI con `Ingles`, `Español`, `Francés` y `Portugués`.
- Ajustes redisenados en secciones enfocadas: `General`, `Transferencias`, `Sitios`, `Seguridad`, `Red` y `Staging y arrastre`.
- Ajustes mantiene los controles visibles al redimensionar (tamano minimo + paginas con scroll).
- Accion de un clic en Ajustes para restaurar layout/tamanos por defecto de la ventana principal.
- Dialogo de permisos con vista octal y presets comunes.
- Dialogo Acerca de con copia de diagnostico y mensajes fallback mas amigables.
- La ventana de cola de transferencias abre centrada respecto a la ventana principal.
- La barra de estado muestra el tipo de conexion activa y el tiempo transcurrido por sesion.
- `Sincronizar` escanea las raices local y remota actuales sin bloquear la UI,
  muestra una vista previa unidireccional Local→Remoto o Remoto→Local y solo
  encola las operaciones seleccionadas explicitamente en esa vista previa.
- La sincronizacion soporta filtros glob y presets reutilizables, conserva por
  defecto los archivos que solo existen en destino y separa el modo espejo
  como una opcion con vista previa y confirmacion explicita de borrados.
- Cuando el backend lo soporta, los pares de archivos seleccionados se pueden
  comparar bajo demanda con SHA-256 sin modificar ninguno de los dos lados.
- El flujo de desconexion se mantiene responsivo: la UI vuelve de inmediato a modo local mientras la limpieza de transferencias puede continuar en segundo plano con watchdog/feedback.
- El reconectar se bloquea mientras la limpieza previa de transferencias siga en curso, evitando solapamientos de sesion.

### 6. Linea base de calidad (CI y tests)

- CI dividido por intencion:
    - push a `dev`: linea base rapida Linux sin integracion, mas el workflow
      dedicado de protocolos reales y sanitizadores
    - PR a `main`: compuertas de integracion Linux/macOS, mas el workflow
      dedicado de protocolos reales y sanitizadores
- Los workflows de integracion levantan servidores temporales SFTP, FTP, FTPS
  explicito, FTPS implicito y WebDAV HTTPS para pruebas end-to-end.
- La compuerta de protocolos curl usa una CA temporal con SAN para
  localhost/127.0.0.1 y sirve WebDAV bajo la ruta base no raiz
  `/openscp-dav`.
- Los binarios de integracion FTP, ambos modos FTPS y WebDAV se ejecutan
  directamente, por lo que un servicio ausente o inaccesible hace fallar CI en
  vez de aparecer como un CTest omitido.
- La cobertura de integracion en PR valida variantes de transporte en CI: directo, tunel proxy `SOCKS5`, tunel proxy `HTTP CONNECT` (con auth) y tunel SSH jump host.
- El workflow de release por tag genera automaticamente notas de draft release desde Conventional Commits (`feat`, `fix`, `BREAKING CHANGE`, etc.).
- Las compuertas de calidad Linux ejecutan los tests no integracion del core y
  del event loop Qt bajo `ASan`+`UBSan` y `TSan`; el workflow nocturno tambien
  ejecuta `cppcheck`, un perfil conservador de `clang-tidy` y una comprobacion
  estricta de `clang-format` para todos los fuentes, headers y archivos `.inc`
  C++ de primera parte rastreados.
- TSan instrumenta OpenSCP y sus tests, pero las bibliotecas Qt precompiladas
  de Ubuntu no vienen instrumentadas con TSan. El job cubre carreras del lado
  de la aplicacion ejercitadas mediante Qt, no carreras completamente internas
  de Qt.

## Requisitos

- Qt `6.x` (probado con `6.8.3`)
- libssh2 (recomendado OpenSSL 3)
- libcurl (opcional; requerido para backends FTP/FTPS/WebDAV)
- tinyxml2 (opcional; requerido para parseo XML del backend WebDAV)
- CMake `3.22+`
- Compilador C++20

Opcional:

- macOS: Keychain (nativo)
- Linux: libsecret / Secret Service
- Cliente OpenSSH (`ssh`) para tunel de jump host SSH.
- El backend FTP/FTPS se puede desactivar explicitamente con
  `-DOPENSCP_ENABLE_FTP_BACKEND=OFF`.
- El backend WebDAV se puede desactivar explicitamente con
  `-DOPENSCP_ENABLE_WEBDAV_BACKEND=OFF`.

## Probar Localmente

```bash
cmake -S . -B build -DOPENSCP_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Script local de CI antes de push/PR:

```bash
./scripts/check_ci_local.sh --clean --full --werror
```

El helper compila todos los ejecutables de prueba configurados para los
backends de protocolo disponibles antes de ejecutar CTest. Agrega `--full`
para compilar tambien la aplicacion grafica.

Si `clang-format` esta instalado, tambien puedes ejecutar localmente la
compuerta global de formato:

```bash
./scripts/check_cpp_quality.sh --format
```

Variantes utiles:

```bash
# Tambien construye el target GUI de la app
./scripts/check_ci_local.sh --clean --full

# Directorio de build personalizado + jobs en paralelo
./scripts/check_ci_local.sh --clean --build-dir build-ci-local -j 8

# Lo mismo via variables de entorno
BUILD_DIR=build-ci-local JOBS=8 ./scripts/check_ci_local.sh --clean
```

Indice de scripts: [scripts/README.md](scripts/README.md)

La separacion interna de capas y las reglas de desarrollo aplicables se
documentan en [Arquitectura](docs/ARCHITECTURE.md) y
[Convenciones de ingenieria](docs/CONVENTIONS.md).

`openscp_sftp_integration_tests` se omite si no defines variables de integracion:

- `OPENSCP_IT_SFTP_HOST`
- `OPENSCP_IT_SFTP_PORT`
- `OPENSCP_IT_SFTP_USER`
- `OPENSCP_IT_SFTP_PASS` o `OPENSCP_IT_SFTP_KEY`
- `OPENSCP_IT_SFTP_KEY_PASSPHRASE` (si aplica)
- `OPENSCP_IT_REMOTE_BASE`
- `OPENSCP_IT_PROXY_TYPE` (`socks5` o `http`, opcional)
- `OPENSCP_IT_PROXY_HOST` (requerido cuando `OPENSCP_IT_PROXY_TYPE` esta definido)
- `OPENSCP_IT_PROXY_PORT` (opcional; por defecto: `1080` para `socks5`, `8080` para `http`)
- `OPENSCP_IT_PROXY_USER` (opcional)
- `OPENSCP_IT_PROXY_PASS` (opcional)
- `OPENSCP_IT_JUMP_HOST` (opcional)
- `OPENSCP_IT_JUMP_PORT` (opcional; por defecto `22`)
- `OPENSCP_IT_JUMP_USER` (opcional)
- `OPENSCP_IT_JUMP_KEY` (opcional)

`openscp_ftp_integration_tests` se omite si no defines variables de integracion:

- `OPENSCP_IT_FTP_HOST`
- `OPENSCP_IT_FTP_PORT` (opcional; por defecto `21`)
- `OPENSCP_IT_FTP_USER` (opcional)
- `OPENSCP_IT_FTP_PASS` (opcional)
- `OPENSCP_IT_FTP_REMOTE_BASE`

`openscp_ftps_integration_tests` se omite si no defines variables de integracion:

- `OPENSCP_IT_FTPS_HOST`
- `OPENSCP_IT_FTPS_PORT` (opcional; por defecto `990`)
- `OPENSCP_IT_FTPS_USER` (opcional)
- `OPENSCP_IT_FTPS_PASS` (opcional)
- `OPENSCP_IT_FTPS_REMOTE_BASE`
- `OPENSCP_IT_FTPS_MODE` (`auto`, `explicit` o `implicit`; opcional)
- `OPENSCP_IT_FTPS_VERIFY_PEER` (`1`/`0`, opcional; por defecto `1`)
- `OPENSCP_IT_FTPS_CA_CERT` (opcional)

`openscp_webdav_integration_tests` se omite si no defines variables de
integracion:

- `OPENSCP_IT_WEBDAV_HOST`
- `OPENSCP_IT_WEBDAV_PORT` (opcional; por defecto `443`)
- `OPENSCP_IT_WEBDAV_USER` (opcional)
- `OPENSCP_IT_WEBDAV_PASS` (opcional)
- `OPENSCP_IT_WEBDAV_REMOTE_BASE`
- `OPENSCP_IT_WEBDAV_SCHEME` (`http` o `https`; opcional)
- `OPENSCP_IT_WEBDAV_BASE_PATH` (opcional; por defecto `/`)
- `OPENSCP_IT_WEBDAV_VERIFY_PEER` (`1`/`0`, opcional; por defecto `1`)
- `OPENSCP_IT_WEBDAV_CA_CERT` (opcional)

La preparacion de servicios Ubuntu para CI y el ejecutor directo estan en
[`scripts/ci`](scripts/ci). Se mantienen separados del manejo de omisiones de
CTest:

```bash
./scripts/ci/setup_protocol_services.sh
./scripts/ci/run_protocol_integration.sh build
```

## Arquitectura de Desarrollo

El build refactorizado separa el codigo de protocolos, el dominio de
sincronizacion, los servicios reutilizables de UI, los widgets visuales y la
raiz de composicion de la aplicacion en targets internos:

- `openscp_core`: interfaces neutrales y backends
  SFTP/SCP/FTP/FTPS/WebDAV.
- `openscp_sync_logic`: tipos de comparacion y motor de sincronizacion
  independiente de widgets.
- `openscp_ui_logic`: sesiones, navegacion, acciones remotas, credenciales,
  descubrimiento recursivo y servicios de transferencias.
- `openscp_ui_widgets`: dialogos y componentes visuales reutilizables.
- `openscp_hello`: `MainWindow`, arranque de la aplicacion y ensamblado de
  recursos.

Las pruebas enlazan estas bibliotecas en vez de recompilar fuentes de
produccion. El target agregado `openscp_test_binaries` compila todos los
ejecutables de prueba configurados. La estructura para contribuidores y las
reglas de formato estan documentadas en [CONTRIBUTING.md](CONTRIBUTING.md).

## Flujos por Plataforma

### macOS

Bucle diario recomendado (tambien es la forma recomendada de abrir un build
local):

```bash
./scripts/macos.sh dev
```

Paso a paso:

```bash
./scripts/macos.sh configure
./scripts/macos.sh build
./scripts/macos.sh run
```

Empaquetado local sin firma:

```bash
./scripts/macos.sh app
./scripts/verify_macos_bundle.sh build/OpenSCP.app

# Otros formatos de artefacto
./scripts/macos.sh pkg
./scripts/macos.sh dmg
./scripts/macos.sh dist
```

El comando `dev` puede abrir la app usando el runtime de desarrollo Qt
detectado. El verificador es para la app ya empaquetada: comprueba que esten los
frameworks Qt y el plugin de plataforma `qcocoa`, y que el enlazado no conserve
rutas de Homebrew, temporales o especificas de la maquina. Este flujo local no
requiere firma ni notarizacion de Apple.

Al empaquetar para macOS 12, todas las bibliotecas de terceros incluidas deben
soportar tambien macOS 12 o una version anterior. Una botella reciente de
Homebrew puede requerir la version mas nueva del host; el linker avisa de esa
diferencia y el verificador rechaza el artefacto. Usa dependencias compiladas
para el deployment target deseado o ajusta `MINIMUM_SYSTEM_VERSION` al minimo
real soportado por el artefacto.

Si Qt esta fuera de la ruta por defecto (`$HOME/Qt/<version>/macos`):

```bash
export QT_PREFIX="/ruta/a/Qt/<version>/macos"
# o
export Qt6_DIR="/ruta/a/Qt/<version>/macos/lib/cmake/Qt6"
```

Detalles completos de empaquetado: [assets/macos/README.md](assets/macos/README.md)

### Linux

Detalles de build y empaquetado Linux (AppImage, Snap, Flatpak): [assets/linux/README.md](assets/linux/README.md)

## Variables de Entorno en Runtime

- `OPENSCP_KNOWNHOSTS_PLAIN=1|0` - fuerza hostnames planos vs hasheados en `known_hosts`.
- `OPENSCP_FP_HEX_ONLY=1` - muestra huellas en HEX con `:`.
- `OPENSCP_TRANSFER_INTEGRITY=off|optional|required` - sobrescribe la politica de integridad de transferencias.
- `OPENSCP_LOG_LEVEL=off|error|warn|info|debug` - ajusta la verbosidad de logs.
- `OPENSCP_ENV=dev|prod` - selector de entorno runtime (`dev` habilita diagnosticos solo de desarrollo).
- `OPENSCP_LOG_SENSITIVE=1` - habilita detalles sensibles de depuracion solo cuando `OPENSCP_ENV=dev` (apagado por defecto).
- `OPENSCP_ENABLE_INSECURE_FALLBACK=1` - habilita fallback inseguro solo cuando el build/plataforma lo soporta.

## Capturas

<p align="center">
    <img src="assets/screenshots/screenshot-site-manager.png" alt="Gestor de sitios con servidores guardados" width="32%">
    <img src="assets/screenshots/screenshot-connect.png" alt="Dialogo de conexion con opciones de autenticacion" width="32%">
    <img src="assets/screenshots/screenshot-transfer-queue.png" alt="Cola de transferencias con progreso, filtros y acciones" width="32%">
    <br>
    <img src="assets/screenshots/screenshot-history.png" alt="Panel de historial de conexiones con sitios recientes" width="32%">
    <img src="assets/screenshots/screenshot-settings.png" alt="Dialogo de ajustes con opciones de seguridad y transferencias" width="32%">
</p>

## Roadmap

- El soporte para Windows esta planeado para futuras versiones.
- Protocolos: ampliar cobertura de interoperabilidad WebDAV.
- Flujos de autenticacion enterprise mas amplios para proxy/jump (por ejemplo, autenticacion jump interactiva fuera de modo batch).
- Mas UX: command palette y temas.

## Creditos y Licencias

- libssh2, libcurl, tinyxml2, OpenSSL, zlib y Qt pertenecen a sus respectivos autores.
- Textos de licencia: [docs/credits/LICENSES/](docs/credits/LICENSES/)
- Materiales Qt (LGPL): [docs/credits](docs/credits)

## Contribuir

- Las contribuciones son bienvenidas. Revisa [CONTRIBUTING.md](CONTRIBUTING.md) para flujo y estandares.
- Issues y pull requests son bienvenidos, especialmente en estabilidad macOS/Linux, i18n y robustez SFTP/SCP/FTP/FTPS/WebDAV.
