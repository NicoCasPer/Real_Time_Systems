#!/usr/bin/env python3
# host/save_audio.py
# Recibe el stream de audio del ESP32 por UART y lo guarda como .wav para
# escucharlo en el PC. Por defecto guarda en ~/Downloads con marca de tiempo.
#
# Uso típico (autodetecta el puerto USB-serie):
#   python3 host/save_audio.py
# O indicando puerto y salida:
#   python3 host/save_audio.py -p /dev/cu.usbserial-1220 -o mi_audio.wav

import argparse
import glob
import os
import struct
import sys
import time
import wave

try:
    import serial
except Exception:
    print("Falta pyserial. Instálalo con: pip install pyserial")
    sys.exit(1)

MAGIC = b'ESPMIC01'
HDR_LEN = 8 + 4 + 4 + 2 + 2  # magic + sample_rate + num_samples + bits + channels


def autodetect_port():
    """Devuelve el primer /dev/cu.usbserial* o /dev/cu.usbmodem* disponible."""
    candidates = sorted(glob.glob('/dev/cu.usbserial*') +
                        glob.glob('/dev/cu.usbmodem*') +
                        glob.glob('/dev/ttyUSB*'))
    return candidates[0] if candidates else None


def default_out():
    downloads = os.path.expanduser('~/Downloads')
    if not os.path.isdir(downloads):
        downloads = os.getcwd()
    stamp = time.strftime('%Y%m%d_%H%M%S')
    return os.path.join(downloads, f'esp32_audio_{stamp}.wav')


parser = argparse.ArgumentParser(description='Recibe el stream de audio del ESP32 y guarda un WAV')
parser.add_argument('--port', '-p', default=None, help='Puerto serie (autodetecta si se omite)')
parser.add_argument('--baud', '-b', type=int, default=115200, help='Baudios (debe coincidir con el ESP32)')
parser.add_argument('--out', '-o', default=None, help='Ruta del WAV de salida (por defecto ~/Downloads con fecha)')
parser.add_argument('--timeout', '-t', type=float, default=30.0, help='Segundos a esperar el header antes de abortar')
parser.add_argument('--reset', '-r', action='store_true',
                    help='Reinicia el ESP32 (pulso DTR/RTS) al abrir para arrancar la grabación sin tocar el botón')
args = parser.parse_args()

port = args.port or autodetect_port()
if port is None:
    print('No se encontró ningún puerto serie. Conecta el adaptador USB-TTL e indica -p /dev/cu.usbserial-XXXX')
    sys.exit(1)

out_path = args.out or default_out()

print('Abriendo puerto serie:', port, '@', args.baud, 'baudios')
ser = serial.Serial(port, args.baud, timeout=1)

if args.reset:
    # Secuencia de auto-reset estilo esptool: EN=low y luego release, con GPIO0
    # alto para arrancar la app (no el bootloader). DTR->GPIO0, RTS->EN.
    print('Reiniciando el ESP32 para arrancar la grabación...')
    ser.setDTR(False)   # GPIO0 = alto -> boot normal (no descarga)
    ser.setRTS(True)    # EN = bajo -> reset asertado
    time.sleep(0.15)
    ser.setRTS(False)   # EN = alto -> la placa arranca y empieza a grabar
    ser.reset_input_buffer()

print('Puerto abierto. Esperando cabecera del ESP32...')
print('(Si no llega nada, pulsa el botón RESET/EN del ESP32 para reiniciar la grabación.)')

# --- Leer y sincronizar la cabecera ---
buf = bytearray()
start = time.time()
while len(buf) < HDR_LEN:
    b = ser.read(HDR_LEN - len(buf))
    if b:
        buf.extend(b)
    elif time.time() - start > args.timeout:
        print('Timeout esperando la cabecera. ¿Está el ESP32 enviando por este puerto?')
        ser.close()
        sys.exit(2)

# Si el comienzo no es el magic (p.ej. llegaron logs antes), resincronizar.
if bytes(buf[:8]) != MAGIC:
    idx = buf.find(MAGIC)
    while idx < 0:
        if time.time() - start > args.timeout:
            print('No se encontró la cabecera ESPMIC01. Abortando.')
            ser.close()
            sys.exit(3)
        b = ser.read(64)
        if b:
            buf.extend(b)
            idx = buf.find(MAGIC)
    buf = buf[idx:]
    while len(buf) < HDR_LEN:
        b = ser.read(HDR_LEN - len(buf))
        if b:
            buf.extend(b)
        elif time.time() - start > args.timeout:
            print('Timeout completando la cabecera tras resincronizar.')
            ser.close()
            sys.exit(3)

sample_rate = struct.unpack_from('<I', buf, 8)[0]
num_samples = struct.unpack_from('<I', buf, 12)[0]
bits_per_sample = struct.unpack_from('<H', buf, 16)[0]
channels = struct.unpack_from('<H', buf, 18)[0]
sample_bytes = bits_per_sample // 8

duration = num_samples / sample_rate if sample_rate else 0
print(f'Cabecera OK: {sample_rate} Hz, {num_samples} muestras (~{duration:.1f} s), '
      f'{bits_per_sample} bits, {channels} canal(es)')

# --- Leer el PCM ---
bytes_to_read = num_samples * sample_bytes
pcm = bytearray()
next_report = sample_rate * sample_bytes  # ~1 s
last_data = time.time()
while len(pcm) < bytes_to_read:
    chunk = ser.read(min(4096, bytes_to_read - len(pcm)))
    if chunk:
        pcm.extend(chunk)
        last_data = time.time()
        if len(pcm) >= next_report:
            sec = len(pcm) / (sample_rate * sample_bytes)
            print(f'  recibidos {sec:.1f}/{duration:.0f} s')
            next_report += sample_rate * sample_bytes
    elif time.time() - last_data > 5.0:
        faltan = bytes_to_read - len(pcm)
        pct = 100.0 * len(pcm) / bytes_to_read
        if pct >= 99.0:
            # Práctico: faltan unas pocas muestras del final (cola del UART).
            # Se rellena con silencio para dejar el WAV con la duración exacta.
            print(f'Captura completa ({pct:.2f}%); se rellena la cola de {faltan} bytes con silencio.')
            pcm.extend(b'\x00' * faltan)
        else:
            print(f'Stream interrumpido: solo se recibieron {len(pcm)}/{bytes_to_read} bytes '
                  f'({pct:.1f}%). Se guarda lo capturado.')
        break

ser.close()

os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
with wave.open(out_path, 'wb') as wf:
    wf.setnchannels(channels)
    wf.setsampwidth(sample_bytes)
    wf.setframerate(sample_rate)
    wf.writeframes(pcm)

print('Listo. WAV guardado en:', out_path)
print('Reproducir con:  afplay', out_path)
