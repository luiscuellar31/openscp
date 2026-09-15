<div align="center">
  <img src="assets/icons/app-openscp.png" alt="Icono de OpenSCP" width="128">
  <h1>OpenSCP</h1>

  <p><strong>Un cliente ligero y multiplataforma de transferencia de archivos inspirado en WinSCP.</strong></p>
  <p><a href="README.md">Read in English</a></p>

  <img src="assets/screenshots/screenshot-main-window.png" alt="Ventana principal de OpenSCP" width="900">
</div>

OpenSCP es una aplicación de escritorio escrita en C++20 y Qt 6 para mover y
administrar archivos entre sistemas locales y remotos. Prioriza un
comportamiento predecible, configuraciones seguras y una interfaz familiar de
doble panel.

## Descarga

Los binarios precompilados de cada versión etiquetada están en la
[página de Releases](https://github.com/luiscuellar31/openscp/releases/latest).

| Plataforma | Archivo |
| --- | --- |
| macOS, Apple Silicon | `OpenSCP-<versión>-arm64-UNSIGNED.dmg` |
| macOS, Intel | `OpenSCP-<versión>-x86_64-UNSIGNED.dmg` |
| Linux, x86_64 | `OpenSCP-<versión>-x86_64.AppImage` |
| Linux, ARM64 | `OpenSCP-<versión>-aarch64.AppImage` |
| Linux, paquete Flatpak | `OpenSCP-x86_64.flatpak`, `OpenSCP-aarch64.flatpak` |

Cada versión incluye un `SHA256SUMS.txt` que cubre todos esos archivos.
Descárgalo junto al que hayas elegido y compruébalos juntos:

```bash
shasum -a 256 -c SHA256SUMS.txt --ignore-missing  # macOS
sha256sum -c SHA256SUMS.txt --ignore-missing      # Linux
```

### Instalación en macOS

Abre el DMG, arrastra OpenSCP a la carpeta Aplicaciones y ábrelo desde ahí.

La primera apertura queda bloqueada: macOS indica que no puede comprobar si la
aplicación contiene software malicioso. Esa comprobación está reservada al
software firmado con un Apple Developer ID de pago, y OpenSCP no está inscrito
en ese programa, así que todas sus compilaciones reciben el mismo aviso
independientemente de su contenido. La suma de verificación publicada es lo que
te permite validar la descarga.

Para permitirla, abre **Ajustes del Sistema → Privacidad y seguridad**, baja
hasta la sección Seguridad, pulsa **Abrir igualmente** junto al mensaje sobre
OpenSCP y confirma. A partir de ahí OpenSCP se abre con normalidad, también
después de reiniciar. En macOS 12 y 13 el mismo ajuste está en **Preferencias
del Sistema → Seguridad y privacidad → General**.

El equivalente desde la terminal:

```bash
xattr -dr com.apple.quarantine /Applications/OpenSCP.app
```

Dos consecuencias que conviene conocer: en macOS 15 y posteriores el antiguo
atajo de Control-clic → Abrir ya no funciona, por lo que la ruta de Privacidad
y seguridad es la única disponible en la interfaz; y como las compilaciones sin
firmar no tienen una identidad estable, macOS vuelve a pedir permiso de llavero
después de cada actualización cuando OpenSCP lee tus contraseñas guardadas.

### Instalación en Linux

El AppImage no requiere instalación y funciona en la mayoría de distribuciones:

```bash
chmod +x OpenSCP-<versión>-x86_64.AppImage
./OpenSCP-<versión>-x86_64.AppImage
```

El paquete Flatpak se integra con el escritorio y se ejecuta confinado:

```bash
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user ./OpenSCP-x86_64.flatpak
flatpak run io.github.luiscuellar31.openscp
```

El remoto de Flathub es necesario porque el paquete depende del entorno de
ejecución de KDE.

## Compilar desde el código fuente

OpenSCP es compatible actualmente con Linux y macOS. Requiere Qt 6, CMake
3.22+, libssh2 y OpenSSL. libcurl habilita FTP y FTPS; WebDAV requiere libcurl
y tinyxml2.

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

- Navegación local y remota de doble panel con rutas planas y clicables, diálogo
  para abrir directorios, historial, favoritos, búsqueda y la posibilidad de
  arrastrar y soltar.
- SFTP y SCP mediante libssh2; FTP, FTPS y WebDAV opcionales mediante libcurl.
- Cola persistente con transferencias en paralelo, pausa, reanudación,
  reintentos, políticas de conflicto, límites de ancho de banda y archivos
  `.part`.
- Sitios guardados con Keychain en macOS y Secret Service/libsecret en Linux.
- Verificación estricta de claves SSH, con aceptación de claves nuevas o
  deshabilitada explícitamente.
- Proxies SOCKS5 y HTTP CONNECT, además de servidores de salto SSH cuando el
  protocolo lo permite.
- Sincronización unidireccional con vista previa, filtros y sumas de
  comprobación opcionales.
- Interfaces en español, inglés, portugués, francés y alemán.

## Navegación por rutas

Cada panel presenta la ubicación actual como un campo de ruta convencional. Haz
clic en cualquier directorio superior dentro de la ruta para abrirlo, o en el
directorio actual para mostrar el diálogo **Abrir directorio**. También puedes
usar `Ctrl+L` (`Cmd+L` también está disponible en macOS) o el atajo Abrir de
la plataforma: `Ctrl+O` en Linux y `Cmd+O` en macOS.

El diálogo permite escribir una ruta directamente y consultar las rutas
recientes y los favoritos del panel local o de la sesión remota seleccionada.
Consulta [Atajos de teclado](docs/KEYBOARD_SHORTCUTS.md) para conocer todos los
atajos y cuándo dependen del panel activo.

Los protocolos disponibles dependen del paquete. Consulta la
[matriz de protocolos](docs/PLATFORM_COMPATIBILITY.md#protocol-availability-by-build)
antes de elegir un paquete.

## Documentación

- [Compilación y empaquetado](docs/BUILDING.md)
- [Atajos de teclado](docs/KEYBOARD_SHORTCUTS.md)
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
- `OPENSCP_ENABLE_INSECURE_FALLBACK=1` solo cuando la compilación lo permite

Los registros sensibles están deshabilitados por defecto y solo deberían
activarse temporalmente en un entorno de desarrollo controlado.

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

- Completar y validar la compatibilidad con Windows; el código actual todavía
  es experimental.
- Probar WebDAV con una mayor variedad de servidores.
- Ampliar la autenticación interactiva y las configuraciones empresariales de
  proxies y servidores de salto SSH.
- Añadir una paleta de comandos y temas seleccionables.

## Lanzamientos y contribuciones

Las versiones publicadas están disponibles en
[GitHub Releases](https://github.com/luiscuellar31/openscp/releases).
`main` contiene el trabajo estable y `dev` recibe los pull requests.

Las contribuciones son bienvenidas. Lee [CONTRIBUTING.md](CONTRIBUTING.md)
antes de abrir un pull request. Reporta vulnerabilidades de forma privada como
indica [SECURITY.md](SECURITY.md).

OpenSCP está disponible bajo GPLv3-only o una licencia comercial. Los
componentes de terceros conservan sus propias licencias; consulta
[Licenciamiento](docs/LICENSING.md) y los
[créditos de terceros](docs/credits/CREDITS.md).
