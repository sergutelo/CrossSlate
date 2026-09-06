# CrossSlate MVP

CrossSlate es un firmware de productividad ligero para **Xteink X4**, evolucionado desde MicroSlate. Conserva el editor de notas y el teclado BLE y añade una pantalla de inicio con información local desde MicroSD.

## Alcance del MVP

- Rebranding visible a **CrossSlate** y Dashboard como pantalla inicial.
- Dashboard con fecha/hora del sistema, batería, clima cacheado, hasta 5 próximas tareas y una nota fijada.
- Notas `.txt` en `/notes/`: crear, abrir, renombrar, borrar, editar y autosalvar.
- Teclado BLE HID y gestión de hasta 4 teclados emparejados.
- Sync WiFi de solo lectura para copia de notas al PC.
- Arbitraje de radio: al iniciar WiFi se detienen el escaneo/reconexión BLE y se desconecta el teclado; BLE vuelve a reconectar al apagar WiFi. No se mantienen BLE y WiFi activos simultáneamente.
- Retorno opcional a CrossInk: **Volver a CrossInk** solo aparece si el slot OTA alterno contiene cabeceras de imagen y aplicación válidas.

## Dashboard y formatos SD

CrossSlate crea `/crossslate` si no existe y lee los siguientes ficheros pequeños:

### `/crossslate/weather.json`

```json
{"temperature_c":21.5,"summary":"Soleado","updated":"2026-09-01 09:30","icon":0}
```

Límites: fichero leído hasta 511 bytes; `summary` 47 bytes; `updated` 31 bytes. El Dashboard intenta actualizar automáticamente el clima fijo de Madrid (Open-Meteo HTTPS, sin API key) al arrancar y con **R**. Usa solo credenciales guardadas en NVS; si falla, conserva la caché. `icon` es opcional y va de 0 a 7.

### `/crossslate/tasks.txt`

Una tarea no vacía por línea. Se muestran como máximo 5 tareas; cada una se trunca a 79 bytes. El fichero leído está limitado a 511 bytes.

```text
Preparar reunión
Revisar borrador
Comprar pilas
```

### `/crossslate/pinned.txt`

La primera línea no vacía se muestra como nota fijada, truncada a 159 bytes. El fichero leído está limitado a 191 bytes.

## Fecha y hora

La hora se sincroniza por NTP cuando se completa una conexión de Sync WiFi, usando la zona CET/CEST. Hasta que exista una hora válida, el Dashboard muestra `Fecha/hora no sincronizada`. El X4 no mantiene necesariamente hora fiable tras pérdida total de alimentación.

## Escrituras seguras de notas

El guardado usa `/notes/<nombre>.txt.tmp`, verifica los bytes escritos, rota el original a `.bak`, promociona el `.tmp` y verifica el tamaño final. Cada operación crítica (`remove`, `rename`, lectura/escritura de `otadata`) comprueba su resultado. Al arrancar:

1. si existe el original, se descarta un `.tmp` huérfano;
2. si falta el original y existe `.bak`, se restaura primero `.bak`;
3. si solo existe `.tmp`, se promociona como recuperación de último recurso.

Un error mantiene el indicador de cambios sin guardar.

## Cambio de arranque OTA

`src/ota_boot_switch.cpp` valida el slot alterno sin depender de `esp_image_verify` (incompatible con ciertas imágenes Xteink parcheadas): comprueba tipo/subtipo, cabecera de imagen, número de segmentos y `esp_app_desc_t`. Para cambiar de app:

- valida la partición de destino de nuevo;
- lee y valida las dos copias de `otadata` por secuencia, estado y CRC;
- borra y escribe únicamente la copia inactiva;
- relee y compara byte a byte antes de reiniciar.

El módulo está implementado localmente para CrossSlate; no copia HAL ni renderers de CrossInk.

## Compilar

Requiere PlatformIO:

```bash
pio run -e xteink_x4
```

No es necesario ni se incluye ningún binario opaco de TypeSlate. Este repositorio contiene el código fuente y las librerías necesarias; PlatformIO descarga `esp-nimble-cpp` como dependencia declarada.

## Controles principales

- Dashboard: **Enter** abre el menú; **Esc/Back** recarga los datos de SD.
- Menú: flechas para navegar, **Enter** para seleccionar.
- Editor: flechas, Home/End, Backspace/Delete; `Ctrl+S` guarda; `Ctrl+N` cambia título; `Ctrl+Z` modo limpio; `Tab` cambia modo de escritura; Esc guarda y vuelve.
- Pulsación larga de Power: guarda y entra en deep sleep.

## Límites pendientes

- Actualización de clima por HTTPS/Open-Meteo: aplazada para no añadir TLS, configuración de ubicación y presión de RAM al MVP. La actualización manual actual consiste en recargar la caché SD.
- Teclado español: el mapeo HID actual es US. Acentos, `ñ`, AltGr y dead keys requieren composición UTF-8 y cambios de cursor/borrado conscientes de puntos de código; no se incluyeron para no arriesgar el editor existente.
- No hay editor en el dispositivo para tareas, clima o nota fijada; se gestionan como ficheros pequeños en la SD.
- La detección del slot alterno valida estructura de imagen, no firma criptográfica.
- No se flashea hardware como parte del build.

## Estructura nueva relevante

- `src/dashboard.{h,cpp}`: carga fija desde SD.
- `src/dashboard_parser.{h,cpp}`: parser acotado sin `String` dinámicas.
- `src/ota_boot_switch.{h,cpp}`: validación y escritura verificada de `otadata`.
- `test/test_dashboard_parser.cpp`: prueba host de clima, tareas y nota fijada.
