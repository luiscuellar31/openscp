# OpenSCP (Pre-alpha)

**OpenSCP** es un explorador de archivos estilo *two-panel commander* escrito en **C++/Qt**, con soporte para **SFTP remoto** basado en `libssh2`.

El objetivo del proyecto es crear una alternativa ligera y multiplataforma a herramientas como WinSCP, enfocada en la simplicidad y en un código abierto y extensible.

---

## ✨ Características actuales (v0.4.0)

- **Exploración en dos paneles**  
  - Panel izquierdo y derecho, navegables de manera independiente.  
  - Cada panel tiene su propia barra de herramientas con un botón **“Arriba”** para retroceder al directorio padre.  

- **Operaciones locales (ya implementadas)**  
  - Copiar archivos/carpetas (`F5`).  
  - Mover archivos/carpetas (`F6`).  
  - Eliminar archivos/carpetas (`Supr`).  
  - Manejo de conflictos (sobrescribir, omitir, aplicar a todos).  

- **Soporte de SFTP (libssh2)**  
  - Conexión a servidores remotos vía usuario + contraseña o clave privada.  
  - Listado de directorios y navegación remota.  
  - **Descarga de archivos** con progreso (barra de progreso Qt) y apertura con la aplicación predeterminada del sistema.  
  - Posibilidad de elegir la carpeta de descarga (se recuerda la última seleccionada).  

- **UI moderna en Qt**  
  - Splitter central ajustable.  
  - Barra de estado con mensajes informativos.  
  - Atajos de teclado para todas las operaciones básicas.  

---

## 🚀 Roadmap

- [ ] Subida de archivos (local → remoto).  
- [ ] Borrado remoto.  
- [ ] Mostrar permisos, tamaños y fechas en el listado remoto.  
- [ ] Integrar validación de `known_hosts`.  
- [ ] Mejoras de UX (arrastrar y soltar, menú contextual).  

---

## 🔧 Requisitos

- Qt 6.x  
- libssh2  
- CMake 3.16+  
- Compilador con soporte de C++17  

---

## 🛠️ Compilación

```bash
git clone https://github.com/tuusuario/OpenSCP-hello.git
cd OpenSCP-hello
cmake -S . -B build
cmake --build build
./build/openscp_hello
