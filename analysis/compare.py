#!/usr/bin/env python3
"""
compare.py - Comparacao Linux (WSL2, proposito geral) x ESP32 (FreeRTOS)

Gera figures/fig8_comparacao.png (curvas de excedencia / CCDF normalizadas pela
mediana da etapa 1 de cada plataforma) e results/comparacao.json.
Requer que analysis/analyze.py tenha sido executado para as duas plataformas.
Uso: python3 analysis/compare.py
"""
import csv
import json
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
NAMES = ["etapa1_tarefa", "etapa2_comportamental", "etapa3_intertarefas", "etapa4_sistema"]
LABELS = ["1. Tarefa", "2. Comportamental", "3. Intertarefas", "4. Sistema"]
COLORS = ["#2a6fdb", "#1a9e77", "#e08a00", "#c8324b"]
PLATS = [("linux", "data", "Linux 6.18 / WSL2 (Ryzen 5 5500X3D)"),
         ("esp32", "data/esp32", "FreeRTOS / ESP32 (Xtensa LX6, 240 MHz)")]

plt.rcParams.update({
    "font.size": 8, "axes.titlesize": 9, "axes.labelsize": 8, "legend.fontsize": 7,
    "xtick.labelsize": 7, "ytick.labelsize": 7, "savefig.dpi": 200,
    "axes.spines.top": False, "axes.spines.right": False, "axes.grid": True, "grid.alpha": 0.25,
})


def load(p):
    with open(p, newline="") as f:
        return np.array([float(r["tempo_us"]) for r in csv.DictReader(f)])


out = {}
fig, axs = plt.subplots(1, 2, figsize=(7.2, 2.9), sharey=True)
for ax, (pid, ddir, title) in zip(axs, PLATS):
    data = [load(f"{ddir}/{n}.csv") for n in NAMES]
    ref = float(np.median(data[0]))
    out[pid] = {"mediana_etapa1_us": ref, "etapas": []}
    for x, lab, c in zip(data, LABELS, COLORS):
        xs = np.sort(x) / ref
        ccdf = 1.0 - np.arange(len(xs)) / len(xs)
        ax.step(xs, ccdf, where="post", color=c, lw=1.2, label=lab)
        out[pid]["etapas"].append({
            "etapa": lab, "n": int(len(x)), "mediana_us": float(np.median(x)),
            "hwm_sobre_mediana1": float(x.max() / ref), "p99_sobre_mediana1": float(np.percentile(x, 99) / ref),
            "cv_pct": float(100 * x.std(ddof=1) / x.mean()),
            "amplitude_rel_pct": float(100 * (x.max() - x.min()) / x.min()),
        })
    ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_title(title, loc="left")
    ax.set_xlabel("tempo de resposta / mediana da etapa 1 (log)")
    ax.set_ylim(1e-4 * 0.8, 1.3)
    ax.axhline(1e-3, color="#999", lw=0.6, ls=":")
    ax.axhline(1e-2, color="#999", lw=0.6, ls=":")
axs[0].set_ylabel("P(R > r)  (excedência, log)")
axs[0].legend(frameon=False, loc="lower left")
fig.tight_layout(); fig.savefig("figures/fig8_comparacao.png"); plt.close(fig)

with open("results/comparacao.json", "w") as f:
    json.dump(out, f, indent=1, ensure_ascii=False)
for pid in out:
    print(pid, f'mediana etapa1 = {out[pid]["mediana_etapa1_us"]:.0f} us')
    for e in out[pid]["etapas"]:
        print(f'  {e["etapa"]:<18} n={e["n"]:<6} med={e["mediana_us"]:8.0f}  HWM/med1={e["hwm_sobre_mediana1"]:7.2f}'
              f'  p99/med1={e["p99_sobre_mediana1"]:5.2f}  CV={e["cv_pct"]:5.1f}%  (max-min)/min={e["amplitude_rel_pct"]:7.1f}%')
