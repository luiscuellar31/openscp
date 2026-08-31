<div align="center">
  <img src="assets/program/icon-openscp-2048.png" alt="Icono de OpenSCP" width="128">
  <h1>OpenSCP</h1>

  <p><strong>Un cliente claro de doble panel para SFTP, SCP, FTP, FTPS y WebDAV.</strong></p>
  <p><a href="README.md">Read in English</a></p>

  <img src="assets/screenshots/screenshot-main-window.png" alt="Ventana principal de OpenSCP" width="900">
</div>

OpenSCP es una aplicación de escritorio escrita en C++20 y Qt 6 para mover y
administrar archivos entre sistemas locales y remotos. Se enfoca en un
comportamiento predecible, valores seguros y un flujo estilo commander. OpenSCP
está inspirado en WinSCP y busca ofrecer una alternativa ligera y
multiplataforma, enfocada en la simplicidad, la seguridad, la claridad y la
extensibilidad.

## Inicio rápido

OpenSCP soporta actualmente Linux y macOS. Requiere una instalación compatible
de Qt 6, CMake 3.22+, libssh2 y OpenSSL. libcurl y tinyxml2 habilitan los
backends opcionales FTP/FTPS y WebDAV.

```bash
git clone https://github.com/luiscuellar31/openscp.git
cd openscp

# Linux
./scripts/linux.sh dev

# macOS
./scripts/macos.sh dev
```

Consulta [Compilar OpenSCP](docs/BUILDING.md) para dependencias por plataforma,
pasos manuales, empaquetado y solución de problemas.

## Características principales

- Navegación local/remota de doble panel con breadcrumbs, historial, favoritos,
  búsqueda y drag-and-drop.
- SFTP y SCP mediante libssh2; FTP, FTPS y WebDAV opcionales mediante libcurl.
- Cola persistente con workers paralelos, pausa, reanudación, reintentos,
  políticas de conflicto, límites de ancho de banda y archivos `.part`.
- Sitios guardados con Keychain en macOS y Secret Service/libsecret en Linux.
- Verificación SSH estricta, accept-new o deshabilitada explícitamente.
- SOCKS5, HTTP CONNECT y jump host SSH cuando el protocolo lo permite.
- Sincronización unidireccional con vista previa, filtros y checksums opcionales.
- Interfaces en inglés, español, francés y portugués.

Los protocolos disponibles dependen del artefacto. Consulta la
[matriz de protocolos](docs/PLATFORM_COMPATIBILITY.md#protocol-availability-by-build)
antes de elegir un paquete.

## Documentación

- [Compilación y empaquetado](docs/BUILDING.md)
- [Contribuciones y traducciones](CONTRIBUTING.md)
- [Arquitectura](docs/ARCHITECTURE.md)
- [Compatibilidad de plataformas y protocolos](docs/PLATFORM_COMPATIBILITY.md)
- [Política de seguridad](SECURITY.md)
- [Licenciamiento](docs/LICENSING.md)

## Diagnóstico en ejecución

Estas variables opcionales ayudan a diagnosticar problemas:

- `OPENSCP_LOG_LEVEL=off|error|warn|info|debug`
- `OPENSCP_TRANSFER_INTEGRITY=off|optional|required`
- `OPENSCP_KNOWNHOSTS_PLAIN=1|0`
- `OPENSCP_FP_HEX_ONLY=1`
- `OPENSCP_ENV=dev|prod` selecciona el entorno de ejecución
- `OPENSCP_LOG_SENSITIVE=1` permite detalles sensibles de diagnóstico solamente
  junto con `OPENSCP_ENV=dev`
- `OPENSCP_ENABLE_INSECURE_FALLBACK=1` solo cuando el build lo permite

El registro de información sensible está deshabilitado por defecto y solamente
debe utilizarse temporalmente en un entorno de desarrollo controlado.

## Más capturas

<p align="center">
  <img src="assets/screenshots/screenshot-site-manager.png" alt="Sitios guardados" width="32%">
  <img src="assets/screenshots/screenshot-connect.png" alt="Diálogo de conexión" width="32%">
  <img src="assets/screenshots/screenshot-transfer-queue.png" alt="Cola de transferencias" width="32%">
</p>

<p align="center">
  <img src="assets/screenshots/screenshot-history.png" alt="Historial de navegación" width="40%">
  <img src="assets/screenshots/screenshot-settings.png" alt="Ajustes de la aplicación" width="40%">
</p>

## Roadmap

- Completar y validar el port para Windows. El código específico de Windows que
  existe actualmente es infraestructura experimental, no funcionalidad de
  ejecución o lanzamiento soportada.
- Ampliar la cobertura de interoperabilidad WebDAV más allá del entorno de
  servidor probado actualmente.
- Admitir más flujos empresariales de autenticación para proxies y jump hosts
  SSH, incluida la autenticación interactiva y no batch del jump host.
- Añadir opciones de personalización para el usuario, como una paleta de
  comandos y temas seleccionables.

## Lanzamientos y contribuciones

Las versiones etiquetadas se publican en
[GitHub Releases](https://github.com/luiscuellar31/openscp/releases).
`main` contiene trabajo estable y `dev` es el destino de pull requests.

Las contribuciones son bienvenidas. Lee [CONTRIBUTING.md](CONTRIBUTING.md)
antes de abrir un pull request. Reporta vulnerabilidades de forma privada como
indica [SECURITY.md](SECURITY.md).

OpenSCP está disponible bajo GPLv3-only o una licencia comercial. Los
componentes de terceros conservan sus propias licencias; consulta
[Licenciamiento](docs/LICENSING.md) y los
[créditos de terceros](docs/credits/CREDITS.md).
