## Plan para `ADC_calibrate.c` (solo corrección de temperatura con 1 punto en arranque)

Como ya confirmaste:
- **Calibración en tiempo de arranque**
- **1 punto** (T_ref de celular, ~17°C)
- **Corrección por diferencia (offset de temperatura)**

Entonces el archivo `ADC_calibrate.c` debería hacer únicamente esto:

### 1) Qué calcula (la fórmula exacta)
1. En arranque lee el NTC → calcula la temperatura “modelo”:
- `T_model_at_boot = ntc_raw_to_celsius(raw_avg)`
2. Obtiene la referencia:
- `T_ref_c` (ej. 17.0 desde una constante)
3. Calcula un offset:
- `T_offset = T_ref_c - T_model_at_boot`
4. En runtime, para cualquier lectura:
- `T_corrected = T_model + T_offset`

Esto implementa tu opción 1:
**`T_corrected = T_model + (T_ref - T_model_at_1point)`**

### 2) Responsabilidad única del archivo
`ADC_calibrate.c` NO debería tocar LEDC ni la lógica de estados.
Solo expone:
- inicialización (calcula `T_offset`)
- función que aplica la corrección

### 3) APIs mínimas recomendadas (para que el resto del código sea simple)
- `void adc_calibrate_init(adc_oneshot_unit_handle_t h, adc_channel_t ntc_channel, float t_ref_c, int samples_avg);`
  - lee N muestras del NTC
  - usa **tu conversión Beta existente** (idealmente como función reutilizada o copiada/compartida) para obtener `T_model_at_boot`
  - guarda `T_offset`

- `float adc_calibrate_apply_temperature(float t_model_c);`
  - retorna `t_model_c + T_offset`

- (opcional para debug) `float adc_calibrate_get_offset(void);`

### 4) Qué necesitas decidir para integrarlo con tu proyecto actual
Como tu conversión Beta hoy vive en `main/ledc_basic_example_main.c` como:
- `static float ntc_raw_to_celsius(int raw)`

Para que `ADC_calibrate.c` sea “solo calibración”, tienes 2 caminos:
- **A (mejor ingeniería)**: mover `ntc_raw_to_celsius` a un lugar compartido (ej. `library_led_c` o un nuevo módulo de conversión), o declarar una función no-`static`.
- **B (rápido pero menos limpio)**: duplicar la función `ntc_raw_to_celsius` dentro de `ADC_calibrate.c` (manteniendo los mismos defines Beta y `NTC_CONNECTED_TO_GND`).

### 5) Integración en tu `temperature_rgb1_task`
En vez de:
- `temperature_c = ntc_raw_to_celsius(ntc_raw);`
haz:
1) `t_model = ntc_raw_to_celsius(ntc_raw)`
2) `temperature_c = adc_calibrate_apply_temperature(t_model)`

Con eso, el RGB1 se alimenta de la temperatura corregida.

---

## Siguiente paso necesario
Para que yo te ayude a **crear el archivo** `main/ADC_calibrate.c` y modificar los includes/llamadas donde corresponda, debes cambiar a **Act Mode** (porque en Explore Mode no puedo crear/editar archivos).

Si me confirmas que irás a Act Mode, te digo exactamente:
- qué funciones mover/duplicar,
- qué líneas tocar en `app_main()` para llamar a `adc_calibrate_init()`,
- y cómo modificar `temperature_rgb1_task()` para usar la temperatura corregida.