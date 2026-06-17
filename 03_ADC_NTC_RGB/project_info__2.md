# Respuesta (calibración ADC vs conversión con pot+NTC)

## ¿Se puede calibrar el ADC con potenciómetro y NTC?
**Se puede hacer una calibración práctica del sistema de medida**, pero **no en el sentido de “calibrar el ADC interno del ESP32 automáticamente”** con pot+NTC.  
En este proyecto, lo que puedes ajustar/calibrar es la cadena:

**ADC raw → resistencia del NTC (por divisor) → temperatura con modelo Beta**.

## Cómo hacerlo usando lo que ya tienes en el código

### 1) Lo más crítico: orientación del divisor (parámetro `NTC_CONNECTED_TO_GND`)
En `main/ledc_basic_example_main.c` tienes:
- `#define NTC_CONNECTED_TO_GND 1`

Eso indica que el programa asume el divisor como (según el comentario del código):
- **3.3V → R fija (10k) → nodo ADC → NTC → GND**

Si tu divisor está al revés (por ejemplo, **NTC hacia 3.3V y R fija hacia GND**), entonces cambia la lógica y deberías usar:
- `NTC_CONNECTED_TO_GND 0`

Si no coincide, la temperatura calculada queda “invertida” o muy errónea.

### 2) Verifica que el ADC está leyendo bien usando el potenciómetro
El código ya imprime lecturas iniciales del ADC:
- `print_initial_adc_check()` imprime el **raw del NTC** y el **raw del potenciómetro**.

Usa el potenciómetro para confirmar que:
- el raw cambia de forma continua al mover el cursor,
- no está saturado todo el tiempo (siempre muy cerca de 0 o de 4095),
- tu circuito de referencia/masa está bien.

> Esto no calibra °C directamente, pero sí valida el comportamiento ADC–voltaje relativo.

### 3) Calibración para temperatura: ajusta parámetros del modelo del NTC (Beta/R0)
Para que la **temperatura calculada** coincida con la real, normalmente ajustas:
- `NTC_NOMINAL_RESISTANCE_OHMS` (R0)
- `NTC_BETA_COEFFICIENT` (B)

Mejor método (si tienes un termómetro de referencia):
1. Pon el montaje en una temperatura conocida (ej. ~25°C).
2. Registra `ntc_raw`.
3. Repite en otro punto (~40–50°C).
4. Ajusta R0/B (o al menos B) para que la curva del modelo coincida con tu NTC real.

## Resumen directo
- ✅ **Sí**: pot + NTC + divisor te permiten **validar/calibrar el modelo de conversión** (y confirmar que el montaje está bien).
- ❌ **No**: no se calibra “el ADC interno” del ESP32 de forma automática con pot+NTC; el ADC ya devuelve un raw calibrado internamente por el driver/atten/bitwidth.

## Pregunta para afinarlo
¿Tu divisor del NTC está cableado como el comentario (resistencia fija a GND, NTC hacia 3.3V) o está al revés?  
Dímelo y te digo exactamente qué valor debería tomar `NTC_CONNECTED_TO_GND` y qué comportamiento deberías ver en los `ntc_raw` al calentar/enfriar.