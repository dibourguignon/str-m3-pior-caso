#!/usr/bin/env bash
# Experimento A: cobertura de linha e de desvio da tarefa Huffman (gcov/gcovr)
# Roda os casos de teste 1..K, de forma incremental, e registra a cobertura
# acumulada de src/huffman.c a cada caso adicionado.
set -e
cd "$(dirname "$0")/.."
make -s coverage
mkdir -p results
TOTAL=7
echo "casos,linha_pct,desvio_pct,linhas_cobertas,linhas_total,desvios_cobertos,desvios_total" > results/cobertura.csv
for K in $(seq 1 $TOTAL); do
    rm -f cov/*.gcda
    ./cov/test_huffman_cov "$K" 1 > /dev/null
    gcovr -r . cov --filter 'src/huffman.c' --object-directory cov --json-summary cov/summary.json > /dev/null
    python3 - "$K" <<'PY' >> results/cobertura.csv
import json, sys
s = json.load(open("cov/summary.json"))
f = s["files"][0]
print(f'{sys.argv[1]},{f["line_percent"]},{f["branch_percent"]},{f["line_covered"]},{f["line_total"]},{f["branch_covered"]},{f["branch_total"]}')
PY
done
# relatorio gcov final (todos os casos) para o anexo
rm -f cov/*.gcda
./cov/test_huffman_cov $TOTAL 1 > /dev/null
gcovr -r . cov --filter 'src/huffman.c' --object-directory cov --txt-metric branch > results/gcovr_final.txt
gcovr -r . cov --filter 'src/huffman.c' --object-directory cov >> results/gcovr_final.txt
(cd cov && gcov -b -c -o . ../src/huffman.c > ../results/gcov_resumo.txt 2>&1 || true)
cat results/cobertura.csv

# Mesma suite com os tamanhos usados no ESP32 (N = 1024, Fibonacci com n = 1024)
mkdir -p cov1024
gcc -O0 --coverage -std=gnu11 -Isrc -DNMAX=1024u -DFIB_N=1024u -o cov1024/test_huffman_cov tests/test_huffman.c src/huffman.c -lpthread
echo "casos,linha_pct,desvio_pct,linhas_cobertas,linhas_total,desvios_cobertos,desvios_total" > results/cobertura_n1024.csv
for K in $(seq 1 $TOTAL); do
    rm -f cov1024/*.gcda
    ./cov1024/test_huffman_cov "$K" 1 > /dev/null
    gcovr -r . cov1024 --filter 'src/huffman.c' --object-directory cov1024 --json-summary cov1024/summary.json > /dev/null
    python3 - "$K" <<'PY' >> results/cobertura_n1024.csv
import json, sys
f = json.load(open("cov1024/summary.json"))["files"][0]
print(f'{sys.argv[1]},{f["line_percent"]},{f["branch_percent"]},{f["line_covered"]},{f["line_total"]},{f["branch_covered"]},{f["branch_total"]}')
PY
done
cat results/cobertura_n1024.csv
