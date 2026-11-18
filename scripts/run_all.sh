#!/usr/bin/env bash
# Campanha completa de medicao (rodar como root para SCHED_FIFO e mlockall):
#   sudo bash scripts/run_all.sh
# Gera data/*.csv, results/plataforma.txt e results/cobertura.csv
set -e
cd "$(dirname "$0")/.."
mkdir -p data results
make -s all

SEED=20261118
N=32768
STRESS="--cpu 12 --cache 2 --vm 2 --vm-bytes 20% --timer 2 --sock 1"

# ---------- registro da plataforma ----------
{
  echo "data: $(date -Iseconds)"
  echo "kernel: $(uname -srvm)"
  grep PRETTY_NAME /etc/os-release
  lscpu | grep -E "Model name|^CPU\(s\)|Thread|Core|Socket|L1d|L2|L3"
  echo "SMT: CPU2 (nucleo RT) e CPU3 sao irmaos SMT do mesmo nucleo fisico"
  echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'nao exposto (VM WSL2; controlado pelo plano de energia do Windows)')"
  echo "sched_rt_runtime_us/period_us: $(cat /proc/sys/kernel/sched_rt_runtime_us)/$(cat /proc/sys/kernel/sched_rt_period_us)"
  echo "preempcao: $(cat /sys/kernel/debug/sched/preempt 2>/dev/null || uname -v)"
  free -h | head -2
  echo "gcc: $(gcc --version | head -1)"
  echo "stress-ng: $(stress-ng --version)"
  echo "N=$N semente=$SEED"
  echo "--- processos em execucao no Linux (WSL) antes do teste:"
  ps -eo pid,comm,%cpu --sort=-%cpu | head -15
} > results/plataforma.txt

# ---------- Etapa 1: tarefa isolada ----------
./bin/etapa1 -a 10000 -n $N -s $SEED -o data/etapa1_tarefa.csv
# ---------- Etapa 2: comportamental (evento UDP externo, um por vez) ----------
./bin/etapa2 -a 3000 -n $N -s $SEED -o data/etapa2_comportamental.csv
# ---------- Etapa 3: intertarefas (sensor + huffman + logger, mutex) ----------
./bin/intertarefas -m 3 -a 10000 -n $N -s $SEED -o data/etapa3_intertarefas.csv -d data/etapa3_detalhe.csv
# ---------- Etapa 4: sistema (etapa 3 + eventos UDP + carga externa) ----------
stress-ng $STRESS -t 130s -q --metrics-brief > results/stress_ng.txt 2>&1 &
SPID=$!
sleep 3
echo "--- processos durante a etapa 4:" >> results/plataforma.txt
ps -eo pid,comm,%cpu --sort=-%cpu | head -25 >> results/plataforma.txt
./bin/intertarefas -m 4 -a 10000 -n $N -s $SEED -o data/etapa4_sistema.csv -d data/etapa4_detalhe.csv
kill $SPID 2>/dev/null || true
wait $SPID 2>/dev/null || true
echo "stress-ng: $STRESS" >> results/plataforma.txt

# ---------- Experimento A: cobertura + tempo de cada caso de teste ----------
./bin/test_huffman 7 1000 > results/casos_teste.csv
bash scripts/coverage.sh

echo "campanha concluida"
