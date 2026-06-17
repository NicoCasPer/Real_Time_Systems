# final_debug/ — RFFT Scope HIL Debug Pipeline

Self-running Hardware-in-the-Loop testbench for the RFFT Scope FPGA pipeline.
No UART, no ESP32, no buttons — power on and the FPGA auto-cycles through
8 pre-computed test vectors (5 kHz sine + gaussian noise) displaying each
on the LCD for ~3 seconds before advancing.

```
                    dominio clk (27 MHz placa)
 debug_test_rom_player → B1 (FIFO+pack) → B2 (bit-reverse)
   → B4 complex_fft_core (B3: butterfly + twiddle ROM inside)
   → B5 rfft_recombine → B5 spectrum_buffer (escritura)
                    dominio clk_pix (40.5 MHz PLL)
 spectrum_buffer (lectura) → spectrum_draw → lcd_ctrl → LCD 800×480
```

No external MCU. The `debug_test_rom_player` replaces `uart_rx` entirely.

## Quick Start

```bash
# 1. Generate test vectors (requires numpy)
python3 scripts/gen_debug_vectors.py

# 2. Synthesize (requires Gowin EDA)
gw_sh final_debug/build_debug_rfft_scope.tcl

# 3. Flash
openFPGALoader -b tangprimer20k final_debug/debug_rfft_scope/impl/pnr/debug_rfft_scope.fs

# 4. Watch — no interaction needed
#    LCD cycles V0 → V1 → ... → V7 → V0 every ~24 seconds
```

## ⚠️ CAUSA DEL "no aparece el pico en 5 kHz" — ROM init en síntesis

**Diagnóstico (verificado en simulación):** el RTL es **correcto** — el
pipeline completo (player → B1 → B2 → B4 → recomb) produce un pico limpio en
**columna 107 (5 kHz)** con DC ≈ 0 para V0. El ruido + DC enorme que se ve en
el hardware viene **solo de que las twiddle ROMs (y/o la ROM del player) no se
inicializan en la síntesis de Gowin** → la mariposa opera con W=0/basura → la
FFT computa basura.

**Causa raíz:** los arrays de ROM usaban el atributo `(* ram_style="block" *)`,
que es sintaxis **Xilinx/Vivado** y **GowinSynthesis la IGNORA**. Sin un
atributo válido, Gowin mapeaba la ROM inicializada a flip-flops ("number of DFF
exceeds resource limit") o la dejaba sin inicializar (cero/basura) → FFT rota.

**FIX aplicado:** se cambió al atributo correcto de GowinSynthesis (estilo
Synplify, SUG550 §5.17) en `twiddle_rom.v` y `debug_test_rom_player.v`:

```verilog
reg [31:0] rom_fft [0:511] /* synthesis syn_romstyle="block_rom" */;
```

Esto fuerza el mapeo a BSRAM **y** conserva la inicialización por `$readmemh`
(GowinSynthesis ≥1.9.8 sí ejecuta `$readmemh` en síntesis). El TCL ya copia los
`.hex` dentro del árbol del proyecto para que la ruta resuelva (regla de ruta
SUG550). **No** dejar las ROMs en cero — eso da pantalla negra / FFT nula.

**Verificación tras `gw_sh`:** abrir el reporte de uso y confirmar que
`rom_fft` (512×32), `rom_recomb` (1025×32) y el `rom` del player (16384×16) se
mapearon a **BSRAM con contenido inicial** y que el conteo de DSP es ~4 (no
0.5 — un 0.5 indica twiddles en cero y FFT rota).

**Fallback (si tu versión de Gowin aún no inicializa la BSRAM inferida):**
generar las ROMs con el **IP Catalog (pROM)** cargando los `.mi` provistos
(`src/block3/twiddles_*.mi`, `src/debug_hex/debug_vectors.mi`) y reemplazar los
módulos de ROM por las instancias del IP.

> Nota: la tabla de recursos previa (DSP 0.5) correspondía al build con
> twiddles en cero — por eso la FFT salía mal. Con el fix el DSP sube a ~4.

## Test Vectors

| Vector | SNR | Description |
|---|---|---|
| V0 | inf dB | Clean 5 kHz sine, narrow peak |
| V1 | 41.4 dB | Tiny noise, bar slightly broader |
| V2 | 35.4 dB | Light noise |
| V3 | 29.1 dB | Moderate noise |
| V4 | 23.5 dB | Visible noise floor |
| V5 | 17.5 dB | High noise |
| V6 | 11.2 dB | Very noisy, broad peak |
| V7 | 5.2 dB | Near-white noise, wide bar |

All vectors use 5 kHz tone at amplitude 0.5 FS (avoids B4 saturation).
Sample rate: 48 kHz, 2048 samples per frame. Display shows 0–24 kHz
with the peak at column ~107 (5 kHz / 46.88 Hz/px ≈ 107).

## Simulation

```bash
cd final_debug

# Debug ROM player unit test
iverilog -g2012 -o tb/tb_debug_player.vvp \
    tb/tb_debug_player.v src/debug_test_rom_player.v && vvp tb/tb_debug_player.vvp

# Full pipeline check (compilation only, PLL is Gowin primitive)
# Use final/tb/ for full E2E with UART stimulus
```

## File Structure

| File | Purpose |
|---|---|
| `src/debug_test_rom_player.v` | **New.** ROM-based autonomous sample injector, auto-cycles 8 vectors. Fixes: `syn_romstyle` BSRAM attr + off-by-one (ahora emite rom[0..2047], antes saltaba la muestra 0 y colaba 1 muestra del vector siguiente) |
| `src/debug_block1_i2s_top.v` | **Modified.** B1 top with ROM player instead of UART |
| `src/debug_rfft_scope_top.v` | **Modified.** Full pipeline top, no uart_rx port |
| `src/debug_rfft_scope.cst` | **New.** Pin constraints (no uart_rx, current_vector on spare GPIOs) |
| `src/block1/*.v` | Copied from `final/` (except `block1_i2s_top.v` which is replaced by debug variant) |
| `src/block2-5/*.v` | Copied from `final/` (identical). B5 files have optional `current_vector` overlay |
| `src/lcd/` | Copied from `final/` (identical) |
| `src/block3/` | Copied from `final/` (identical — twiddle ROMs + butterfly) |
| `src/debug_hex/` | Generated test vector files (`debug_vectors.hex`, metadata) |
| `scripts/gen_debug_vectors.py` | **New.** Python generator for test vectors |
| `tb/tb_debug_player.v` | **New.** Testbench for ROM player |
| `build_debug_rfft_scope.tcl` | **New.** Gowin build script |

## Differences from final/

| `final/` | `final_debug/` |
|---|---|
| UART input from ESP32 | ROM-based test vectors |
| `uart_rx` → `sample_fifo` | `debug_test_rom_player` → `sample_fifo` |
| `rfft_scope_top` with `uart_rx` port | `debug_rfft_scope_top` without UART |
| CSC with `uart_rx` pin | CSC with `current_vector[2:0]` monitor pins |
| External ESP32+MAX9814 required | Self-running, no external hardware |
| Manual test vector selection | Auto-cycles V0→V7 every ~3s |

## Regenerating Test Vectors

```bash
# Change amplitude, frequency, noise levels in the script:
python3 scripts/gen_debug_vectors.py

# Edit gen_debug_vectors.py to tune:
#   FS, F_TONE, N_SAMPLES, AMPLITUDE, NUM_VECTORS, NOISE_SIGMAS[]
#   Then re-run synthesis.
```

## LCD Overlay

The top-left corner shows "V0" through "V7" indicating the current test vector.
This is implemented in `src/block5/spectrum_draw.v` (modified copy with
`current_vector` input port and 5×7 font overlay).

## Pin Assignments (Tang Primer 20K Dock)

| Signal | Pin | Note |
|---|---|---|
| `clk` | H11 | Board oscillator 27 MHz |
| `rst_n` | T10 | Onboard button (active-low) |
| `fifo_overflow` | L16 | LED (should stay off) |
| `frame_dropped` | L14 | LED (should stay off) |
| `current_vector[2]` | N14 | Debug monitor pin |
| `current_vector[1]` | M11 | Debug monitor pin |
| `current_vector[0]` | R3 | Debug monitor pin |
| `uart_rx` | — | **Not connected** (T13 free) |
| LCD pins | (same as final/) | 800×480 RGB panel |

If `current_vector[*]` pins conflict with your Dock, remove those lines
from `debug_rfft_scope.cst` — the overlay still works on LCD.
