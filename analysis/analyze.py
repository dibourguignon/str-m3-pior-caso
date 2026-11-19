#!/usr/bin/env python3
"""
analyze.py - Analises obrigatorias (secoes 6 e 7) da atividade
"Quanto o teste enxerga do pior caso?" - Sistemas em Tempo Real / UNIVALI

Entrada : data/etapa{1..4}_*.csv (ativacao,tempo_us), results/cobertura.csv,
          results/casos_teste.csv, data/etapa{3,4}_detalhe.csv
Saida   : figures/*.png, results/resumo.json, results/sintese.csv,
          results/perdas_etapa*.txt, results/expD.csv

Uso: python3 analysis/analyze.py [linux|esp32]   (a partir da raiz do projeto)
     linux (padrao): data/*.csv       -> figures/fig*.png,       results/*
     esp32         : data/esp32/*.csv -> figures/esp32_fig*.png, results/esp32_*
Deterministico: os mesmos CSV geram exatamente as mesmas tabelas e figuras.
"""
import csv
import json
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
os.makedirs("figures", exist_ok=True)
os.makedirs("results", exist_ok=True)

PLAT = sys.argv[1] if len(sys.argv) > 1 else "linux"
DDIR = "data" if PLAT == "linux" else f"data/{PLAT}"
FP = "figures/" if PLAT == "linux" else f"figures/{PLAT}_"      # prefixo das figuras
RP = "results/" if PLAT == "linux" else f"results/{PLAT}_"      # prefixo dos resultados

STAGES = [
    ("1. Tarefa",         f"{DDIR}/etapa1_tarefa.csv"),
    ("2. Comportamental", f"{DDIR}/etapa2_comportamental.csv"),
    ("3. Intertarefas",   f"{DDIR}/etapa3_intertarefas.csv"),
    ("4. Sistema",        f"{DDIR}/etapa4_sistema.csv"),
]
COLORS = ["#2a6fdb", "#1a9e77", "#e08a00", "#c8324b"]
KS = (10, 20, 50)
D_FACTOR = 1.2

plt.rcParams.update({
    "font.size": 8, "axes.titlesize": 9, "axes.labelsize": 8, "legend.fontsize": 7,
    "xtick.labelsize": 7, "ytick.labelsize": 7, "figure.dpi": 150, "savefig.dpi": 200,
    "axes.spines.top": False, "axes.spines.right": False, "axes.grid": True,
    "grid.alpha": 0.25, "font.family": "DejaVu Sans",
})


def load(path):
    with open(path, newline="") as f:
        r = csv.DictReader(f)
        return np.array([float(row["tempo_us"]) for row in r])


def hwm_pct(x, pct):
    """HWM(pct): maximo apos descartar (100-pct)% das maiores medicoes.

    Ex.: 10.000 medicoes e pct = 99 -> ordena, descarta as 100 maiores e
    devolve a maior que sobrou. Nao e percentil interpolado: e sempre um
    valor que foi de fato medido."""
    xs = np.sort(x)
    drop = int(round(len(xs) * (100 - pct) / 100.0))
    return float(xs[len(xs) - drop - 1])


def miss_analysis(x, D):
    """Analise das perdas de deadline (secoes 6.3 e 6.4 do enunciado).

    perda           : ativacao com R > D (R = D conta como cumprido)
    idx / dist      : lista das ativacoes perdidas e distancias entre perdas seguidas
    S (skip factor) : menor distancia entre duas perdas; S = 1 = perdas consecutivas
    runs            : tamanho de cada sequencia de perdas seguidas (1 = isolada,
                      >= 2 = rajada)
    janela          : para k = 10, 20 e 50, conta as perdas em cada posicao da
                      janela deslizante (todas as N-k+1 janelas completas,
                      avancando uma ativacao por vez); m = k - pior janela
    """
    miss = x > D
    idx = np.flatnonzero(miss) + 1                       # ativacoes (1-based)
    dist = np.diff(idx)
    S = int(dist.min()) if len(dist) else None
    # rajadas = sequencias de perdas consecutivas
    runs = []
    if len(idx):
        L = 1
        for d in dist:
            if d == 1:
                L += 1
            else:
                runs.append(L); L = 1
        runs.append(L)
    runs = np.array(runs, dtype=int)
    bursts = runs[runs >= 2]
    mk = {}
    win = {}
    for k in KS:
        # convolucao com k uns = soma movel: w[i] = perdas nas ativacoes i+1 .. i+k
        w = np.convolve(miss.astype(int), np.ones(k, dtype=int), mode="valid")
        win[k] = w
        mk[k] = (int(k - w.max()), k, int(w.max()))
    return {
        "miss": miss, "idx": idx, "dist": dist, "S": S, "runs": runs,
        "n_miss": int(miss.sum()), "pct_ok": 100.0 * (1 - miss.mean()),
        "n_isoladas": int((runs == 1).sum()), "n_rajadas": int(len(bursts)),
        "maior_rajada": int(runs.max()) if len(runs) else 0, "mk": mk, "win": win,
    }


def max_miss_window(miss, k):
    return int(np.convolve(miss.astype(int), np.ones(k, dtype=int), mode="valid").max())


def min_D_for(x, m, k):
    """Menor D (dentre os valores medidos) tal que a sequencia e (m,k)-firme.

    Usado no Experimento D. Aumentar D nunca cria perdas novas, entao o pior
    numero de perdas por janela so diminui com D. Isso permite busca binaria
    sobre os proprios tempos medidos (em ordem crescente): o menor candidato
    com no maximo k - m perdas na pior janela e a resposta."""
    cand = np.unique(x)
    lo, hi = 0, len(cand) - 1                           # com D = max, 0 perdas
    while lo < hi:                                      # perdas maximas e monotona em D
        mid = (lo + hi) // 2
        if max_miss_window(x > cand[mid], k) <= k - m:
            hi = mid
        else:
            lo = mid + 1
    return float(cand[lo])


# ------------------------------------------------------------------ dados
data = [load(p) for _, p in STAGES]
st = []
for (name, _), x in zip(STAGES, data):
    st.append({
        "etapa": name, "n": int(len(x)), "min": float(x.min()), "media": float(x.mean()),
        "mediana": float(np.median(x)), "desvio": float(x.std(ddof=1)),
        "hwm100": float(x.max()), "hwm99": hwm_pct(x, 99), "p999": float(np.percentile(x, 99.9)),
        "arg_hwm": int(x.argmax()) + 1,
    })

D = round(st[0]["hwm100"] * D_FACTOR, 1)               # D fixo, definido UMA vez na etapa 1
with open(f"{RP}D.txt", "w") as f:
    f.write(f"D = HWM(100%) etapa 1 x {D_FACTOR} = {st[0]['hwm100']:.3f} x {D_FACTOR} = {D:.1f} us\n")

mas = [miss_analysis(x, D) for x in data]

# cobertura (experimento A)
cov_rows = list(csv.DictReader(open("results/cobertura.csv")))
cov_final = cov_rows[-1]
cov_txt = f'{float(cov_final["linha_pct"]):.0f}% / {float(cov_final["desvio_pct"]):.0f}%'
casos = list(csv.DictReader(open("results/casos_teste.csv")))

# ------------------------------------------------------------------ tabela-sintese
sint = []
for i, (s, m) in enumerate(zip(st, mas)):
    row = {
        "Etapa": s["etapa"], "Cobertura (linha/desvio)": cov_txt,
        "Min (us)": f'{s["min"]:.0f}', "Media (us)": f'{s["media"]:.0f}',
        "HWM(100%) (us)": f'{s["hwm100"]:.0f}', "HWM(99%) (us)": f'{s["hwm99"]:.0f}',
        "% no prazo": f'{m["pct_ok"]:.2f}', "S": "-" if m["S"] is None else str(m["S"]),
    }
    for k in KS:
        mm, kk, _ = m["mk"][k]
        row[f"(m,k) k={k}"] = f"({mm},{kk})"
    sint.append(row)
with open(f"{RP}sintese.csv", "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(sint[0].keys()))
    w.writeheader(); w.writerows(sint)

# listas de perdas e distancias (6.3)
for i, m in enumerate(mas):
    with open(f"{RP}perdas_etapa{i+1}.txt", "w") as f:
        f.write(f"D = {D:.1f} us\nperdas = {m['n_miss']}\n")
        f.write("ativacoes com perda: " + ", ".join(map(str, m["idx"])) + "\n")
        f.write("distancias: " + ", ".join(map(str, m["dist"])) + "\n")
        f.write(f"S = {m['S']}\nrajadas (>=2) = {m['n_rajadas']}, maior = {m['maior_rajada']}, isoladas = {m['n_isoladas']}\n")
        f.write("comprimentos das sequencias: " + ", ".join(map(str, m["runs"])) + "\n")

# ------------------------------------------------------------------ Fig 1: histogramas
# legenda unica embaixo e valores no titulo de cada grafico: nada fica por cima das barras
fig, axs = plt.subplots(2, 2, figsize=(7.2, 5.0))
for i, ax in enumerate(axs.flat):
    x, s = data[i], st[i]
    lo = min(s["min"], D) * 0.97
    hi = max(s["hwm100"], D) * 1.03
    bins = np.linspace(s["min"] * 0.98, s["hwm100"] * 1.01, 80)
    ax.hist(x, bins=bins, color=COLORS[i], alpha=0.85, edgecolor="none")
    ax.set_yscale("log")
    ax.set_xlim(lo, hi)
    ax.set_ylim(0.7, len(x) * 2)
    ax.axvline(s["media"], color="#555", ls=":", lw=1.1, label="média")
    ax.axvline(s["hwm99"], color="#7a3cc4", ls="--", lw=1.1, label="HWM(99%)")
    ax.axvline(s["hwm100"], color="black", ls="-", lw=1.2, label="HWM(100%)")
    ax.axvline(D, color="#d62728", ls="-.", lw=1.4, label="D")
    ax.set_title(f'{s["etapa"]} (n = {s["n"]})\n'
                 f'média {s["media"]:.0f} · HWM(99%) {s["hwm99"]:.0f} · HWM {s["hwm100"]:.0f} µs',
                 fontsize=8)
    ax.set_xlabel("tempo de resposta (µs)")
    if i % 2 == 0:
        ax.set_ylabel("ativações (log)")
h, l = axs.flat[0].get_legend_handles_labels()
fig.legend(h, l + [], loc="lower center", ncol=4, frameon=False, bbox_to_anchor=(0.5, 0.0))
fig.tight_layout(rect=(0, 0.05, 1, 1))
fig.savefig(f"{FP}fig1_histogramas.png"); plt.close(fig)

# ------------------------------------------------------------------ Fig 2: temporal
# legenda unica embaixo; folga vertical para os picos nao encostarem no titulo
fig, axs = plt.subplots(4, 1, figsize=(7.2, 5.0), sharex=False)
for i, ax in enumerate(axs):
    x, s = data[i], st[i]
    a = np.arange(1, len(x) + 1)
    ax.plot(a, x, lw=0.35, color=COLORS[i], label="tempo de resposta")
    sp = x > s["hwm99"]
    ax.scatter(a[sp], x[sp], s=5, color="black", zorder=3, label="picos (acima do HWM(99%))")
    ax.axhline(D, color="#d62728", ls="-.", lw=1, label=f"D = {D:.0f} µs")
    lo, hi = min(x.min(), D), max(x.max(), D)
    pad = (hi - lo) * 0.15
    ax.set_ylim(lo - pad, hi + pad)
    ax.set_ylabel("µs")
    ax.set_title(f'{s["etapa"]} (HWM {s["hwm100"]:.0f} µs)', loc="left", fontsize=8.5)
    ax.set_xlim(0, len(x) + 1)
    if i == 3:
        ax.set_xlabel("número da ativação")
h, l = axs[3].get_legend_handles_labels()
fig.legend(h, l, loc="lower center", ncol=3, frameon=False, bbox_to_anchor=(0.5, 0.0))
fig.tight_layout(rect=(0, 0.045, 1, 1))
fig.savefig(f"{FP}fig2_temporal.png"); plt.close(fig)

# ------------------------------------------------------------------ Fig 3: convergencia (Exp. B)
marks = [10, 20, 100, 1000, 3000, 10000]
conv = {}
fig, ax = plt.subplots(figsize=(6.4, 2.7))
for i, x in enumerate(data):
    cm = np.maximum.accumulate(x)
    n = np.arange(1, len(x) + 1)
    ax.step(n, cm, where="post", color=COLORS[i], lw=1.3, label=st[i]["etapa"])
    jumps = np.flatnonzero(np.diff(cm) > 0) + 2          # ativacoes onde o HWM subiu
    conv[i] = {
        "marcos": {str(m_): float(cm[m_ - 1]) for m_ in marks if m_ <= len(x)},
        "n_saltos": int(len(jumps) + 1), "ultimo_salto": int(jumps[-1]) if len(jumps) else 1,
        "n_95": int(np.argmax(cm >= 0.95 * cm[-1]) + 1),
    }
    ax.scatter([m_ for m_ in marks if m_ <= len(x)], [cm[m_ - 1] for m_ in marks if m_ <= len(x)],
               color=COLORS[i], s=12, zorder=3)
ax.axhline(D, color="#d62728", ls="-.", lw=1, label=f"D = {D:.0f} µs")
ax.set_xscale("log"); ax.set_yscale("log")
ax.set_xlabel("número de amostras (log)"); ax.set_ylabel("HWM acumulado (µs, log)")
ax.legend(frameon=False, ncol=5, loc="lower center", bbox_to_anchor=(0.5, 1.0), fontsize=6.8,
          handlelength=1.6, columnspacing=1.0)
fig.tight_layout(); fig.savefig(f"{FP}fig3_convergencia.png"); plt.close(fig)

# ------------------------------------------------------------------ Fig 4: janela deslizante (6.4)
kc = {10: "#2a6fdb", 20: "#e08a00", 50: "#c8324b"}


def window_plot(ax, m, n, title):
    for k in KS:
        w = m["win"][k]
        ax.plot(np.arange(1, len(w) + 1), w, lw=1.0, color=kc[k], label=f"k = {k}")
    ax.set_title(title, loc="left")
    ax.set_xlim(0, n)
    ax.set_ylim(-3, 68)
    ax.set_yticks([0, 10, 20, 30, 40, 50])
    mk_txt = "   ".join(f'({m["mk"][k][0]},{k})' for k in KS)
    ax.text(0.98, 0.95, mk_txt, transform=ax.transAxes, ha="right", va="top", fontsize=7.5,
            color="#222", bbox=dict(boxstyle="round,pad=0.25", fc="white", ec="#bbb", lw=0.6))


# grade 2 x 2 com a mesma escala vertical; legenda comum embaixo (nada fica fora da figura)
fig, axs = plt.subplots(2, 2, figsize=(6.4, 4.0), sharey=True)
for i, ax in enumerate(axs.flat):
    window_plot(ax, mas[i], len(data[i]), st[i]["etapa"])
    if i % 2 == 0:
        ax.set_ylabel("perdas na janela")
    if i >= 2:
        ax.set_xlabel("início da janela (ativação)")
h, l = axs.flat[0].get_legend_handles_labels()
fig.legend(h, l, loc="lower center", ncol=3, frameon=False, bbox_to_anchor=(0.5, 0.0))
fig.tight_layout(rect=(0, 0.05, 1, 1))
fig.savefig(f"{FP}fig4_janela.png"); plt.close(fig)

# ------------------------------------------------------------------ Exp. C: margens
h1 = st[0]["hwm100"]
margens = {p: h1 * (1 + p / 100) for p in (10, 25, 50)}
expC = []
for s, x in zip(st, data):
    expC.append({
        "etapa": s["etapa"], "hwm": s["hwm100"],
        "margem_necessaria_pct": 100 * (s["hwm100"] / h1 - 1),
        "cv_pct": 100 * s["desvio"] / s["media"],
        "faixa_p1_p99": [float(np.percentile(x, 1)), float(np.percentile(x, 99))],
        **{f"ok_{p}": bool(v >= s["hwm100"]) for p, v in margens.items()},
        **{f"pct_cobre_{p}": float(100 * (x <= v).mean()) for p, v in margens.items()},
    })
fig, ax = plt.subplots(figsize=(7.2, 2.5))
xpos = np.arange(4)
ax.bar(xpos, [s["hwm100"] for s in st], color=COLORS, width=0.55, label="HWM(100%) da etapa")
ax.bar(xpos, [s["hwm99"] for s in st], color="white", alpha=0.45, width=0.55, hatch="///",
       edgecolor="#333", lw=0.4, label="HWM(99%)")
for p, v in margens.items():
    ax.axhline(v, ls="--", lw=1, color={10: "#888", 25: "#555", 50: "#111"}[p],
               label=f"HWM etapa 1 + {p}% = {v:.0f} µs")
for j, s in enumerate(st):
    ax.text(j, s["hwm100"], f'{s["hwm100"]:.0f}', ha="center", va="bottom", fontsize=7)
ax.set_xticks(xpos, [s["etapa"] for s in st])
ax.set_ylabel("µs"); ax.set_yscale("log")
ax.legend(frameon=False, fontsize=6.5, ncol=2, loc="upper left")
fig.tight_layout(); fig.savefig(f"{FP}fig5_margens.png"); plt.close(fig)

# ------------------------------------------------------------------ Exp. D
x3, x4 = data[2], data[3]
D_list = {
    "D1 = HWM(100%) etapa 1": st[0]["hwm100"],
    "D fixo = 1,2 x HWM etapa 1": D,
    "D2 = intermediário": None,
    "D3 = HWM(99%) etapa 4": st[3]["hwm99"],
}
D_list["D2 = intermediário"] = round((D + st[3]["hwm99"]) / 2, 1)
expD = []
# Repete a analise 6.3 completa (lista de perdas, distancias, S, rajadas) para cada D:
# um arquivo por etapa, com uma secao para cada valor de D.
expD_txt = {"3. Intertarefas": [], "4. Sistema": []}
for lbl, dv in sorted(D_list.items(), key=lambda kv: kv[1]):
    for nm, x in (("3. Intertarefas", x3), ("4. Sistema", x4)):
        m = miss_analysis(x, dv)
        expD_txt[nm].append(
            f"=== {lbl}: D = {dv:.1f} us ===\n"
            f"perdas = {m['n_miss']} de {len(x)} ({100 - m['pct_ok']:.2f}%)\n"
            "ativacoes com perda: " + ", ".join(map(str, m["idx"])) + "\n"
            "distancias: " + ", ".join(map(str, m["dist"])) + "\n"
            f"S = {m['S']}\nrajadas (>=2) = {m['n_rajadas']}, maior = {m['maior_rajada']}, "
            f"isoladas = {m['n_isoladas']}\n"
            + "".join(f"(m,{k}) = ({m['mk'][k][0]},{k})  " for k in KS) + "\n")
        expD.append({
            "D_rotulo": lbl, "D_us": round(dv, 1), "etapa": nm, "pct_ok": round(m["pct_ok"], 2),
            "perdas": m["n_miss"], "S": m["S"], "mk20": f'({m["mk"][20][0]},20)',
            "isoladas": m["n_isoladas"], "rajadas": m["n_rajadas"], "maior_rajada": m["maior_rajada"],
            "mk10": f'({m["mk"][10][0]},10)', "mk50": f'({m["mk"][50][0]},50)',
        })
with open(f"{RP}expD.csv", "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(expD[0].keys()))
    w.writeheader(); w.writerows(expD)
for nm, partes in expD_txt.items():
    with open(f"{RP}expD_perdas_etapa{nm[0]}.txt", "w") as f:
        f.write(f"Experimento D - {nm}: analise 6.3 e 6.4 para cada valor de D\n\n" + "\n".join(partes))

reqs = [(19, 20), (18, 20), (15, 20), (10, 20)]
minD = {f"({m_},{k_})": {"etapa4": min_D_for(x4, m_, k_), "etapa3": min_D_for(x3, m_, k_)}
        for m_, k_ in reqs}

# curva: % no prazo e m(k=20) em funcao de D
Ds = np.linspace(st[0]["min"], st[3]["hwm100"], 400)
fig, (a1, a2) = plt.subplots(1, 2, figsize=(7.2, 2.6))
for nm, x, c in (("3. Intertarefas", x3, COLORS[2]), ("4. Sistema", x4, COLORS[3])):
    a1.plot(Ds, [100 * (x <= d).mean() for d in Ds], color=c, label=nm)
    a2.plot(Ds, [20 - max_miss_window(x > d, 20) for d in Ds], color=c, label=nm)
for a in (a1, a2):
    a.axvline(D, color="#d62728", ls="-.", lw=1, label=f"D fixo {D:.0f}")
    a.set_xscale("log"); a.set_xlabel("deadline D (µs, log)")
for r_, ls in (("(18,20)", ":"), ("(19,20)", "--")):
    a2.axvline(minD[r_]["etapa4"], color="#333", ls=ls, lw=0.9, label=f"menor D p/ {r_} na etapa 4")
a1.set_ylabel("% de deadlines cumpridos"); a1.set_title("Percentual cumprido × D", loc="left")
a2.set_ylabel("m (k = 20)"); a2.yaxis.set_major_locator(matplotlib.ticker.MaxNLocator(integer=True)); a2.set_title("(m,20)-firme exibido × D", loc="left")
a1.legend(frameon=False); a2.legend(frameon=False, fontsize=6)
fig.tight_layout(); fig.savefig(f"{FP}fig6_expD.png"); plt.close(fig)

# janela deslizante k=20 na etapa 4 para cada D do experimento D
fig, ax = plt.subplots(figsize=(7.2, 2.3))
for (lbl, dv), c in zip(sorted(D_list.items(), key=lambda kv: kv[1]), ["#7a3cc4", "#c8324b", "#e08a00", "#1a9e77"]):
    w = np.convolve((x4 > dv).astype(int), np.ones(20, dtype=int), mode="valid")
    ax.plot(np.arange(1, len(w) + 1), w, lw=0.6, color=c, label=f"{lbl} = {dv:.0f} µs → ({20 - w.max()},20)")
ax.set_xlim(0, len(x4)); ax.set_ylabel("perdas (k = 20)")
ax.set_xlabel("posição inicial da janela (ativação)")
ax.set_title("Etapa 4 - perdas por janela de 20 ativações para cada D", loc="left")
ax.legend(frameon=False, ncol=2, fontsize=6.5, loc="upper left"); ax.set_ylim(0, 30)
fig.tight_layout(); fig.savefig(f"{FP}fig7_expD_janela.png"); plt.close(fig)

# ------------------------------------------------------------------ causas (detalhe etapas 3/4)
causas = {}
for i, p in ((3, f"{DDIR}/etapa3_detalhe.csv"), (4, f"{DDIR}/etapa4_detalhe.csv"),
             (3, f"{DDIR}/etapa3_intertarefas_detalhe.csv"), (4, f"{DDIR}/etapa4_sistema_detalhe.csv")):
    if not os.path.exists(p):
        continue
    rows = np.array([[float(v) for v in r.values()] for r in csv.DictReader(open(p))])
    t, lat, wt = rows[:, 1], rows[:, 2], rows[:, 3]
    mi = t > D
    causas[i] = {
        "lat_media": float(lat.mean()), "lat_max": float(lat.max()),
        "espera_media": float(wt.mean()), "espera_max": float(wt.max()),
        "pct_bloqueadas_100us": float(100 * (wt > 100).mean()),
        "perdas_com_bloqueio_pct": float(100 * (wt[mi] > 100).mean()) if mi.any() else None,
        "perdas_com_lat_500_pct": float(100 * (lat[mi] > 500).mean()) if mi.any() else None,
        "exec_sem_bloqueio_media": float((t - lat - wt).mean()),
        "exec_sem_bloqueio_max": float((t - lat - wt).max()),
    }

# ------------------------------------------------------------------ resumo
resumo = {
    "D": D, "D_fator": D_FACTOR, "stats": st, "cobertura": cov_rows, "casos_teste": casos,
    "perdas": [{k: (v if not isinstance(v, np.ndarray) else None) for k, v in m.items()
                if k not in ("miss", "win", "idx", "dist", "runs")} | {
                   "primeiras_perdas": m["idx"][:25].tolist(), "primeiras_dist": m["dist"][:25].tolist(),
                   "hist_runs": {int(L): int((m["runs"] == L).sum()) for L in np.unique(m["runs"])}}
               for m in mas],
    "convergencia": conv, "margens": margens, "expC": expC, "expD": expD, "minD": minD,
    "causas": causas,
}
with open(f"{RP}resumo.json", "w") as f:
    json.dump(resumo, f, indent=1, ensure_ascii=False, default=str)

# saida de console
print(f"D = {D:.1f} us")
for r in sint:
    print(" | ".join(r.values()))
print("convergencia:", json.dumps(conv, indent=0))
print("minD:", minD)
for e in expD:
    print(e)
print("expC:", json.dumps(expC, indent=0))
print("causas:", json.dumps(causas, indent=0))
