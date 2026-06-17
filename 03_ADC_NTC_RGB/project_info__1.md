# ESP32 — ADC + NTC + Potenciómetro + RGB (LEDC) — Codebase Overview

## Summary
Este proyecto (ESP-IDF en C) lee dos señales analógicas con el ADC1 del ESP32: un **NTC** (GPIO34/ADC1_CH6) y un **potenciómetro** (GPIO35/ADC1_CH7). Con el valor del NTC calcula temperatura usando el modelo **Beta** y muestra un color en un RGB (RGB1). Con el potenciómetro, y usando un **botón con debounce** (GPIO16), controla una **máquina de estados** para configurar y luego “congelar” el color en el segundo RGB (RGB2) mediante PWM con **LEDC**.

## Architecture
- **Patrón**: aplicación bare-metal con **FreeRTOS** y dos tareas concurrentes (modelo “task-based / concurrent loop”).
- **Tecnología**:
  - Lenguaje: **C**
  - Framework: **ESP-IDF**
  - ADC: `esp_adc/adc_oneshot.h` (OneShot driver)
  - PWM: `driver/ledc.h`
  - RTOS: `freertos/*`
- **Punto de entrada**: `app_main()` en `main/ledc_basic_example_main.c`.
- **Ejecución**:
  1. Inicializa mutex ADC.
  2. Configura ADC y LEDC.
  3. Configura botón (pull-up interno).
  4. Imprime lecturas crudas iniciales por consola.
  5. Crea dos tareas:
     - `temperature_rgb1_task()` cada 1000 ms: lee NTC → convierte a °C → elige color.
     - `rgb2_state_machine_task()` cada 20 ms: lee potenciómetro en estados de configuración → guarda valores al pulsar → muestra color guardado.

## Directory Structure
```text
project-root/
├── main/
│   ├── CMakeLists.txt
│   ├── ledc_basic_example_main.c   — App principal: ADC (NTC/pot) + LEDC + FreeRTOS + FSM + botón
│   ├── library_led_c.c             — Helpers LEDC (configurar canales y setear duty)
│   └── library_led_c.h
├── README.md
└── (build/, sdkconfig*, etc.)
```

## Key Abstractions

### `app_main()`
- **File**: `main/ledc_basic_example_main.c` (función `app_main`)
- **Responsabilidad**: inicializar periféricos (ADC, LEDC, GPIO botón), preparar estado compartido (mutex) y arrancar tareas.
- **Interface**: no expone API; orquesta el arranque.
- **Lifecycle**: ejecuta una vez al boot.
- **Used by**: FreeRTOS/ESP-IDF runtime (entry point de la app).

### `configure_adc()`
- **File**: `main/ledc_basic_example_main.c`
- **Responsabilidad**: crear unidad ADC1 (oneshot) y configurar bitwidth/attenuación para canales del ADC.
- **Detalles relevantes**:
  - Bitwidth: `ADC_BITWIDTH_12`
  - Atenuación: `ADC_ATTEN_DB_11` (para leer casi 0–3.3V con más rango).
- **Used by**: `app_main()`.

### `read_adc_raw_average(adc_channel_t channel, int samples)`
- **File**: `main/ledc_basic_example_main.c`
- **Responsabilidad**: leer múltiples muestras y promediar para reducir ruido.
- **Interface**:
  - `channel`: canal ADC (6 o 7)
  - `samples`: número de lecturas a promediar
- **Diseño no-obvio**:
  - Usa `s_adc_mutex` para que ADC no sea accedido simultáneamente por dos tareas.
- **Used by**:
  - `temperature_rgb1_task()` (NTC)
  - `rgb2_state_machine_task()` (potenciómetro)

### `adc_raw_to_pwm_8bit(int raw)`
- **File**: `main/ledc_basic_example_main.c`
- **Responsabilidad**: mapear el valor crudo ADC (0..4095) a duty LEDC 8-bit (0..255), con clamp.
- **Cómo se interpreta**:
  - Luego ese duty se pasa a `set_rgb_pwm()` para generar luz PWM.
- **Used by**: `rgb2_state_machine_task()` en estados de configuración.

### `ntc_raw_to_celsius(int raw)`
- **File**: `main/ledc_basic_example_main.c`
- **Responsabilidad**: convertir ADC crudo del NTC a temperatura en °C usando:
  - un divisor resistivo
  - modelo **Beta** (`NTC_BETA_COEFFICIENT`)
- **Invariantes/chequeos**:
  - Evita divisiones por cero “forzando” raw a [1..4094].
  - Usa `NTC_CONNECTED_TO_GND` para escoger si el NTC está hacia GND o hacia 3.3V.
- **Used by**: `temperature_rgb1_task()`.

### `temperature_rgb1_task(void *arg)`
- **File**: `main/ledc_basic_example_main.c`
- **Responsabilidad**: loop periódico para NTC→temperatura→RGB1 (selección de color por umbrales).
- **Lógica de negocio** (no trivial):
  - Si `< 25°C` → Azul
  - Si `25°C..35°C` → Verde
  - Si `> 35°C` → Rojo
- **Used by**: `xTaskCreate` desde `app_main()`.

### `rgb2_state_machine_task(void *arg)`
- **File**: `main/ledc_basic_example_main.c`
- **Responsabilidad**:
  - Mantener un **FSM** que configura R, luego B, luego G usando el potenciómetro,
  - y al pulsar botón guarda el valor (como duty 8-bit) del color activo.
  - En `SHOW_COLOR`, muestra el color guardado sin seguir leyendo pot.
- **Interacción con botón**:
  - Usa `button_pressed_event()` para debounce.
  - Cada pulsación valida guarda `current_pwm` del canal activo.
- **Used by**: `xTaskCreate` desde `app_main()`.

### `button_pressed_event(debounce_button_t *button)`
- **File**: `main/ledc_basic_example_main.c`
- **Responsabilidad**: debounce por tiempo y detección de flanco hacia el nivel presionado.
- **Diseño no-obvio**:
  - Registra el tick cuando cambia la muestra y solo confirma cuando el cambio se mantiene >= `BUTTON_DEBOUNCE_TIME_MS`.

### Helpers LEDC (`library_led_c.c/.h`)
- **File**: `main/library_led_c.c`, `main/library_led_c.h`
- **Responsabilidad**: abstracciones genéricas para configurar/setear RGB mediante structs.
- **Nota importante**: en `ledc_basic_example_main.c` **no se usan** estas funciones directamente; ahí se implementa `configure_ledc_pwm()`, `set_rgb_pwm()` y utilidades similares.
  - Esto sugiere que `library_led_c.*` es una librería de apoyo para ejercicios, pero el “main” actual opera con código propio.

## Data Flow
1. **Boot** → `app_main()` configura periféricos:
   - ADC oneshot (bitwidth/atten)
   - LEDC timer + 6 canales (dos RGB con 3 canales cada uno)
   - GPIO botón con pull-up
2. **Lecturas ADC sincronizadas**:
   - `read_adc_raw_average()` bloquea `s_adc_mutex` mientras hace varias lecturas consecutivas.
3. **NTC (RGB1)**:
   1) `temperature_rgb1_task()` lee NTC (`ADC_CHANNEL_6`) con promedio (16)
   2) `ntc_raw_to_celsius()` calcula resistencia del NTC desde el divisor
   3) Aplica ecuación Beta para obtener temperatura en °C
   4) Selecciona un color RGB y llama `set_rgb_pwm()`
4. **Potenciómetro (RGB2) con FSM**:
   1) `rgb2_state_machine_task()` revisa el estado actual
   2) En estados `CONFIG_RED/BLUE/GREEN` lee pot (`ADC_CHANNEL_7`) con promedio (16)
   3) `adc_raw_to_pwm_8bit()` convierte ADC→duty 8-bit
   4) Mientras no se pulse, actualiza el color del canal activo
   5) Al detectar `button_pressed_event()` guarda `current_pwm` en `saved_red/blue/green`
   6) En `SHOW_COLOR` deja de leer pot y renderiza el color guardado

## Non-Obvious Behaviors & Design Decisions
- **“Calibración” vs “conversión a temperatura”**: el código NO realiza calibración automática por hardware; usa un modelo teórico (Beta + divisor) con parámetros fijos. Si cambia tu NTC real (tolerancia, valor nominal, B real) o cambia el montaje del divisor, la temperatura calculada tendrá error.
- **Selección de orientación del divisor con `NTC_CONNECTED_TO_GND`**:
  - Si el cableado real es al revés (NTC hacia 3.3V en vez de hacia GND), debes cambiar este `#define` o la conversión dará temperaturas “invertidas” (o muy incorrectas).
- **Mutex ADC**:
  - Hay dos tareas que leen ADC; el mutex evita lecturas concurrentes que pueden corromper el flujo o causar resultados inconsistentes.
- **Mapeo ADC→PWM con clamp y “offset”**:
  - `adc_raw_to_pwm_8bit()` no usa Vref explícita; asume linealidad 0..4095. En cambio, el driver ADC ya aplica su calibración/escala interna según atten/bitwidth.
- **Botón con pull-up y `BUTTON_PRESSED_LEVEL = 0`**:
  - “Pulsar” significa llevar el pin a GND; si tu cableado usa el otro nivel, habrá que ajustar esa constante o se invertirá el evento.
- **Doble implementación de LEDC**:
  - Existe `library_led_c.*`, pero el `main` usa implementaciones directas. Un ingeniero nuevo podría confundirse creyendo que la librería es la base del proyecto.

## Respuesta a tu pregunta (ADC calibración con potenciómetro y NTC)
### ¿“Se puede” calibrar el ADC de ESP32 con potenciómetro y NTC midiendo divisor?
- **En este código “sí se hace” una calibración práctica del *sistema de medida* (divisor + conversiones) a nivel de software**, pero **no calibra el ADC internamente** (offset/gain del ADC) de manera automática.
- El programa:
  - Usa el **modelo del divisor resistivo** (NTC + resistencia serie conocida) y el modelo Beta para convertir ADC→°C.
  - Para el potenciómetro, simplemente usa ADC→PWM para controlar brillo/color, asumiendo relación lineal ADC~V.

### Cómo lo harías en la práctica (para que la lectura tenga sentido)
1. **Asegura el divisor correcto**:
   - El código asume por defecto:
     - `3.3 V --- resistencia fija (10k) --- ADC(GPIO34) --- NTC --- GND`
   - Si tu montaje es el contrario (NTC arriba y resistencia fija a GND), cambia:
     - `#define NTC_CONNECTED_TO_GND 0` (en vez de 1).
2. **Usa el potenciómetro como referencia de voltaje**:
   - Si conectas el potenciómetro como divisor y varías la posición, puedes observar **raw ADC** en consola (lo imprime en `print_initial_adc_check()` para pot).
   - Con mediciones reales de voltaje (con multímetro) puedes estimar si tu conversión “raw→V” te está cuadrando. En este proyecto el cálculo de °C no usa V directamente; usa raw para calcular resistencia del NTC, que depende de que el divisor esté bien.
3. **“Calibración” para temperatura (NTC)**:
   - Lo más efectivo suele ser ajustar parámetros del modelo:
     - `NTC_SERIES_RESISTOR_OHMS` (resistencia fija del divisor real)
     - `NTC_NOMINAL_RESISTANCE_OHMS` (valor nominal del NTC real)
     - `NTC_BETA_COEFFICIENT` (B real del datasheet, si difiere)
   - Si tienes el datasheet exacto de tu NTC, normalmente ya basta con poner esos valores.
   - Si no, puedes ajustar el modelo comparando:
     - temperatura real medida con termómetro + raw ADC del NTC en 2 puntos (por ejemplo ~25°C y ~50°C), y recalcular/ajustar B (o ajustar R0).
4. **Punto clave**: “calibrar el ADC” (offset/gain del ADC del chip) es distinto de “calibrar la conversión ADC→temperatura”.
   - Este proyecto está mejor alineado con el segundo enfoque (la conversión).

## Module Reference
| File | Purpose |
|------|---------|
| `main/ledc_basic_example_main.c` | App completa: ADC (NTC + pot), conversión, FSM de botón, PWM RGB por LEDC |
| `main/library_led_c.c` | Librería helper para configurar/setear RGB con structs (no usada por el main actual) |
| `main/library_led_c.h` | Declara structs y APIs de la librería helper |

## Suggested Reading Order
1. `main/ledc_basic_example_main.c` — empieza por `app_main()` para entender inicialización y tareas.
2. `main/ledc_basic_example_main.c` — lee `configure_adc()` y `read_adc_raw_average()` para entender cómo se miden/filtran los valores.
3. `main/ledc_basic_example_main.c` — lee `ntc_raw_to_celsius()` para ver qué parámetros gobiernan la “calibración” de temperatura.
4. `main/ledc_basic_example_main.c` — lee `rgb2_state_machine_task()` y `button_pressed_event()` para entender el flujo del pot + botón.
5. `main/library_led_c.h/.c` — como referencia de estilo/structs, pero no asumir que son el camino principal del main.
