# OpenSCP (Pre-alpha)

**OpenSCP** es un explorador de archivos estilo *two-panel commander* escrito en **C++/Qt**, con soporte para **SFTP remoto** basado en `libssh2`.

El objetivo del proyecto es ofrecer una alternativa ligera y multiplataforma a herramientas como *WinSCP*, enfocada en la simplicidad y en un código abierto y extensible.

---

## Características (v0.4.0)

- **Exploración en dos paneles**  
  - Panel izquierdo y derecho, navegables de manera independiente.  
  - Cada panel tiene su propia **toolbar** con botón **Arriba** para retroceder al directorio padre.  

- **Operaciones locales**  
  - Copiar (`F5`) y mover (`F6`) recursivo.  
  - Eliminar (`Supr`).  
  - Manejo de conflictos: sobrescribir, omitir, renombrar y aplicar a todos.

- **Soporte SFTP (libssh2)**  
  - Conexión con usuario/contraseña o clave privada.  
  - Validación de `known_hosts` (estricto por defecto).  
  - Navegación de directorios remotos.  
  - Descargar (F7) archivos y carpetas (recursivo) con barra de progreso global, cancelación y resolución de colisiones (sobrescribir/omitir/renombrar/… todo).  
  - Subir (F5) archivos y carpetas (recursivo) con progreso y cancelación.  
  - Crear carpeta, renombrar y borrar (incluye borrado recursivo) en remoto.  

- **Interfaz Qt**  
  - Splitter central ajustable.  
  - Barra de estado con mensajes.  
  - Atajos de teclado para todas las operaciones básicas.  

---

## Roadmap

- [ ] Descarga recursiva con estimación total de tamaño y ETA.  
- [ ] Reintentos, colas avanzadas y “reanudación” (resume).  
- [ ] Vista de permisos/propietarios y edición chmod/chown.  
- [ ] Preferencias: selector/política de `known_hosts` desde UI.  
- [ ] Mejoras de UX (drag & drop, menú contextual).  

---

## Novedades respecto a v0.4.0

Desde la 0.4.0 el proyecto incorporó un gestor de transferencias con cola visible (pausar, reanudar, cancelar y reintentar desde UI), soporte de reanudación por archivo y límites de velocidad globales/por tarea, además de una UX más pulida con arrastrar‑y‑soltar entre paneles, un menú contextual remoto más completo (incluye Descargar, Subir, Renombrar, Borrar, Nueva carpeta y Cambiar permisos) y doble clic para previsualizar archivos remotos descargándolos temporalmente. En SFTP se añadió compatibilidad con keyboard‑interactive (OTP/2FA) y ssh‑agent, junto con validación de huella y política de known_hosts seleccionable (Estricto/TOFU/Off) desde el diálogo de conexión, guardando automáticamente nuevos hosts cuando procede. También se sumó un gestor de sitios con almacenamiento de credenciales en el llavero del sistema en macOS (Keychain) y migración desde configuraciones antiguas; se implementó edición de permisos (chmod) con opción recursiva y una comprobación de escribibilidad del directorio remoto para habilitar o bloquear acciones según permisos. Además, se mejoró el feedback en barra de estado, el ordenado y el ancho de columnas en la vista remota, y se añadió reconexión con backoff durante transferencias para mayor robustez.

---

## Requisitos

- [Qt 6.x](https://www.qt.io/download) (módulos **Core**, **Widgets**, **Gui**)  
- [libssh2](https://www.libssh2.org/)  
- [CMake 3.16+](https://cmake.org/download/)  
- Compilador con soporte de **C++17** o superior

---

## Compilación

### Linux / macOS

```bash
# Clonar el repositorio
git clone https://github.com/tuusuario/OpenSCP-hello.git
cd OpenSCP-hello

# Generar los archivos de build
cmake -S . -B build

# Compilar
cmake --build build

# Ejecutar
./build/openscp_hello
```

## Estado
Este release (v0.4.0) marca una versión temprana usable (pre‑alpha) con transferencias recursivas y validación de known_hosts.
Se recomienda solo para pruebas y retroalimentación temprana.
