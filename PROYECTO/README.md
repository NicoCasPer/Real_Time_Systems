# STR 2026 — Sistema de Control Ambiental (Environmental Control)

Proyecto de **Sistemas en Tiempo Real** sobre **ESP32 + ESP-IDF 5.5 (FreeRTOS)**.
Controla la temperatura de un recinto mediante un **NTC**, regula un **ventilador** y un
**motor de cortina** por **PWM**, gestiona una **alarma**, una **luz ambiente RGB**, una
**agenda horaria** y todo se opera desde un **dashboard web** servido por el propio ESP32.

---

## 1. Descripción general

El ESP32 levanta su propio **punto de acceso WiFi** y, opcionalmente, se conecta a la red
local (**modo estación / STA**). Sirve una página web (dashboard) con una **API REST** para:

- Leer temperatura en tiempo real (NTC con calibración por ecuación Beta).
- Controlar la **ventilación** (modo automático proporcional o manual).
- Controlar el **motor de la cortina** (manual o por **agenda horaria**).
- Configurar una **alarma** por sobre-temperatura (enciende el ventilador al 100%).
- Ajustar la **luz ambiente RGB**.
- Conectarse a una red WiFi y sincronizar la **hora real** por NTP.

Todo el estado se **persiste en NVS** (memoria no volátil), por lo que sobrevive a reinicios.

---

## 2. Tareas de FreeRTOS (lo más preguntado)

El sistema corre **7 tareas concurrentes**, repartidas en los **2 núcleos** del ESP32 y
con prioridades distintas para garantizar el comportamiento de tiempo real:

| # | Tarea | Función | Prioridad | Stack (bytes) | Núcleo | Periodo | Archivo |
|---|-------|---------|:---------:|:-------------:|:------:|---------|---------|
| 1 | `wifi_app_task` | Máquina de estados del WiFi (AP + STA, eventos) | **5** | 4096 | 0 | Por eventos (cola) | [wifi_app.c:953](main/wifi_app.c#L953) |
| 2 | `checking_app_task` | Espera sincronización de hora y compara registros horarios | **5** | 4096 | 1 | 30 s | [wifi_app.c:954](main/wifi_app.c#L954) |
| 3 | `check_sta_connection_state` | Monitorea la reconexión en modo estación (se crea al conectar) | **5** | 4096 | 0 | Por eventos | [wifi_app.c:754](main/wifi_app.c#L754) |
| 4 | `httpd` (servidor HTTP) | Atiende las peticiones web/REST (la crea `httpd_start`) | **4** | 8192 | 0 | Por petición | [http_server.c:1214](main/http_server.c#L1214) |
| 5 | `http_server_monitor` | Procesa la cola de eventos del servidor web | **3** | 4096 | 0 | Por eventos (cola) | [http_server.c:1188](main/http_server.c#L1188) |
| 6 | `env_temp_task` | Lee el NTC, actualiza RGB, alarma y ventilador | **5** | 4096 | 1 | 1 s | [environment_control.c:461](main/environment_control.c#L461) |
| 7 | `env_schedule_task` | Aplica la agenda horaria al motor de la cortina | **4** | 4096 | 1 | 15 s | [environment_control.c:462](main/environment_control.c#L462) |

> Las prioridades y tamaños de pila de las tareas de red están centralizados en
> [tasks_common.h](main/tasks_common.h).

**¿Por qué estas prioridades?**
- Las tareas de **adquisición/control** (`env_temp_task`) y la **gestión WiFi** corren a la
  prioridad más alta (**5**) porque son las que tienen requisitos temporales más exigentes
  (muestreo periódico y atención a eventos de red).
- El **servidor HTTP** (4) y la **agenda** (4) van por debajo: pueden tolerar más latencia.
- El **monitor** del servidor (3) es el menos crítico.

**Reparto de núcleos (afinidad):** las tareas de red (WiFi/HTTP) se fijan al **núcleo 0**
y las de control ambiental (temperatura/agenda) al **núcleo 1**, para que el tráfico de red
no interfiera con el muestreo periódico del sensor.

---

## 3. Sincronización (mutex / semáforos)

Como varias tareas comparten datos, se usan **mutex** (semáforos de exclusión mutua) para
evitar **condiciones de carrera**:

| Mutex | Protege | Archivo |
|-------|---------|---------|
| `s_state_lock` | La estructura global de estado `s_state` (temperatura, modos, RGB, agenda…) | [environment_control.c:59](main/environment_control.c#L59) |
| `s_adc_lock` | El acceso al ADC (lecturas del NTC) | [environment_control.c:60](main/environment_control.c#L60) |

Patrón usado: `lock_state()` / `unlock_state()` rodean toda lectura o escritura del estado
compartido. Así, mientras la tarea de temperatura actualiza `s_state`, el servidor HTTP no
puede leer un valor a medio escribir.

---

## 4. Mapa de pines (hardware)

Definido en [environment_control.h:10-16](main/environment_control.h#L10-L16):

| GPIO | Función | Tipo | Detalle |
|:----:|---------|------|---------|
| **18** | PWM motor de cortina | Salida PWM (LEDC) | 25 kHz · 10 bits → driver MOSFET |
| **25** | PWM ventilador | Salida PWM (LEDC) | 25 kHz · 10 bits → driver MOSFET |
| **34** | Sensor NTC | Entrada ADC1_CH6 | Solo entrada · divisor resistivo |
| **21** | LED RGB — Rojo | Salida PWM (LEDC) | 5 kHz · 8 bits |
| **22** | LED RGB — Verde | Salida PWM (LEDC) | 5 kHz · 8 bits |
| **23** | LED RGB — Azul | Salida PWM (LEDC) | 5 kHz · 8 bits |
| **2** | LED de alarma | Salida digital | Parpadeo en alarma |
| **GND** | Tierra común | — | ESP32 + baterías + motores |

---

## 5. Drivers de potencia — Motor y Ventilador (IRFZ44N)

El ESP32 **no puede mover motores directamente** (sus pines dan ~3.3 V y poca corriente).
Cada motor se controla con un **MOSFET de canal N** como interruptor de lado bajo (*low-side*),
conmutado por PWM:

```
        Batería 9V (+)
              │
        [ Motor / Ventilador ]   (2 pines)
              │
              ├───────►|────────┐   diodo flyback 1N4007 (raya hacia +9V)
              │                 │
              D (Drain)         │
GPIO ─[100 Ω]─ G (Gate) ── IRFZ44N
              │                 │
           [10 kΩ]              S (Source)
              │                 │
             GND ────────────── GND  ← común con ESP32 y batería(−)
```

**Componentes por motor (×2):**
- **IRFZ44N** — MOSFET de potencia (idealmente **IRLZ44N**, *logic-level*, para saturar bien con 3.3 V).
- **R 100 Ω** — entre GPIO y *Gate* (limita el pico de corriente al pin).
- **R 10 kΩ** — entre *Gate* y GND (*pull-down*: mantiene el MOSFET apagado durante el arranque).
- **Diodo 1N4007** — *flyback*, en paralelo con el motor (absorbe el pico inductivo).

**Funcionamiento:** el PWM enciende y apaga el MOSFET miles de veces por segundo; el
**ciclo de trabajo (duty)** determina la **velocidad** del motor (0–100 %).

> ⚠️ **Importante:** el "servo" de la cortina es en realidad un **motor DC de 2 pines**, por
> eso se controla por velocidad PWM (igual que el ventilador) y **no** por ancho de pulso de
> servo. El GND debe ser **común** entre ESP32, baterías y motores.

---

## 6. Sensor de temperatura NTC

Termistor **NTC de 10 kΩ / B3950** en un **divisor de tensión** con una resistencia fija
**de 15 kΩ**:

```
3.3 V ──[ R serie 15 kΩ ]──┬── GPIO34 (ADC)
                           │
                       [ NTC 10 kΩ ]
                           │
                          GND
```

**Cadena de conversión** (en [environment_control.c](main/environment_control.c)):
1. **Lectura ADC** con **filtro de mediana** de 64 muestras (rechaza picos de ruido).
2. **Calibración de fábrica** del ADC (esquema *curve fitting* / *line fitting*) → mV reales.
3. **Resistencia del NTC:** `R = V·Rserie / (Vcc − V)`.
4. **Temperatura (ecuación Beta):** `1/T = 1/T₀ + (1/β)·ln(R/R₀)`.

**Parámetros** ([environment_control.c:37-41](main/environment_control.c#L37-L41)):

| Parámetro | Valor |
|-----------|-------|
| `NTC_NOMINAL_RESISTANCE_OHMS` (R₀ a 25 °C) | 10 000 Ω |
| `NTC_SERIES_RESISTOR_OHMS` (R serie) | 15 000 Ω |
| `NTC_BETA_COEFFICIENT` (β) | 3950 |
| `NTC_NOMINAL_TEMPERATURE_C` (T₀) | 25 °C |
| `NTC_ADC_SAMPLES` (muestras por lectura) | 64 |

> **Nota de estabilidad:** el NTC es de alta impedancia; para lecturas estables se recomienda
> un **condensador de 100 nF entre GPIO34 y GND** y cables cortos lejos de los motores.

---

## 7. PWM (módulo LEDC del ESP32)

Se usa el periférico **LEDC** con 3 *timers* independientes y 5 canales:

| Timer | Uso | Frecuencia | Resolución | Canales |
|-------|-----|:----------:|:----------:|---------|
| `RGB_TIMER` (0) | LED RGB | 5 kHz | 8 bits (0–255) | 0, 1, 2 |
| `FAN_TIMER` (1) | Ventilador | 25 kHz | 10 bits (0–1023) | 3 |
| `SERVO_TIMER` (2) | Motor cortina | 25 kHz | 10 bits (0–1023) | 4 |

Las frecuencias de 25 kHz están **fuera del rango audible** (no hay chillido en los motores).

---

## 8. Lógica de control

### Ventilación
- **Modo AUTO:** velocidad **proporcional** a la temperatura entre `Temp deseada` (0 %) y
  `Temp máxima` (100 %).
- **Modo MANUAL:** velocidad fija elegida por el usuario.

### Alarma + ventilador (requisito clave)
Cuando `temperatura > Temp máxima` → **alarma activa**:
- El **LED de alarma parpadea**.
- El **ventilador se fuerza al 100 %** sin importar el modo, para bajar la temperatura.
- Al normalizarse, el ventilador **vuelve solo** a su control normal (auto o manual).
Implementado en la tarea `env_temp_task` ([environment_control.c](main/environment_control.c)).

### Cortina (motor DC)
- **Modo MANUAL:** apertura/velocidad 0–100 % desde el dashboard.
- **Modo AGENDA:** hasta **8 registros** programables (hora, minuto, apertura). La tarea
  `env_schedule_task` compara la hora actual y aplica el registro correspondiente.
  Requiere **modo STA** con hora sincronizada por **NTP**.

---

## 9. Conectividad WiFi y hora (NTP)

- **Modo AP (punto de acceso):** red **`ESP32`**, contraseña **`12345678`**, IP fija
  **`192.168.0.1`** ([wifi_app.h:19-25](main/wifi_app.h#L19-L25)).
- **Modo STA (estación):** se conecta a tu red local desde el dashboard. La IP la asigna el
  router y se imprime en el monitor serie: `IP del ESP32 en tu red: …`
  ([wifi_app.c:620](main/wifi_app.c#L620)).
- **Hora real (SNTP):** se sincroniza con `0.co.pool.ntp.org`. **Zona horaria de Colombia
  (UTC-5)** configurada con `TZ="<-05>5"` ([wifi_app.c:69](main/wifi_app.c#L69)).

---

## 10. Servidor HTTP y API REST

El ESP32 sirve el dashboard (HTML/CSS/JS embebidos) y expone esta API:

| Endpoint | Método | Función |
|----------|:------:|---------|
| `/` , `/app.css`, `/app.js`, `/jquery…` | GET | Dashboard web |
| `/api/state` | GET | Estado completo (temperatura, modos, RGB, agenda…) en JSON |
| `/api/curtain` | POST | Modo y apertura del motor de cortina |
| `/api/schedule` | POST | Programa un registro de la agenda |
| `/api/fan` | POST | Modo (auto/manual), velocidad y umbrales de temperatura |
| `/api/rgb` | POST | Color y brillo del LED RGB |
| `/wifiConnect.json` | POST | Conexión a la red WiFi (modo STA) |

Definidos en [http_server.c:1219-1391](main/http_server.c#L1219-L1391).

---

## 11. Persistencia (NVS)

El estado (`s_state`) se guarda en **NVS** (espacio `"env"`) cada vez que cambia una
configuración (`env_save_state()`), y se recarga al arrancar (`env_load_state()`). Así, los
modos, umbrales, colores y agenda **se conservan tras un reinicio**.

---

## 12. Arquitectura de archivos

```
main/
├── main.c                  → app_main(): inicializa NVS, hora, control ambiental y WiFi
├── environment_control.c/h → NTC, PWM (RGB/ventilador/motor), alarma, agenda, NVS
├── wifi_app.c/h            → WiFi AP+STA, máquina de estados, SNTP/hora
├── http_server.c/h         → Servidor web + API REST
├── rgb_led.c/h             → (módulo RGB auxiliar)
├── tasks_common.h          → Prioridades, stacks y núcleos de las tareas de red
└── webpage/                → Dashboard embebido (index.html, app.js, app.css)
```

Arranque ([main.c](main/main.c)):
```c
nvs_flash_init();      // 1. Memoria no volátil
init_obtain_time();    // 2. Prepara la sincronización de hora
env_control_start();   // 3. Lanza control ambiental (PWM, ADC, tareas)
wifi_app_start();      // 4. Lanza WiFi + servidor web
```

---

## 13. Compilación, particiones y flasheo

**Tabla de particiones personalizada** ([partitions.csv](partitions.csv)) porque el binario
(~1.2 MB) no cabía en la partición por defecto de 1 MB. La partición `factory` se amplió a
**1.94 MB** (flash de 2 MB).

```bash
# Cargar el entorno de ESP-IDF
source /Users/nicocasper/.espressif/v5.5.2/esp-idf/export.sh

# Compilar
idf.py -C /Users/nicocasper/Real_Time_Systems/PROYECTO build

# Flashear y abrir el monitor serie
idf.py -C /Users/nicocasper/Real_Time_Systems/PROYECTO -p <PUERTO> flash monitor
```

Para encontrar el puerto: `ls /dev/cu.*` (ej. `/dev/cu.usbserial-XXXX`).

---

## 14. Posibles preguntas del profesor (con respuestas)

**P: ¿Cuántas tareas tiene el sistema y con qué prioridades?**
R: 7 tareas. Las críticas (gestión WiFi `wifi_app_task`, lectura/control `env_temp_task`)
van a **prioridad 5**; el servidor HTTP y la agenda a **4**; el monitor del servidor a **3**.
Se reparten entre los 2 núcleos: red en el núcleo 0 y control ambiental en el núcleo 1.

**P: ¿Por qué usas mutex y dónde?**
R: Para evitar **condiciones de carrera** sobre datos compartidos entre tareas. `s_state_lock`
protege la estructura de estado (la escriben la tarea de temperatura y el servidor HTTP), y
`s_adc_lock` serializa el acceso al ADC. Sin mutex, una tarea podría leer un valor a medio
actualizar.

**P: ¿Cómo controlas la velocidad de los motores?**
R: Con **PWM** (módulo LEDC) a 25 kHz. El **ciclo de trabajo** (0–100 %) controla la potencia
media entregada. El ESP32 no mueve el motor directamente: usa un **MOSFET IRFZ44N** como
interruptor de lado bajo, con resistencia de *gate* (100 Ω), *pull-down* (10 kΩ) y diodo
*flyback*.

**P: ¿Por qué el diodo flyback?**
R: El motor es una carga **inductiva**; al cortar la corriente genera un pico de tensión
inverso que dañaría el MOSFET. El diodo lo recircula y lo absorbe.

**P: ¿Por qué PWM a 25 kHz y no más bajo?**
R: Por encima de ~20 kHz está **fuera del rango audible**, así el motor no produce un zumbido.

**P: ¿Cómo conviertes la lectura del ADC a temperatura?**
R: Divisor de tensión → resistencia del NTC `R = V·Rserie/(Vcc−V)` → temperatura con la
**ecuación de Steinhart-Hart simplificada (Beta):** `1/T = 1/T₀ + (1/β)·ln(R/R₀)`. Uso la
**calibración de fábrica** del ADC del ESP32 y un **filtro de mediana** de 64 muestras.

**P: ¿Por qué un filtro de mediana y no un promedio?**
R: La mediana **rechaza los valores atípicos** (picos de ruido del WiFi y de la alta impedancia
del NTC) sin que un solo pico arrastre el resultado, cosa que sí pasa con el promedio.

**P: ¿Cómo garantizas el tiempo real / periodicidad?**
R: Cada tarea periódica usa `vTaskDelay`/`vTaskDelayUntil` con su periodo, y las prioridades
aseguran que las tareas críticas se ejecuten primero. La afinidad de núcleo separa la red del
control para que el tráfico no introduzca *jitter* en el muestreo.

**P: ¿Qué pasa si se reinicia el ESP32?**
R: El estado (modos, umbrales, RGB, agenda) se **guarda en NVS** y se restaura al arrancar.

**P: ¿Cómo funciona la alarma?**
R: Si la temperatura supera `Temp máxima`, se activa la alarma: parpadea el LED y el
**ventilador se fuerza al 100 %** para enfriar, sin importar el modo; al bajar, vuelve a su
control normal.

**P: ¿Por qué la agenda necesita internet?**
R: Porque la hora se obtiene por **NTP** (modo STA). Sin red, el reloj no se sincroniza y la
agenda no tiene una referencia horaria válida.

**P: ¿Cómo se comunican las tareas con el servidor web?**
R: A través de la **estructura de estado compartida** protegida por mutex y de **colas**
(`queue`) para los eventos del WiFi y del servidor HTTP.

**P: ¿Por qué la resistencia serie es de 15 kΩ y el NTC de 10 kΩ?**
R: El NTC físico es un 10 kΩ/B3950. La resistencia serie de 15 kΩ fija el punto de trabajo del
divisor. Ambos valores deben coincidir en el firmware (`NTC_NOMINAL_RESISTANCE_OHMS = 10000`,
`NTC_SERIES_RESISTOR_OHMS = 15000`) o la conversión a temperatura sale desplazada.

---

## 15. Resumen de características

- ✅ 7 tareas FreeRTOS con prioridades y afinidad de núcleo definidas.
- ✅ Sincronización por mutex (estado + ADC).
- ✅ Medición de temperatura con NTC (Beta + calibración ADC + filtro de mediana).
- ✅ Control PWM de ventilador y motor con drivers MOSFET.
- ✅ Alarma por sobre-temperatura con forzado del ventilador.
- ✅ Agenda horaria (8 registros) sincronizada por NTP.
- ✅ Luz ambiente RGB configurable.
- ✅ Dashboard web + API REST servidos por el ESP32 (AP + STA).
- ✅ Persistencia en NVS.
