# Twitch Vita

[![License: GPL-3.0](https://img.shields.io/badge/License-GPL_3.0-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-PS_Vita_%7C_PS_TV-informational.svg)](https://www.playstation.com/)
[![SDK](https://img.shields.io/badge/Toolchain-VitaSDK-green.svg)](https://vitasdk.org/)
[![Version](https://img.shields.io/badge/Version-v1.0.0-purple.svg)](https://github.com/xbnjx/twitch_vita/releases)

**Creador del proyecto:** [Benjamin Duarte](https://benjaminduarte.lat/)  
**Web del creador:** <https://benjaminduarte.lat/>

Cliente nativo no oficial de Twitch para las consolas PlayStation Vita y PlayStation TV, desarrollado en lenguaje C haciendo uso del toolchain de desarrollo VitaSDK.

El proyecto implementa un reproductor multimedia de alto rendimiento con decodificación de video acelerada por hardware nativo (`SceVideodec` / `SceAvcdec`), integración con la API de Twitch mediante el flujo seguro de vinculación por código QR (OAuth Device Flow), soporte para directos, grabaciones anteriores (VODs de hasta 48 horas) y clips con selección dinámica de resolución, junto con una interfaz de usuario ergonómica adaptada tanto a controles físicos como a pantalla táctil.

---

## Tabla de Contenidos

- [Características de la aplicación](#características-de-la-aplicación)
- [Requisitos de Instalación en la consola](#requisitos-de-instalación-del-cliente-en-la-consola-playstation-vita--ps-tv)
- [Guía de Instalación](#instalación)
- [Controles](#controles)
  - [Botones físicos](#botones-físicos)
  - [Pantalla táctil](#pantalla-táctil)
- [Requisitos de desarrollo y compilación](#requisitos-de-desarrollo-y-compilación)
- [Instrucciones de compilación](#instrucciones-de-compilación)
- [Consideraciones de rendimiento y arquitectura](#consideraciones-de-rendimiento-y-alcance-del-proyecto)
- [Licencia](#licencia)
- [Avisos de Terceros y Créditos](#avisos-de-terceros-y-créditos)
- [Descargo de responsabilidad (Disclaimer)](#descargo-de-responsabilidad-disclaimer)

---

## Requisitos de Instalación del cliente en la consola (PlayStation Vita / PS TV)

Para instalar y ejecutar Twitch en la consola, se requieren las siguientes condiciones y dependencias:

1. **Consola liberada**:
   - Firmware compatible (3.60 o 3.65 recomendado) con HENkaku / Enso instalado.
   - Opción **"Activar homebrew no seguro"** habilitada en `Ajustes > Ajustes de HENkaku`.
2. **VitaShell**:
   - Administrador de archivos esencial para transferir paquetes mediante USB o FTP e instalar archivos `.vpk`.
3. **iTLS-Enso (Imprescindible)**:
   - Añade soporte a nivel del sistema operativo para los protocolos TLS 1.2 y TLS 1.3 con certificados raíz modernos actualizados. Twitch y sus servidores de streaming (CDN) rechazan estrictamente conexiones que no utilicen TLS moderno.
4. **Plugins de kernel recomendados**:
   - `fd_fix.skprx`: Corrige errores y caídas en los descriptores de archivos al suspender o reanudar aplicaciones.
   - `kubridge.skprx`: Facilita la interoperabilidad y puente de funciones de kernel requeridas por aplicaciones multimedia.

---

## Instalación

Sigue estos pasos detallados para instalar el cliente de Twitch en tu PlayStation Vita o PlayStation TV:

### Paso 1: Preparación previa
1. Enciende tu consola y dirígete a la aplicación del sistema **Ajustes**.
2. Entra en **Ajustes de HENkaku** y asegúrate de que la casilla **"Activar homebrew no seguro"** (*Enable Unsafe Homebrew*) esté marcada. Esto es indispensable para que las aplicaciones multimedia puedan acceder a las funciones de red y aceleración de video por hardware.
3. Asegúrate de tener una conexión a internet Wi-Fi activa y configurada.

### Paso 2: Transferencia del archivo VPK a la consola
Descarga la versión más reciente del archivo `Twitch.vpk` y transfiérelo a la tarjeta de memoria (`ux0:`) utilizando cualquiera de estos dos métodos en **VitaShell**:

#### Opción A: Mediante cable USB (Recomendado por velocidad)
1. Abre **VitaShell** en tu PS Vita.
2. Presiona el botón **START** para abrir los ajustes de VitaShell y verifica que en `USB device` esté seleccionada tu tarjeta principal (`Memory Card` o `sd2vita`) y en `SELECT button` esté configurado en `USB`.
3. Conecta la consola a tu ordenador mediante el cable USB.
4. Presiona el botón **SELECT** en VitaShell; la consola aparecerá en tu ordenador como una unidad de disco extraíble.
5. Copia el archivo `Twitch.vpk` en la raíz de `ux0:` o dentro de una carpeta accesible (por ejemplo, `ux0:data/`).
6. Al finalizar la copia, presiona **Círculo (◯)** en la consola para desconectar de forma segura el modo USB.

#### Opción B: Mediante red local por FTP
1. Abre **VitaShell** en tu PS Vita.
2. Presiona el botón **START** y verifica que `SELECT button` esté configurado en `FTP`.
3. Presiona el botón **SELECT** para iniciar el servidor FTP. VitaShell mostrará en pantalla una dirección IP y un puerto (por ejemplo: `ftp://192.168.1.15:1337`).
4. Abre tu cliente FTP favorito en tu PC o teléfono (FileZilla, WinSCP o el explorador de archivos) e ingresa la dirección IP y el puerto indicados.
5. Navega al directorio `ux0:` (o `ux0:data/`) y transfiere el archivo `Twitch.vpk`.
6. Presiona **Círculo (◯)** en la Vita para cerrar el servidor FTP.

### Paso 3: Instalación del paquete en VitaShell
1. Dentro del explorador de archivos de VitaShell, navega hasta la ruta donde copiaste el archivo (`ux0:` o `ux0:data/`).
2. Resalta el archivo `Twitch.vpk` con el cursor y presiona el botón **Cruz (✕)**.
3. VitaShell te preguntará si deseas instalar el paquete: presiona **Cruz (✕)** para confirmar.
4. Si aparece una advertencia sobre permisos extendidos (*extended permissions / unsafe homebrew*), presiona **Cruz (✕)** nuevamente para autorizar la instalación.
5. Espera a que la barra de progreso complete el 100%.
6. *(Opcional)*: Una vez finalizada la instalación, puedes presionar **Triángulo (△)** sobre `Twitch.vpk` y elegir **Delete** para borrar el archivo instalador y recuperar espacio en tu tarjeta de memoria.

### Paso 4: Ejecución desde el LiveArea
1. Presiona el botón físico **PlayStation** (PS) para regresar al menú principal de la consola (LiveArea).
2. Verás la nueva burbuja de **Twitch** generada en la pantalla.
3. Toca la burbuja y pulsa **Iniciar** para comenzar a disfrutar de la aplicación.

---

## Requisitos de desarrollo y compilación

Para compilar el proyecto a partir del código fuente se requiere un entorno de desarrollo basado en Linux o WSL con las siguientes herramientas instaladas:

### Herramientas del sistema
- **CMake** (versión 3.20 o superior)
- **Make** o **Ninja**
- **Python 3**
- **Git**

### Toolchain y dependencias de VitaSDK
- **VitaSDK**: Toolchain de compilación cruzada `arm-vita-eabi-gcc`.
- Bibliotecas para VitaSDK (instalables mediante `vdpm` o el gestor de paquetes de VitaSDK):
  - `vita2d` (biblioteca de renderizado acelerado por GPU GXM)
  - `libcurl` (cliente de transferencia HTTP/HTTPS)
  - `libssl` y `libcrypto` (soporte criptográfico y TLS)
  - `jansson` (parseo y serialización de JSON)
  - `freetype` (renderizado tipográfico de alta definición)
  - `libpng`, `libjpeg-turbo`, `zlib` (decodificación de formatos gráficos)

---

## Instrucciones de compilación

1. Definir la variable de entorno que apunta al directorio de instalación de VitaSDK:

```bash
export VITASDK=/ruta/a/vitasdk
export PATH=$VITASDK/bin:$PATH
```

2. Configurar el proyecto con CMake utilizando el archivo toolchain de VitaSDK:

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
```

3. Compilar el ejecutable y empaquetar el VPK:

```bash
cmake --build build -- -j$(nproc)
```

Al finalizar el proceso, se generará el archivo `Twitch.vpk` en el directorio de compilación listo para ser transferido a la consola.

---

## Características de la aplicación

### Acceso directo con y sin cuenta (Modo Espectador)
- **Inicio inmediato**: La aplicación arranca de forma instantánea directo a la pantalla principal sin verificaciones bloqueantes de sesión ni pantallas forzadas de inicio de sesión.
- **Uso sin cuenta**: Permite explorar el catálogo completo de transmisiones en vivo, buscar canales y reproducir directos, grabaciones (VODs) y clips libremente en Modo Espectador.
- **Vinculación opcional de cuenta por código QR**: En cualquier momento, el usuario puede vincular su cuenta mediante el estándar Device Flow desde la pestaña *Siguiendo* o desde *Ajustes*, generando un código QR y código alfanumérico para validar en `twitch.tv/activate`.
- **Carga transparente de sesión**: Si ya existe una sesión guardada, la aplicación la inicializa en segundo plano y sincroniza los canales seguidos de manera asíncrona sin demorar el arranque de la app.
- **Selector de idioma en login**: Selector de idioma integrado directamente en la pantalla de vinculación (Español, Inglés y Portugués).

### Reproducción y Calidades
- Decodificación acelerada por hardware nativo para transmisiones en directo, grabaciones (VODs) y clips (`vita-hw-decoder`).
- Selector de calidades optimizadas:
  - **HD**: Calidad de alta definición adaptada dinámicamente según el tipo de contenido:
    - **En transmisiones en vivo**: Proporciona **720p real**, aprovechando las transmisiones a tasa de bits constante (CBR) con cadencia estable.
    - **En grabaciones (VODs) y Clips**: Mapea en el backend a **480p**, manteniendo la opción y preferencia visual como "HD" en la interfaz. Esto asegura que la reproducción sea completamente fluida y sin caídas de fotogramas ni parones en el decodificador de hardware.
  - **360p** (calidad predeterminada recomendada para balance óptimo de red y batería)
  - **240p**
  - **144p**
- Selector de calidad no bloqueante:
  - El menú de cambio de resolución se procesa dentro del bucle de presentación sin pausar el video de fondo ni congelar el ciclo de descarte de buffers (`render_complete`). Esto asegura que el video se mantenga reproduciéndose fluidamente a sus fotogramas por segundo originales sin desincronización de audio.
- Buffer dinámico y estabilidad de FPS:
  - Búfer de precarga HLS optimizado con política de retención estricta que impide descartar segmentos en memoria antes de ser leídos por los hilos de audio y video.
  - Ventana de tolerancia de cadencia que previene el descarte cíclico de cuadros ante fluctuaciones de red, manteniendo una tasa de cuadros estable y constante a lo largo de sesiones prolongadas.
- Comportamiento diferenciado según el tipo de contenido:
  - **Transmisiones en vivo**: Reconexión limpia de la señal para sincronizar con la emisión en directo al modificar la calidad o al seguir/dejar de seguir al streamer.
  - **Grabaciones (VODs)**: Conmutación de calidad con preservación de la posición exacta de reproducción en la barra de tiempo (sin reiniciar a `0:00`). Búsqueda instantánea (*Seek Optimization*) con alineación exacta al inicio de segmento (`vt_hls_segment_start_ms`) y sincronización estricta al fotograma clave del video.
  - **Clips de video**: Reproducción en streaming HTTPS continuo con extracción dinámica de tokens (`ClipPlayback`) y perfiles de resolución optimizados.
  - **Control de integridad de línea de tiempo y sincronización A/V**:
    - **Alineación de timestamp base (PTS baseline)**: Normalización matemática de marcas de tiempo de contenedor MPEG-TS y HLS relativas al inicio del stream, eliminando por completo los saltos erráticos a la hora 26 o al 100% causados por marcas de tiempo de 33 bits sin restar la base ($2^{33} / 90000 \text{ Hz} \approx 26.5 \text{ horas}$).
    - **Bloqueo bidireccional de desincronización (Interlock de starvation)**: Detección activa de vaciado de buffer de video (`ready_frames == 0`) que pausa automáticamente el reloj y la salida de audio de fondo ante demoras de red, reanudando ambos de forma coordinada al rellenar el buffer. Esto elimina completamente las congelaciones con audio corriendo de fondo y el posterior efecto de "cámara rápida" para alcanzar al audio.

### Navegación y Secciones
- **En vivo**: Lista de transmisiones activas con miniaturas en tiempo real, título, categoría y número de espectadores.
- **Siguiendo**: Lista completa sincronizada con el servidor, mostrando canales en directo (**LIVE**) en la parte superior y canales desconectados (**OFFLINE**) a continuación. En caso de no tener sesión activa, muestra una tarjeta con botón interactivo de "Iniciar sesión con QR" accionable con el botón **START** o pulsación táctil.
- **Ajustes**: Configuración de calidad preferida, idioma de la interfaz (Español, Inglés, Portugués, etc.) y gestión de cuenta de Twitch con diálogo interactivo de confirmación de cierre de sesión con botones **SÍ (✕)** y **NO (◯)**.
- **Búsqueda global**: Accesible en todo momento mediante el botón físico Cuadrado (□) o pulsando sobre la barra de búsqueda superior, con resultados instantáneos y salida rápida con Círculo (◯).
- **Pestañas inferiores ergonómicas**: Distribución equidistante de las pestañas principales con márgenes dedicados para los iconos de los gatillos **L** y **R**, evitando solapamientos visuales.
- **Cancelación unificada de tareas**: Pantallas de carga con spinner e indicación interactiva `"Toca para cancelar o acciona [◯]"`.
- **Barra superior táctil ampliada**: Cabecera ergonómica que incorpora el logotipo de Twitch, buscador táctil, estado de batería y reloj, y botón directo a **"Mi canal"** con icono de SELECT.
- **Guía visual con iconos de botones PlayStation Vita**: Indicadores gráficos con los iconos reales de los botones de la consola:
  - **Cuadrado (□)**: Dentro de la píldora de búsqueda superior y en el botón de Seguir / Dejar de seguir en canal.
  - **START**: En el botón interactivo de "Iniciar sesión con QR" dentro de la pestaña Siguiendo (sin sesión) y para pausar/reanudar en el reproductor.
  - **SELECT**: Integrado en el botón "Mi canal" para acceso directo al perfil del streamer autenticado.
  - **Gatillos L y R**: En los extremos de las barras de pestañas principales y subpestañas de canales para cambio rápido.
  - **Cruz (✕)**: Indicador de apertura en tarjetas, botón de reproducción de directos y confirmaciones de inicio de sesión.
  - **Triángulo (△)**: En el botón de selector de calidad, recarga de seguidos y generación de nuevo código de vinculación.
  - **Círculo (◯)**: En los botones de retroceso, salida de búsqueda y cierre del reproductor.
- **Perfil de Canal**: Vista completa del creador con estado en vivo, opción de Seguir/Dejar de seguir sincronizada en tiempo real con los servidores de Twitch, y subpestañas dedicadas:
  - **En vivo**: Reproducción directa de la transmisión actual.
  - **Videos**: Catálogo paginado de grabaciones anteriores (VODs) con duración exacta y barra de tiempo.
  - **Clips**: Sección de mejores momentos con miniaturas, duración, visualizaciones y cambio de resolución.

---

## Controles

### Botones físicos

| Botón | En Menús y Listas | En Reproductor de Video |
| :--- | :--- | :--- |
| **D-Pad / Stick Izquierdo** | Navegar entre elementos y tarjetas | Adelantar / Retroceder 10 segundos (VODs y Clips) |
| **Gatillos L / R** | Alternar pestañas (En vivo, Siguiendo, Ajustes) o subpestañas de canal | En VODs: Salto rápido de 1 minuto (L: -1 min, R: +1 min) |
| **Cruz (✕)** | Seleccionar / Abrir stream, canal o clip | Pausar / Reanudar reproducción |
| **Círculo (◯)** | Salir de búsqueda / Volver en vinculación / Cancelar diálogos | Salir del reproductor (`[◯] Salir`) |
| **Triángulo (△)** | Actualizar feed / Generar nuevo código QR (en login) | Abrir selector de calidad (1080p, 720p, 480p, 360p, 240p, 144p) |
| **Cuadrado (□)** | Iniciar búsqueda global de canales | Seguir / Dejar de seguir streamer actual |
| **START** | Iniciar sesión con QR (en pestaña Siguiendo sin sesión) | Pausar / Reanudar reproducción |
| **SELECT** | Abrir directamente "Mi canal" | Recargar señal del directo (solo en streams en vivo) |

### Pantalla táctil

- **Navegación**: Toque directo sobre cualquier transmisión, video o ajuste. Barra inferior optimizada en tres secciones anchas para pulsación táctil ergonómica.
- **Barra superior ampliada**: Toque directo en el campo de búsqueda para desplegar el teclado en pantalla, y botón táctil destacado "Mi canal" para acceder rápidamente al perfil propio.
- **Controles táctiles en streams en vivo**: Al tocar la pantalla se despliega el menú HUD con botones centrales representados exclusivamente por iconos limpios:
  - **Pausar / Reanudar (`▶` / `⏸`)**: Alterna la reproducción de la transmisión.
  - **Recargar señal (`⟳`)**: Reconecta y resincroniza inmediatamente el flujo con el punto de emisión en directo.
- **Controles táctiles en grabaciones (VODs) y Clips**: Toque en el centro para pausar o reanudar; barra de tiempo interactiva (*Scrubber*) en la parte inferior con arrastre táctil y previsualización del minutero en tiempo real.
- **Barra de tiempo con confirmación al soltar (*Release on Touch Up*)**: Al arrastrar la barra de tiempo en VODs o Clips con la pantalla táctil capacitiva, la posición visual y el minutero se actualizan fluidamente en pantalla indicando `"Cargando..."` sin saturar la red ni reiniciar conexiones repetidamente; la búsqueda (*seek*) en el motor multimedia se ejecuta exclusivamente al levantar el dedo, garantizando máxima precisión y fluidez.
- **Barra superior del reproductor**: Botones táctiles para volver (`✕`), alternar estado de seguimiento (`Seguir` / `Siguiendo`) y abrir el selector de resolución (`⚙`).

---

## Consideraciones de rendimiento y alcance del proyecto

Se recomienda no añadir más funciones a las existentes en esta versión.

### Soporte de VODs de hasta 48 horas y gestión de desbordamiento PTS
- El motor de listas de reproducción HLS cuenta con búfer dinámico autoexpandible (`hls_buf_ensure`), permitiendo cargar índices completos de directos maratónicos de hasta 48 horas sin cortes.
- Si un VOD supera las 47h:59m:59s (techo máximo del decodificador multimedia), la aplicación lo notifica de inmediato mediante un aviso en pantalla impidiendo bloqueos en el hardware.
- Normalización automática de marcas de tiempo de contenedor MPEG-TS relativas al inicio del stream, asegurando que las grabaciones siempre arranquen puntualmente en `00:00:00` y respeten el tiempo real de emisión.

### Decisión técnica: exclusión del sistema de chat interactivo
Durante el desarrollo se evaluó e integró de forma experimental el chat en vivo; sin embargo, se determinó su exclusión definitiva para garantizar la estabilidad absoluta de la aplicación debido a las limitaciones arquitectónicas de la PlayStation Vita:

1. **Sobrecarga de cifrado SSL/TLS y WebSockets (IRC / TMI)**:
   - Mantener un socket bidireccional continuo con tráfico entrante constante compite directamente por ciclos de CPU y ancho de banda del adaptador Wi-Fi con el hilo de red HLS, el cual ya trabaja a máxima exigencia transfiriendo fragmentos de video y audio.
2. **Parseo intensivo de mensajes y fragmentación de memoria**:
   - En canales activos, Twitch transmite decenas de eventos por segundo. Cada mensaje llega con metadatos extensos de usuario, insignias, emotes y menciones.
   - El procesamiento recurrente en C y los ciclos sucesivos de reserva y liberación de memoria (`malloc`/`free`) saturan los núcleos del procesador ARM Cortex-A9 (333–444 MHz) y fragmentan la memoria RAM de la consola (512 MB), produciendo micro-parones (*stuttering*), retrasos en el ciclo de descarte de buffers y desincronización de audio con el decodificador por hardware (`SceAvcdec`).
3. **Saturación en el renderizado tipográfico (FreeType)**:
   - El renderizado de fuentes en la consola rasteriza texto vectorial mediante FreeType directamente en la CPU. Dibujar decenas de nombres de usuario con colores dinámicos, textos con saltos de línea y desplazamiento vertical continuo a 30/60 fps consume los milisegundos de tiempo de cuadro que la GPU y el motor de video necesitan para presentar los fotogramas de forma fluida.

Prescindir del chat permite consagrar el 100% de la potencia y el ancho de banda del bus de memoria (~1.6 GB/s) a la decodificación nativa de video, permitiendo transmisiones estables y sin congelamientos incluso durante sesiones prolongadas.

### Decisión técnica: inclusión de la sección de clips con selección de calidad
A diferencia de otras plataformas como Kick, donde los clips se sirven de forma fija e inalterable en su calidad original (frecuentemente 1080p a 60 fps con altos bitrates que saturan el decodificador de hardware de la PS Vita provocando congelamientos severos), Twitch genera perfiles de transcodificación escalables para sus clips (**1080p**, **720p**, **480p** y **360p**).

Gracias a esta infraestructura del servicio y al uso de la consulta GraphQL `ClipPlayback`, la aplicación extrae dinámicamente el listado de resoluciones y tokens firmados. Esto permite reproducir clips seleccionando la calidad ideal (como 360p o 480p), ofreciendo una reproducción fluida, nítida y perfectamente adaptada a las especificaciones térmicas y de hardware de la consola.

### Decisión técnica: comportamiento de la calidad HD en directos vs VODs y clips
Durante las pruebas de rendimiento en hardware real se determinó una diferencia arquitectónica fundamental entre cómo Twitch sirve las transmisiones en vivo y cómo almacena las grabaciones (VODs) y clips:

1. **Transmisiones en vivo (720p real fluido)**:
   - Los directos de Twitch se originan bajo directrices estrictas de emisión: tasa de bits constante (**CBR**), intervalos fijos de fotogramas clave (**GOP exacto de 2.0 segundos**) y entrega fragmentada en pequeños bloques regulares de 2 segundos desde la CDN.
   - El decodificador por hardware de la consola (`SceAvcdec`) procesa este flujo regular de manera óptima a 720p porque el flujo de datos es continuo, predecible y no satura los búferes de entrada de la GPU ni el bus de memoria (~1.6 GB/s).

2. **Grabaciones (VODs) y Clips (backend a 480p)**:
   - Twitch no cuenta con un reproductor ni un pipeline de transcodificación optimizado para procesadores de bajo consumo en su contenido grabado y clips.
   - Los **clips** se sirven como archivos progresivos MP4 con tasa de bits variable (**VBR**) agresiva que alcanza picos superiores a 8,000–12,000 kbps, compresión *High Profile* con múltiples fotogramas de referencia y entrega por ráfagas intensivas de red (*burst delivery*) que saturan el chip Wi-Fi y desbordan la memoria intermedia de `SceAvcdec` a 720p, provocando micro-congelamientos y desincronización de audio.
   - Los **VODs** agrupan fragmentos más largos (6 a 10 segundos) con variaciones de sincronización A/V y estructuras de fotogramas complejas.
   - Para resolver esto sin sacrificar la ergonomía del usuario, la aplicación mantiene la opción visible y preferencia guardada como **"HD"**, pero el motor interno asigna el backend de VODs y clips a **480p** (854x480). Dado que la pantalla de la PS Vita tiene una resolución nativa de 960x544 píxeles, 480p ofrece una nitidez prácticamente idéntica a 720p en pantalla pero consumiendo apenas ~1,500–2,000 kbps de ancho de banda, permitiendo decodificación fluida a velocidad constante, cero parones y menor consumo térmico.

### Decisión técnica: corrección del desbordamiento de línea de tiempo (Salto a hora 26)
En grabaciones y clips se identificó que al retroceder o adelantar el video, en ciertas ocasiones la barra de tiempo saltaba erráticamente a la **hora 26** en videos de apenas 1 o 2 horas.
- **Causa raíz**: En el estándar MPEG-TS utilizado por Twitch, los sellos de tiempo de presentación (**PTS**) se representan mediante un contador entero sin signo de 33 bits que opera a 90 kHz. El valor máximo representable es $2^{33} - 1 = 8,589,934,591$ tics, lo que equivale exactamente a:
  $$\frac{2^{33}}{90,000 \text{ Hz}} \approx 95,443.7 \text{ segundos} \approx \mathbf{26 \text{ horas, } 30 \text{ minutos, } 43 \text{ segundos}}$$
- Cuando un segmento contiene marcas de tiempo ligeramente negativas (por ejemplo, desfasajes de pre-roll de audio o reordenamiento de cuadros B antes del segundo 0:00), el contador entero de 33 bits sufre un subdesbordamiento aritmético (*underflow*) y se convierte en $2^{33} - \text{offset}$, proyectando un valor de aproximadamente 26 horas y 30 minutos.
- **Solución implementada**: Se introdujo extensión de signo explícita de 33 a 64 bits y acotamiento estricto contra la duración real del video (`duration_ms`), asegurando que las marcas de tiempo negativas se fijen en 0:00 y la barra de tiempo nunca sobrepase la duración real del contenido.

### Estado final del proyecto
En el estado actual, el consumo de recursos se encuentra balanceado: la reproducción de transmisiones, VODs y clips, así como la navegación por la interfaz, son fluidas y estables sin necesidad de recurrir a overclocking. Incrementar el número de peticiones al servidor o añadir subsistemas en segundo plano sobrecargaría innecesariamente la CPU. El proyecto se considera completo, estable y plenamente funcional como cliente nativo.

---

## Licencia

Este proyecto se distribuye bajo los términos de la **GNU General Public License v3.0 (GPL-3.0)**.  
Copyright (C) 2026 **Benjamin Duarte** (<https://benjaminduarte.lat/>).

Este programa es software libre: puedes redistribuirlo y/o modificarlo bajo los términos de la Licencia Pública General de GNU publicada por la Free Software Foundation, bien de la versión 3 de dicha Licencia o (a tu elección) cualquier versión posterior.

Este programa se distribuye con la esperanza de que sea útil, pero **SIN NINGUNA GARANTÍA**; ni siquiera la garantía implícita de COMERCIABILIDAD o IDONEIDAD PARA UN PROPÓSITO PARTICULAR. Para más detalles, consulta el archivo [LICENSE](LICENSE).

---

## Avisos de Terceros y Créditos

Este proyecto utiliza librerías, tipografías y recursos de terceros de la comunidad de código abierto (FFmpeg, libcurl, Mbed TLS, Jansson, libwebp, vita2dlib, qrcodegen y fuentes tipográficas).  
Para consultar los textos completos de licencias, autores y atribuciones, revisa el archivo [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

## Descargo de responsabilidad (Disclaimer)

- **Twitch** es una marca comercial registrada de **Twitch Interactive, Inc.** o **Amazon.com, Inc.**
- **PlayStation Vita** y **PlayStation TV** son marcas registradas de **Sony Interactive Entertainment Inc.**
- Este cliente es un desarrollo independiente y de código abierto sin fines comerciales. No está afiliado, respaldado ni respaldado oficialmente por Twitch, Amazon o Sony.

