#!/usr/bin/env python3
"""
capture.py - Executa e captura a campanha de medicao do ESP32 pela serial.

1. Reinicia a placa (linha RTS -> EN).
2. Le os blocos "### INICIO <nome> ... ### FIM <nome>" e grava:
     data/esp32/<nome>.csv            (ativacao,tempo_us)
     data/esp32/<nome>_detalhe.csv    (etapas 3 e 4: + latencia de inicio e espera no mutex)
     results/esp32_casos_teste.csv    (Experimento A: HWM de cada caso de teste)
     results/esp32_plataforma.txt     (plataforma, calibracao, contadores)
3. ESTRESSE DA ETAPA 4: entre "### UART_ESTRESSE ON" e "### UART_ESTRESSE OFF" o PC envia,
   na taxa maxima da linha (115200 baud, 8N1), quadros deterministicos de 262 bytes:
       A5 5A | seq (uint16 LE) | payload 256 B | soma de 16 bits do payload (LE)
   O payload vem de random.Random(20261118): qualquer grupo que rode este script com a
   mesma placa e o mesmo firmware aplica exatamente o mesmo estimulo.

Uso: python esp32/capture.py COM3 [nome]   (requer pyserial; o Python do PlatformIO ja o possui)
     nome (padrao "esp32_nova"): grava em data/<nome>/ e results/<nome>_*.
     Os dados do relatorio estao em data/esp32/ e so sao sobrescritos com nome = esp32.
     Depois: python3 analysis/analyze.py <nome>   -> figures/<nome>_*.png, results/<nome>_*
"""
import os
import random
import sys
import threading
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
NAME = sys.argv[2] if len(sys.argv) > 2 else "esp32_nova"
SEED = 20261118
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "data", NAME)
RES = os.path.join(ROOT, "results")
os.makedirs(OUT, exist_ok=True)
os.makedirs(RES, exist_ok=True)
resumos = []

ser = serial.Serial(PORT, 115200, timeout=1, write_timeout=2)
ser.dtr = False
ser.rts = True           # EN em nivel baixo -> reset
time.sleep(0.2)
ser.rts = False
t_start = time.time()


def log(msg):
    print(f"[{time.time() - t_start:6.0f}s] {msg}", flush=True)


# ---------------- gerador do estresse UART ----------------
stress_on = threading.Event()
stress_stats = {"quadros": 0, "bytes": 0}


def make_frames(n_frames):
    rnd = random.Random(SEED)
    for seq in range(n_frames):
        payload = bytes(rnd.randrange(32, 127) for _ in range(256))
        s = sum(payload) & 0xFFFF
        yield b"\xA5\x5A" + (seq & 0xFFFF).to_bytes(2, "little") + payload + s.to_bytes(2, "little")


def stress_sender():
    frames = make_frames(10 ** 9)
    while True:
        stress_on.wait()
        while stress_on.is_set():
            # mantem a fila de saida curta: a taxa efetiva e a da propria linha serial
            if ser.out_waiting > 600:
                time.sleep(0.002)
                continue
            f = next(frames)
            ser.write(f)
            stress_stats["quadros"] += 1
            stress_stats["bytes"] += len(f)


threading.Thread(target=stress_sender, daemon=True).start()

# ---------------- leitura dos blocos ----------------
info = []
block, rows, header = None, [], None
deadline = t_start + 30 * 60
while time.time() < deadline:
    raw = ser.readline()
    if not raw:
        continue
    line = raw.decode("utf-8", "replace").strip()
    if "### UART_ESTRESSE ON" in line:
        stress_on.set(); t_on = time.time()
        log("estresse UART: PC comecou a enviar quadros"); info.append(line)
        continue
    if "### UART_ESTRESSE OFF" in line:
        stress_on.clear()
        dur = time.time() - t_on
        msg = (f"estresse UART: PC enviou {stress_stats['quadros']} quadros, {stress_stats['bytes']} bytes "
               f"em {dur:.1f} s ({stress_stats['bytes'] / dur:.0f} B/s)")
        log(msg); info.append(line); info.append(msg)
        continue
    if line.startswith("### INICIO"):
        block, rows, header = line.split()[2], [], None
        info.append(line)
        log(line)
        continue
    if line.startswith("### FIM") and block and block != "PLATAFORMA":
        if header and header.startswith("ativacao"):
            n_cols = len(header.split(","))
            good = [r for r in rows if r.count(",") == n_cols - 1]
            with open(os.path.join(OUT, f"{block}.csv"), "w", newline="") as f:
                f.write("ativacao,tempo_us\n")
                for r in good:
                    c = r.split(",")
                    f.write(f"{c[0]},{c[1]}\n")
            if n_cols > 2:
                with open(os.path.join(OUT, f"{block}_detalhe.csv"), "w", newline="") as f:
                    f.write(header + "\n" + "\n".join(good) + "\n")
        else:                                   # bloco generico (ex.: casos de teste)
            good = rows
            with open(os.path.join(RES, f"{NAME}_{block}.csv"), "w", newline="") as f:
                f.write(header + "\n" + "\n".join(rows) + "\n")
        log(f"{block}: {len(good)} linhas gravadas")
        block = None
        continue
    if block and block != "PLATAFORMA":
        if header is None and (line.startswith("ativacao") or line.startswith("caso,")):
            header = line
        elif line[:1].isdigit():
            rows.append(line)
        continue
    if line.startswith("### RESUMO"):
        resumos.append(line[len("### RESUMO "):])
    if line and not line.startswith(("I (", "D (", "V (")):
        info.append(line)
        log(line)
    if "CAMPANHA CONCLUIDA" in line:
        break

with open(os.path.join(RES, f"{NAME}_plataforma.txt"), "w", encoding="utf-8") as f:
    f.write("\n".join(info) + "\n")
ser.close()

# ---------------- resumo final (calculado no proprio ESP32) ----------------
if resumos:
    print("\n" + "=" * 100)
    print("RESUMO DA CAMPANHA (calculado no ESP32; tempos em us)")
    print("=" * 100)
    cols = ["n", "min", "media", "HWM", "HWM99", "D", "no_prazo", "perdas", "S", "maior_rajada", "(m,20)"]
    print(f"{'etapa':<23}" + "".join(f"{c:>13}" for c in cols))
    for r in resumos:
        nome, resto = r.split(":", 1)
        kv = dict(p.split("=", 1) for p in resto.split() if "=" in p)
        print(f"{nome:<23}" + "".join(f"{kv.get(c, '-'):>13}" for c in cols))
    print("=" * 100)
print(f"captura encerrada: dados em data/{NAME}/ e results/{NAME}_*", flush=True)
