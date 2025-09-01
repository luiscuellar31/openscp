# OpenSCP (Pre-alpha)

**OpenSCP** es un explorador de archivos estilo *two-panel commander* escrito en **C++/Qt**, con soporte para **SFTP remoto** basado en `libssh2`.

El objetivo del proyecto es ofrecer una alternativa ligera y multiplataforma a herramientas como *WinSCP*, enfocada en la simplicidad y en un código abierto y extensible.

---

## Características (v0.3.0)

- **Exploración en dos paneles**  
  - Panel izquierdo y derecho, navegables de manera independiente.  
  - Cada panel tiene su propia **toolbar** con botón **Arriba** para retroceder al directorio padre.  

- **Operaciones locales**  
  - Copiar (`F5`)  
  - Mover (`F6`)  
  - Eliminar (`Supr`)  
  - Manejo de conflictos: sobrescribir, omitir o aplicar a todos.  

- **Soporte SFTP (libssh2)**  
  - Conexión con usuario/contraseña o clave privada.  
  - Navegación de directorios remotos.  
  - **Descarga de archivos** con barra de progreso y apertura automática con la app por defecto.  
  - Selección de carpeta de descarga (se recuerda la última elegida).  

- **Interfaz Qt**  
  - Splitter central ajustable.  
  - Barra de estado con mensajes.  
  - Atajos de teclado para todas las operaciones básicas.  

---

## Roadmap

- [ ] Subida de archivos (local → remoto).  
- [ ] Borrado remoto.  
- [ ] Mostrar permisos, tamaños y fechas en el listado remoto.  
- [ ] Validación de `known_hosts` para seguridad SSH.  
- [ ] Mejoras de UX (drag & drop, menú contextual).  

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
Este release marca la primera versión mínimamente usable (pre-alpha).
Se recomienda solo para pruebas y retroalimentación temprana.
