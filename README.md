# Quanto o teste enxerga do pior caso? — STR / UNIVALI (M3)

Medição do tempo de resposta da **codificação de Huffman** (C, `clock_gettime(CLOCK_MONOTONIC)`)
em quatro escopos de teste — tarefa, comportamental, intertarefas e sistema — com análise de
HWM, HWM(99%), skip factor e (m,k)-firme por janela deslizante.

* **Aluno:** Diego B. Rangel (RA 22.2.1252) · **Repositório:** https://github.com/dibourguignon/str-m3-pior-caso
* **Plataforma principal:** ESP32 (FreeRTOS / ESP-IDF 6.0.1), firmware em `esp32/`.
* **Plataforma de comparação:** Linux 6.18 (WSL2, Ryzen 5 5500X3D), programas em `src/`.
* A tarefa sob teste (`src/huffman.c`) é **o mesmo arquivo** nas duas plataformas.

## Estrutura

| Caminho | Conteúdo |
|---|---|
| `src/huffman.c/.h` | Tarefa sob teste: codifica, decodifica e valida (sem `malloc`, 100% dos desvios alcançáveis) |
| `esp32/src/main.c` | Firmware ESP32: casos de teste (Exp. A) + etapas 1–4 |
| `esp32/capture.py` | Reinicia a placa, **gera o estresse UART da etapa 4** e grava os CSV |
| `esp32/platformio.ini` | Projeto PlatformIO (abrir a pasta `esp32/`); ambientes `demo` (padrão) e `campanha` |
| `esp32/build.ps1` | Alternativa pelo terminal quando o caminho tem espaços (copia para `C:\str_m3_esp32`) |
| `src/etapa*.c`, `src/intertarefas.c`, `src/common.h` | Etapas 1–4 na plataforma Linux (comparação) |
| `tests/test_huffman.c` | Casos de teste do Experimento A (`-DNMAX=1024` = tamanhos do ESP32) |
| `scripts/coverage.sh` | Cobertura de linha/desvio incremental com gcov/gcovr (N = 32768 e N = 1024) |
| `scripts/run_all.sh` | Campanha completa na plataforma Linux |
| `analysis/analyze.py` | Todas as análises e figuras (seções 6 e 7 do enunciado) |
| `analysis/compare.py` | Comparação entre plataformas (curvas de excedência) |
| `data/esp32/*.csv`, `data/*.csv` | Dados brutos (`ativacao,tempo_us`) + detalhe das etapas 3/4 |
| `results/`, `figures/` | Tabelas, perdas/distâncias, Exp. D, cobertura, plataforma; figuras (`esp32_*` = ESP32) |
| `report/relatorio.pdf` | Relatório |

## Parâmetros

* ESP32: **N = 1024**, semente **20261118**, tarefas no núcleo 1, períodos sensor/huffman/logger = 4/10/33 ms,
  D = 1,2 × HWM(100%) da etapa 1 = 1,2 × 2265 = **2718 µs** (fixo nas 4 etapas).
* Etapa 4 (ESP32): etapa 3 + eventos GPIO (2–8 ms) + **rajada UART vinda do PC** — quadros de 262 B
  (`A5 5A | seq | 256 B | soma`) gerados por `random.Random(20261118)`, enviados continuamente a 115200 baud;
  o driver UART roda no núcleo 1 com uma interrupção por byte e cada quadro é tratado por uma tarefa de prioridade 11.
* Linux: N = 32768, D = 6549 µs, carga externa `stress-ng`.

## Como reproduzir

**Somente as tabelas e figuras do relatório** (a partir dos CSV incluídos — determinístico):

```bash
python3 analysis/analyze.py esp32     # plataforma principal  -> figures/esp32_*.png, results/esp32_*
python3 analysis/analyze.py linux     # comparação            -> figures/fig*.png,     results/*
python3 analysis/compare.py           # figura de excedência Linux x ESP32
bash scripts/coverage.sh              # Experimento A (gcov/gcovr)
```

**ESP32 pelo VS Code + PlatformIO** (qualquer ESP32 DevKit; a mesma placa + o mesmo script aplicam o mesmo estímulo):

1. Clone o repositório em uma pasta **sem espaços** (limitação do ESP-IDF), por exemplo
   `git clone https://github.com/dibourguignon/str-m3-pior-caso C:\str-m3-pior-caso`.
2. No VS Code, **File → Open Folder → `C:\str-m3-pior-caso\esp32`** (a pasta `esp32`, não a raiz).
   O `platformio.ini` fixa `platformio/espressif32@7.0.1` (ESP-IDF 6.0.1); na primeira vez o
   PlatformIO baixa a plataforma e o ESP-IDF (~1,5 GB).
3. Botões do PlatformIO: **Build** e **Upload**. O ambiente padrão é `demo` (1.000 ativações por
   etapa, ~45 s); o ambiente `campanha` (10.000 / 3.000 / 10.000 / 10.000, ~6 min) gerou os dados do relatório.
4. Feche o Serial Monitor e, no terminal do PlatformIO (pasta `esp32`), rode a captura, que gera o
   estresse UART da etapa 4 e mostra o resumo:
   ```powershell
   & "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" capture.py COM3 defesa
   ```

Pelo terminal, sem os botões: `pio run -e demo -t upload` (ou `-e campanha`) dentro de `esp32/`.
Se o repositório estiver em uma pasta com espaços, `powershell -File esp32\build.ps1 -Upload [-Demo]`
copia o projeto para `C:\str_m3_esp32` e compila lá. Depois de uma coleta:
`python3 analysis/analyze.py defesa` gera as figuras e tabelas de `data/defesa/`.

O firmware imprime, depois de cada etapa, uma linha `### RESUMO` (mín., média, HWM, HWM(99%), D,
% no prazo, perdas, S, maior rajada e (m,20)), e o `capture.py` mostra no fim uma tabela com as quatro
etapas. O segundo argumento do `capture.py` é o nome da coleta: os dados vão para `data/<nome>/` e
`results/<nome>_*`. Os dados do relatório ficam em `data/esp32/` e só são sobrescritos com o nome `esp32`.
Sem o `capture.py` (por exemplo, com `pio device monitor`), o firmware roda igual, mas a etapa 4 fica sem o
estresse UART, que é gerado pelo script.

**Nova campanha no Linux:** `sudo bash scripts/run_all.sh` (requer `build-essential gcovr stress-ng python3-numpy python3-matplotlib`).

Uma nova campanha gera números diferentes (o tempo depende do hardware e da carga); por isso a
plataforma e as condições ficam registradas em `results/*plataforma*.txt`.
