Entendido: usarás un “baño térmico” casero (candela) y tomarás como referencia **≈ temperatura ambiente medida con el celular (17 °C)**.

## Punto clave (importante para que la calibración sea válida)
Con esa estrategia, **solo tienes 1 punto confiable** (17 °C). Para calibrar con un modelo de corrección tipo:
- `raw_corrected = gain * raw + offset`
necesitas **al menos 2 puntos** (por ejemplo 17 °C y un segundo valor mientras subes temperatura).

Si usas solo 17 °C, lo único que puedes hacer de forma razonable es estimar un **offset** (y aun así sería débil porque el ADC también tiene ganancia/escala).

Además, “temperatura del celular” suele tener errores (el sensor del móvil no mide exactamente lo mismo que el NTC y tarda en estabilizar).

## Qué puedes calibrar exactamente en ESP32 (y cómo encaja con tu pedido)
Cuando dices “calibrar el ADC según la temperatura medida por el NTC”, en la práctica lo más coherente es:
1) Usar el **ADC del NTC** para estimar la temperatura (tu función Beta).
2) Calcular cómo debería “ajustarse” el ADC (o el raw) para que, a cierta temperatura de referencia, el resultado cuadre.

Eso implica que `ADC_calibrate.c` (como archivo extra) debería:
- Leer ADC del NTC (canal 6).
- Convertir raw→temperatura con tu fórmula Beta actual (o temporalmente usando el modelo actual).
- Ajustar parámetros de corrección (gain/offset) para el raw o para la conversión.

## Diseño recomendado para `ADC_calibrate.c` (para tu caso)
### Opción A (la más simple): calibración 2 puntos → `gain/offset` sobre raw
- Registrar:
  - Punto 1: `T1_ref` (por ejemplo 17 °C con celular) y `raw1` del NTC (promediado).
  - Punto 2: `T2_ref` (otra lectura de celular cerca a estabilizar, por ejemplo 30 °C o 40 °C) y `raw2`.
- Calcular:
  - `gain = (T2_ref - T1_ref) / (T_model2 - T_model1)` **si corriges la temperatura**
  - o `offset/gain` **si corriges raw** (depende de qué quieras “corregir”).

**Recomendación realista:** en tu proyecto actual es más directo corregir la **temperatura calculada** (porque el NTC ya llega a °C por Beta). O sea, calibras la *salida* de tu conversión, no el ADC físico.

### Opción B (más “ADC-like”): corregir raw antes de convertir
Sería:
- `raw_corrected = a * raw + b`
- luego `raw_corrected` entra en el modelo Beta.

Requiere la misma idea de 2 puntos, pero el ajuste matemático se hace en el dominio raw.

## Cómo lo usarías con tu “candela” (protocolo práctico)
1) Espera a que el NTC esté realmente a **ambiente** y estabilizado.
2) Mide con celular **T1_ref**.
3) Registra `raw1` (promedio, p.ej. 16 muestras como ya haces).
4) Calienta suavemente con la candela hasta un segundo rango (no quemar el NTC).
5) Espera estabilización y registra `T2_ref` en el celular (aunque imperfecto).
6) Ejecuta calibración (calcula parámetros).
7) En el resto del programa usa la corrección.

## Lo que necesito para afinar el diseño (sin tocar código aún)
Para decirte exactamente qué API tendría `ADC_calibrate.c` y qué variables almacenar:
1) ¿Quieres corregir **temperatura** (°C) o **raw ADC**?
2) ¿Puedes obtener un **segundo punto** además de 17 °C? (ej. 30 °C o 40 °C del celular)
3) ¿Usarás la calibración en tiempo de arranque (una vez) o la recalibras durante el runtime?

Si me respondes esas 3, te dejo un esquema concreto de funciones y estructura (sin implementar) para que cuando pases a Act Mode puedas crear `main/ADC_calibrate.c` y conectarlo a `ledc_basic_example_main.c`.