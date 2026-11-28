# Cómo usar OpenSCP

[Read in English](USAGE.md)

Esta guía explica cómo se comporta OpenSCP una vez abierto: cómo moverse entre
carpetas, qué hacen las conexiones, las transferencias y la sincronización en
los casos poco habituales, y cómo obtener información de diagnóstico. El resto
de la documentación está en inglés:

- [Atajos de teclado](KEYBOARD_SHORTCUTS.md)
- [Cómo actualizar OpenSCP](UPDATES.md)
- [Compatibilidad de plataformas y protocolos](PLATFORM_COMPATIBILITY.md)

## Navegación por rutas

Cada panel presenta la ubicación actual como un campo de ruta convencional. Haz
clic en cualquier directorio superior dentro de la ruta para abrirlo, o en el
directorio actual para mostrar el diálogo **Abrir directorio**. También puedes
usar `Ctrl+L` (`Cmd+L` también está disponible en macOS) o el atajo Abrir de
la plataforma: `Ctrl+O` en Linux y Windows y `Cmd+O` en macOS.

El diálogo permite escribir una ruta directamente y consultar las rutas
recientes y los favoritos del panel local o de la sesión remota seleccionada.
Consulta [Atajos de teclado](KEYBOARD_SHORTCUTS.md) para conocer todos los
atajos y cuándo dependen del panel activo.

## Conexiones

Cancelar una conexión o cerrar la ventana también cancela las solicitudes
pendientes de autenticación SSH y confirmación de huella. SSH prueba las
direcciones resueltas como hace Happy Eyeballs: si una no ha respondido en
250 ms, o rechaza la conexión, empieza la siguiente, alternando IPv6 e IPv4, y
todos los intentos comparten un límite de 20 segundos. La resolución de nombres
usa el sistema y puede tardar más, independientemente de ese límite TCP.

## Transferencias

Las descargas locales rechazan los enlaces simbólicos en las carpetas
superiores del destino que el usuario puede modificar. Elige la carpeta física
de destino cuando la ruta de una descarga contenga un enlace.

Las descargas requieren archivos `.part` regulares. Si una entrada `.part`
existente es una FIFO (tubería con nombre), la descarga informa de un error de
E/S local sin esperar a un lector ni reemplazar esa entrada. Elige otro destino
o revisa la entrada en conflicto antes de reintentar.

Las transferencias que comparten un destino o su ruta `.part` se ejecutan en
serie. Los nombres locales que solo difieren en mayúsculas o normalización
Unicode también se ejecutan en serie, incluso en volúmenes que los distinguen.
Al eliminar una tarea con sus datos parciales, se conserva el `.part` mientras
otra tarea de la cola pueda reanudarse desde él, y no se borra nada de las
tareas terminadas ni de las subidas por SCP clásico, que escriben directamente
en el destino.

Al mover un archivo local al servidor en macOS y Linux, OpenSCP completa la
subida y conserva el origen local con una advertencia. El borrado automático no
puede excluir que otro proceso sustituya el origen después de comprobarlo.
Revisa la subida y el archivo local antes de borrarlo manualmente; después,
reintenta la tarea para completar la limpieza sin repetir la subida. Tras
reiniciar, la limpieza requiere revisión manual porque ya no se dispone de la
identidad original del archivo. En Windows, OpenSCP borra el origen local
después de la subida si no cambió: mantiene el archivo abierto contra cambios
mientras lo comprueba y lo borra. La limpieza del origen remoto al mover
archivos mediante descarga mantiene su comportamiento.

## Sincronización

La sincronización resuelve una raíz local enlazada a su ruta física y rechaza
las carpetas enlazadas que haya debajo de esa raíz.

La sincronización nunca copia FIFOs, sockets ni dispositivos, y deja fuera los
nombres con barra invertida o caracteres de control, que las subidas también
rechazan. Un nombre que solo se diferencia de otro del lado contrario en
mayúsculas o normalización Unicode aparece como conflicto y no se copia ni se
borra.

El modo espejo solo propone borrar donde el recorrido del origen comprobó que
los elementos no existen. Los enlaces omitidos, las carpetas ilegibles y los
límites de profundidad conservan los elementos del destino en esos subárboles
sin comprobar.

## Diagnóstico

Estas variables de entorno opcionales ayudan a diagnosticar problemas:

- `OPENSCP_LOG_LEVEL=off|error|warn|info|debug`
- `OPENSCP_TRANSFER_INTEGRITY=off|optional|required`
- `OPENSCP_KNOWNHOSTS_PLAIN=1|0`
- `OPENSCP_FP_HEX_ONLY=1`
- `OPENSCP_ENV=dev|prod` selecciona el entorno de ejecución
- `OPENSCP_LOG_SENSITIVE=1` permite detalles sensibles de diagnóstico solamente
  junto con `OPENSCP_ENV=dev`
- `OPENSCP_ENABLE_INSECURE_FALLBACK=1` solo cuando la compilación lo permite

Los mensajes van a la salida de error estándar. En Windows, donde OpenSCP no
muestra consola, van también a la salida del depurador, que
[DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview)
muestra al momento. Para guardarlos en un archivo, ábrelo desde PowerShell
guardando la salida de error en uno:

```powershell
$env:OPENSCP_LOG_LEVEL = 'debug'
Start-Process OpenSCP.exe -RedirectStandardError openscp.log
```

Los registros sensibles están deshabilitados por defecto y solo deberían
activarse temporalmente en un entorno de desarrollo controlado.
