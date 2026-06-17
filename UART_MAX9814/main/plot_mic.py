import sys
import argparse
from collections import deque
import threading

import serial
import serial.tools.list_ports
import matplotlib.pyplot as plt
import matplotlib.animation as animation
import pyaudio # Importar PyAudio
import numpy as np # Importar NumPy para manejo de arrays de audio

WINDOW_SIZE_DEFAULT = 300
BAUD_RATE_DEFAULT = 115200

# --- Configuración de audio para PyAudio ---
AUDIO_SAMPLE_RATE = 48000  # Coincide con SAMPLE_RATE en tu código C
AUDIO_FORMAT = pyaudio.paInt16 # Q15 se mapea a un entero de 16 bits con signo
AUDIO_CHANNELS = 1         # Micrófono mono
AUDIO_CHUNK_SIZE = 256    # Número de muestras por escritura al stream de audio
                           # Reducido para que la reproducción empiece antes
                           # (ajusta según latencia/estabilidad)

# Variables globales para PyAudio
p = None
audio_stream = None
audio_playback_buffer = deque() # Búfer para acumular muestras antes de reproducir
audio_lock = threading.Lock()


def pyaudio_callback(in_data, frame_count, time_info, status):
    """Callback para PyAudio que entrega exactamente `frame_count` muestras.
    Lee desde `audio_playback_buffer` bajo lock y rellena con ceros si hacen falta.
    """
    global audio_playback_buffer, audio_lock
    samples = []
    with audio_lock:
        n = min(len(audio_playback_buffer), frame_count)
        for _ in range(n):
            samples.append(audio_playback_buffer.popleft())
    if len(samples) < frame_count:
        samples.extend([0] * (frame_count - len(samples)))
    out = np.array(samples, dtype=np.int16)
    return (out.tobytes(), pyaudio.paContinue)

def list_serial_ports():
    return list(serial.tools.list_ports.comports())

def choose_port_auto(preferred_prefix="/dev/cu."):
    ports = list_serial_ports()
    # Prefer devices that start with /dev/cu. (típico en macOS)
    for p in ports:
        if p.device.startswith(preferred_prefix):
            return p.device
    # Fallback: return first available
    return ports[0].device if ports else None

def open_serial_port(port, baud):
    try:
        ser = serial.Serial(port, baud, timeout=1)
        print(f"Conectado a {port} @ {baud} baudios")
        return ser
    except serial.SerialException as e:
        raise

def parse_line(line):
    try:
        return int(line.strip())
    except ValueError: # Capturar ValueError en lugar de Exception genérica para conversiones a int
        return None

def update(frame, ser, plot_data_deque, line, ax, debug_mode):
    global audio_playback_buffer # Acceder al búfer global

    # Read available lines
    try:
        while ser.in_waiting:
            raw = ser.readline().decode(errors="ignore")
            value = parse_line(raw)
            if value is not None:
                # Añadir para la visualización
                plot_data_deque.append(value)

                # Añadir para la reproducción de audio (thread-safe)
                with audio_lock:
                    audio_playback_buffer.append(value)

                if debug_mode:
                    print(value) # Imprimir si el modo debug está activo

                # Añadir al búfer; la escritura la realiza un hilo separado

    except Exception as e:
        # Esto capturaría errores como SerialException si el puerto se desconecta
        print(f"Error en la lectura serial: {e}")
        pass # No detener la animación completamente si hay un error menor

    # Actualizar la línea del gráfico
    vals = list(plot_data_deque)
    line.set_ydata(vals)

    # Optional dynamic y-limits for plotting
    if vals:
        minh = min(vals)
        maxh = max(vals)
        # Asegurarse de que los límites del gráfico no sean demasiado estrechos si todos los valores son iguales
        if maxh - minh > 0:
            ax.set_ylim(minh - 10, maxh + 10)
        else: # Si todos los valores son iguales, dar un rango predeterminado
            ax.set_ylim(minh - 500, minh + 500) # Ajusta este rango según tus datos
    else:
        ax.set_ylim(-32768, 32767) # Rango por defecto para Q15

    return line,

def main():
    global p, audio_stream # Acceder a las variables globales de PyAudio

    parser = argparse.ArgumentParser(description="Plot ADC samples from ESP32 serial and play audio")
    parser.add_argument("--port", help="Serial port (e.g. /dev/cu.SLAB_USBtoUART)")
    parser.add_argument("--baud", type=int, default=BAUD_RATE_DEFAULT, help="Baud rate")
    parser.add_argument("--window", type=int, default=WINDOW_SIZE_DEFAULT, help="Samples window size for plotting")
    parser.add_argument("--debug", action="store_true", help="Print incoming values to stdout for debugging")
    args = parser.parse_args()

    # --- Configuración del puerto serial ---
    port = args.port
    if not port:
        detected = choose_port_auto()
        if detected:
            print(f"Puerto detectado automáticamente: {detected}")
            port = detected
        else:
            print("No se detectaron puertos seriales. Conecta el ESP32 y vuelve a intentarlo.")
            sys.exit(1)

    try_ports = [port]
    for p_info in list_serial_ports(): # 'p_info' para evitar conflicto con la variable global 'p' de pyaudio
        if p_info.device not in try_ports and p_info.device.startswith("/dev/cu."):
            try_ports.append(p_info.device)

    ser = None
    for p_device in try_ports: # 'p_device' para evitar conflicto
        try:
            ser = open_serial_port(p_device, args.baud)
            port = p_device
            break
        except serial.SerialException as e:
            print(f"No se puede abrir {p_device}: {e}")
            continue

    if ser is None:
        print("No fue posible abrir ningún puerto serial. Ejecuta `lsof /dev/tu_puerto` para ver quién lo usa, o cierra otras apps (screen, minicom, Arduino IDE, esptool, etc.).")
        sys.exit(1)

    # --- Inicialización de PyAudio ---
    p = pyaudio.PyAudio()
    try:
        audio_stream = p.open(format=AUDIO_FORMAT,
                      channels=AUDIO_CHANNELS,
                      rate=AUDIO_SAMPLE_RATE,
                      output=True,
                      frames_per_buffer=AUDIO_CHUNK_SIZE,
                      stream_callback=pyaudio_callback) # usar callback para menor latencia
        print(f"Stream de audio PyAudio abierto a {AUDIO_SAMPLE_RATE} Hz (callback).")
    except Exception as e:
        print(f"Error al inicializar PyAudio o abrir el stream: {e}")
        ser.close()
        sys.exit(1)

    # --- Configuración de Matplotlib ---
    plot_data_deque = deque([0] * args.window, maxlen=args.window)

    fig, ax = plt.subplots()
    line, = ax.plot(range(args.window), list(plot_data_deque), lw=1)
    ax.set_xlim(0, args.window - 1)
    ax.set_ylim(-32768, 32767) # Rango completo para Q15
    ax.set_title("Micrófono MAX9814 - Señal Q15")
    ax.set_xlabel("Muestras")
    ax.set_ylabel("Valor Q15")
    ax.grid(True)

    # La función 'update' ahora manejará tanto el ploteo como la reproducción de audio
    ani = animation.FuncAnimation(fig, update, fargs=(ser, plot_data_deque, line, ax, args.debug),
                                  interval=50, blit=False, cache_frame_data=False) # cache_frame_data=False para mejor rendimiento con datos en tiempo real

    plt.tight_layout()
    try:
        plt.show()
    finally:
        # --- Limpieza al cerrar ---
        print("Cerrando puertos y streams...")
        if ser:
            ser.close()
        # No hay hilo de audio cuando se usa callback; sólo seguir con el vaciado y cierre

        # Si quedan muestras en el búfer de reproducción, enviarlas antes de cerrar
        try:
            with audio_lock:
                if audio_playback_buffer and audio_stream:
                    remaining = np.array(list(audio_playback_buffer), dtype=np.int16)
                    try:
                        audio_stream.write(remaining.tobytes())
                    except Exception:
                        pass
                    audio_playback_buffer.clear()
        except NameError:
            # audio_playback_buffer no definido o similar; ignorar
            pass

        if audio_stream:
            try:
                audio_stream.stop_stream()
            except Exception:
                pass
            try:
                audio_stream.close()
            except Exception:
                pass
        if p:
            p.terminate()

if __name__ == "__main__":
    main()